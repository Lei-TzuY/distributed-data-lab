use std::io;
use std::path::{Path, PathBuf};
#[cfg(windows)]
use std::sync::atomic::{AtomicU64, Ordering};
#[cfg(windows)]
use std::time::{SystemTime, UNIX_EPOCH};

use serde::Serialize;
use thiserror::Error;

use crate::generation_directory::GenerationDirectoryError;
#[cfg(any(unix, windows))]
use crate::generation_directory::{canonical_reservation_name, verify_generation_directory};
#[cfg(any(unix, windows))]
use crate::generation_lock::acquire_generation_writer_lease;
use crate::generation_lock::GenerationWriterLockError;
#[cfg(windows)]
use crate::windows_durable::move_no_replace_write_through;

pub const GENERATION_RESERVATION_PROTOCOL: &str = "append_log_generation_reservation_unix_v1";
pub const GENERATION_RESERVATION_WINDOWS_PROTOCOL: &str =
    "append_log_generation_reservation_windows_v1";

#[cfg(windows)]
static NEXT_WINDOWS_STAGING_COUNTER: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct GenerationReservationSummary {
    pub protocol: &'static str,
    pub generation: u64,
    pub reservation: String,
    pub authoritative_generation: u64,
    pub highest_observed_generation: u64,
}

#[derive(Debug, Error)]
pub enum GenerationReservationError {
    #[error("append-log generation reservation is unsupported on this platform; no reservation was written")]
    UnsupportedPlatform,
    #[error(transparent)]
    Lock(#[from] GenerationWriterLockError),
    #[error(transparent)]
    Directory(#[from] GenerationDirectoryError),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "generation reservation {generation} is visible but parent-directory durability could not be confirmed at {directory}: {source}"
    )]
    DurabilityUncertain {
        generation: u64,
        directory: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("generation reservation {generation} was not retained by post-write verification")]
    NotRetained { generation: u64 },
}

/// Durably reserves the next generation id before a candidate file is constructed.
///
/// Unix synchronizes a create-new reservation and then the generation directory. Windows writes and
/// synchronizes a unique sibling staging file, then publishes the canonical reservation name with
/// the audited no-overwrite `MOVEFILE_WRITE_THROUGH` primitive. Both paths hold the shared writer
/// lease across allocation and retained-state verification.
pub fn reserve_next_generation(
    directory: &Path,
) -> Result<GenerationReservationSummary, GenerationReservationError> {
    #[cfg(unix)]
    {
        reserve_next_generation_unix(directory)
    }

    #[cfg(windows)]
    {
        reserve_next_generation_windows(directory)
    }

    #[cfg(not(any(unix, windows)))]
    {
        let _ = directory;
        Err(GenerationReservationError::UnsupportedPlatform)
    }
}

#[cfg(unix)]
fn reserve_next_generation_unix(
    directory: &Path,
) -> Result<GenerationReservationSummary, GenerationReservationError> {
    use std::fs::{File, OpenOptions};

    let lease = acquire_generation_writer_lease(directory)?;
    let before = verify_generation_directory(lease.directory())?;
    let generation = before.next_generation_id()?;
    let reservation = canonical_reservation_name(generation);
    let reservation_path = lease.directory().join(&reservation);

    let file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&reservation_path)
        .map_err(|source| io_error(&reservation_path, source))?;
    file.sync_all()
        .map_err(|source| io_error(&reservation_path, source))?;

    if let Err(source) = File::open(lease.directory()).and_then(|directory| directory.sync_all()) {
        return Err(GenerationReservationError::DurabilityUncertain {
            generation,
            directory: lease.directory().to_path_buf(),
            source,
        });
    }

    retained_summary(
        lease.directory(),
        generation,
        reservation,
        GENERATION_RESERVATION_PROTOCOL,
    )
}

#[cfg(windows)]
fn reserve_next_generation_windows(
    directory: &Path,
) -> Result<GenerationReservationSummary, GenerationReservationError> {
    let lease = acquire_generation_writer_lease(directory)?;
    let before = verify_generation_directory(lease.directory())?;
    let generation = before.next_generation_id()?;
    let reservation = publish_generation_reservation_windows(lease.directory(), generation)?;

    retained_summary(
        lease.directory(),
        generation,
        reservation,
        GENERATION_RESERVATION_WINDOWS_PROTOCOL,
    )
}

/// Publishes one caller-selected durable reservation on Windows.
///
/// This is an internal retained-entry primitive for bootstrap flows such as legacy migration. The
/// caller is responsible for proving that `generation` is the correct fresh id and for holding any
/// required writer lease. Normal allocation must continue to use [`reserve_next_generation`].
#[cfg(windows)]
pub(crate) fn publish_generation_reservation_windows(
    directory: &Path,
    generation: u64,
) -> Result<String, GenerationReservationError> {
    use std::fs::{self, OpenOptions};

    if generation == 0 {
        return Err(GenerationReservationError::Directory(
            GenerationDirectoryError::Invalid(
                "generation reservation id must be greater than zero".to_owned(),
            ),
        ));
    }

    let reservation = canonical_reservation_name(generation);
    let reservation_path = directory.join(&reservation);
    match fs::symlink_metadata(&reservation_path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Ok(_) => {
            return Err(GenerationReservationError::Directory(
                GenerationDirectoryError::Invalid(format!(
                    "generation reservation already exists: {}",
                    reservation_path.display()
                )),
            ))
        }
        Err(source) => return Err(io_error(&reservation_path, source)),
    }

    let staging_path = windows_staging_path(directory, generation)?;
    let staging = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&staging_path)
        .map_err(|source| io_error(&staging_path, source))?;
    staging
        .sync_all()
        .map_err(|source| io_error(&staging_path, source))?;
    drop(staging);

    if let Err(source) = move_no_replace_write_through(&staging_path, &reservation_path) {
        match fs::symlink_metadata(&reservation_path) {
            Ok(_) => {
                return Err(GenerationReservationError::DurabilityUncertain {
                    generation,
                    directory: directory.to_path_buf(),
                    source,
                });
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                let _ = fs::remove_file(&staging_path);
                return Err(io_error(&reservation_path, source));
            }
            Err(_) => {
                return Err(GenerationReservationError::DurabilityUncertain {
                    generation,
                    directory: directory.to_path_buf(),
                    source,
                });
            }
        }
    }

    Ok(reservation)
}

#[cfg(windows)]
fn windows_staging_path(
    directory: &Path,
    generation: u64,
) -> Result<PathBuf, GenerationReservationError> {
    let parent = directory.parent().ok_or_else(|| {
        GenerationReservationError::Directory(GenerationDirectoryError::Invalid(
            "Windows reservation directory has no parent for same-volume staging".to_owned(),
        ))
    })?;
    let pid = std::process::id();
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| duration.as_nanos());
    let counter = NEXT_WINDOWS_STAGING_COUNTER.fetch_add(1, Ordering::Relaxed);
    Ok(parent.join(format!(
        ".append-log-reserve-{generation:020}-{pid}-{nanos:x}-{counter:016x}.staging"
    )))
}

#[cfg(any(unix, windows))]
fn retained_summary(
    directory: &Path,
    generation: u64,
    reservation: String,
    protocol: &'static str,
) -> Result<GenerationReservationSummary, GenerationReservationError> {
    let after = verify_generation_directory(directory)?;
    if !after
        .summary()
        .reservation_generation_ids
        .contains(&generation)
        || after.summary().highest_observed_generation < generation
    {
        return Err(GenerationReservationError::NotRetained { generation });
    }

    Ok(GenerationReservationSummary {
        protocol,
        generation,
        reservation,
        authoritative_generation: after.summary().authoritative_generation,
        highest_observed_generation: after.summary().highest_observed_generation,
    })
}

#[cfg(any(unix, windows))]
fn io_error(path: &Path, source: io::Error) -> GenerationReservationError {
    GenerationReservationError::Io {
        path: path.to_path_buf(),
        source,
    }
}
