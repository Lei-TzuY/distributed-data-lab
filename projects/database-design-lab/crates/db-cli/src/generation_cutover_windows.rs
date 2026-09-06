use std::path::Path;

use crate::generation_cutover::LegacyCutoverSummary;

pub const LEGACY_CUTOVER_WINDOWS_PROTOCOL: &str = "append_log_legacy_cutover_sentinel_windows_v1";
pub const LEGACY_CUTOVER_SENTINEL_PROTOCOL: &str = "append_log_legacy_cutover_sentinel_v1";

#[cfg(windows)]
pub fn cutover_migrated_legacy_append_log_windows(
    legacy_source: &Path,
    target_directory: &Path,
) -> Result<LegacyCutoverSummary, LegacyCutoverWindowsError> {
    windows::cutover(legacy_source, target_directory)
}

#[cfg(not(windows))]
pub fn cutover_migrated_legacy_append_log_windows(
    legacy_source: &Path,
    target_directory: &Path,
) -> Result<LegacyCutoverSummary, LegacyCutoverWindowsError> {
    let _ = (legacy_source, target_directory);
    Err(LegacyCutoverWindowsError::UnsupportedPlatform)
}

use std::io;
use std::path::PathBuf;

use db_core::DbError;
use thiserror::Error;

use crate::generation_directory::GenerationDirectoryError;
use crate::generation_lock::GenerationWriterLockError;

#[derive(Debug, Error)]
pub enum LegacyCutoverWindowsError {
    #[error(
        "Windows legacy append-log cutover is unsupported on this platform; no filesystem access was performed"
    )]
    UnsupportedPlatform,
    #[error("invalid Windows legacy append-log cutover: {0}")]
    Invalid(String),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error(transparent)]
    Directory(#[from] GenerationDirectoryError),
    #[error(transparent)]
    Lock(#[from] GenerationWriterLockError),
    #[error(transparent)]
    Json(#[from] serde_json::Error),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "legacy source changed before Windows pathname cutover; no cutover sentinel was published"
    )]
    SourceChangedBeforeCutover,
    #[error(
        "retained legacy evidence {retained_path} is visible but its write-through publication could not be confirmed: {source}; preserve both legacy pathname and retained evidence and verify explicitly"
    )]
    RetainedEvidenceDurabilityUncertain {
        retained_path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "Windows cutover replacement at {legacy_path} has an uncertain outcome: {source}; preserve retained evidence {retained_path} and generation target and verify the pathname before retrying"
    )]
    CutoverPublicationUncertain {
        legacy_path: PathBuf,
        retained_path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "legacy pathname {legacy_path} contains the cutover sentinel but retained source {retained_path} no longer matches the pre-cutover snapshot"
    )]
    RetainedSourceChangedAfterCutover {
        legacy_path: PathBuf,
        retained_path: PathBuf,
    },
}

#[cfg(windows)]
mod windows {
    use std::ffi::OsString;
    use std::fs::{self, File, OpenOptions};
    use std::io::{Read, Write};
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    use db_storage_log::{InspectionReport, LogEngine};
    use serde::Serialize;
    use tempfile::NamedTempFile;

    use super::*;
    use crate::generation_directory::{verify_generation_directory, VerifiedGenerationDirectory};
    use crate::generation_lock::acquire_generation_writer_lease;
    use crate::windows_durable::{move_no_replace_write_through, move_replace_write_through};

    const IMPORT_GENERATION: u64 = 1;
    const COMPARE_BUFFER_BYTES: usize = 64 * 1024;
    const RETAINED_SUFFIX: &str = ".retired-append-log-v1";
    const MAX_SENTINEL_PATH_BYTES: usize = 4096;
    static NEXT_STAGING_COUNTER: AtomicU64 = AtomicU64::new(1);

    #[derive(Debug, Serialize)]
    struct Sentinel<'a> {
        protocol: &'static str,
        target_directory: &'a str,
        retained_legacy_path: &'a str,
    }

    pub(super) fn cutover(
        legacy_source: &Path,
        target_directory: &Path,
    ) -> Result<LegacyCutoverSummary, LegacyCutoverWindowsError> {
        let source = canonical_legacy_source(legacy_source)?;
        let source_state = require_clean_source(&source)?;
        let snapshot = capture_snapshot(&source, &source_state)?;
        if !files_equal(&source, snapshot.path())? {
            return Err(LegacyCutoverWindowsError::SourceChangedBeforeCutover);
        }

        let lease = acquire_generation_writer_lease(target_directory)?;
        let before = verify_generation_directory(lease.directory())?;
        validate_fresh_windows_import_target(&source_state, &before)?;
        let target_state = LogEngine::inspect(before.authoritative_log_path(), true)?;
        if target_state.entries != source_state.entries {
            return invalid("target generation 1 does not reproduce the current legacy live state");
        }

        let retained_path = sibling_path(&source, RETAINED_SUFFIX)?;
        ensure_retained_snapshot(&source, snapshot.path(), &retained_path)?;

        let target_display = bounded_path_string(lease.directory(), "target generation directory")?;
        let retained_display = bounded_path_string(&retained_path, "retained legacy source")?;
        let sentinel = serde_json::to_vec_pretty(&Sentinel {
            protocol: LEGACY_CUTOVER_SENTINEL_PROTOCOL,
            target_directory: &target_display,
            retained_legacy_path: &retained_display,
        })?;
        let sentinel_staging = unique_staging_path(&source, "cutover-sentinel")?;
        write_synced_new(&sentinel_staging, &sentinel)?;

        if !source_matches_snapshot(&source, snapshot.path())? {
            let _ = fs::remove_file(&sentinel_staging);
            return Err(LegacyCutoverWindowsError::SourceChangedBeforeCutover);
        }
        let before_replacement = verify_generation_directory(lease.directory())?;
        validate_fresh_windows_import_target(&source_state, &before_replacement)?;
        let before_replacement_state =
            LogEngine::inspect(before_replacement.authoritative_log_path(), true)?;
        if before_replacement_state != target_state {
            let _ = fs::remove_file(&sentinel_staging);
            return invalid("target generation changed while Windows cutover was in progress");
        }

        if let Err(source_error) = move_replace_write_through(&sentinel_staging, &source) {
            if exact_file_bytes(&source, &sentinel).unwrap_or(false) {
                return Err(LegacyCutoverWindowsError::CutoverPublicationUncertain {
                    legacy_path: source,
                    retained_path,
                    source: source_error,
                });
            }
            if source_matches_snapshot(&source, snapshot.path()).unwrap_or(false) {
                let _ = fs::remove_file(&sentinel_staging);
                return Err(io_error(&source, source_error));
            }
            return Err(LegacyCutoverWindowsError::CutoverPublicationUncertain {
                legacy_path: source,
                retained_path,
                source: source_error,
            });
        }

        require_exact_file_bytes(&source, &sentinel, "published Windows cutover sentinel")?;
        if !files_equal(&retained_path, snapshot.path())? {
            return Err(
                LegacyCutoverWindowsError::RetainedSourceChangedAfterCutover {
                    legacy_path: source,
                    retained_path,
                },
            );
        }

        let final_verified = verify_generation_directory(lease.directory())?;
        validate_fresh_windows_import_target(&source_state, &final_verified)?;
        let final_state = LogEngine::inspect(final_verified.authoritative_log_path(), true)?;
        if final_state != target_state {
            return invalid("target generation changed while Windows pathname cutover completed");
        }

        Ok(LegacyCutoverSummary {
            protocol: LEGACY_CUTOVER_WINDOWS_PROTOCOL,
            legacy_path: source.to_string_lossy().into_owned(),
            retained_legacy_path: retained_path.to_string_lossy().into_owned(),
            target_directory: lease.directory().to_string_lossy().into_owned(),
            target_generation: IMPORT_GENERATION,
            source_file_bytes: source_state.verification.file_bytes,
            source_record_count: source_state.verification.record_count,
            live_keys: source_state.verification.live_keys,
            final_generation: final_verified.summary().clone(),
        })
    }

    fn validate_fresh_windows_import_target(
        source: &InspectionReport,
        target: &VerifiedGenerationDirectory,
    ) -> Result<(), LegacyCutoverWindowsError> {
        let summary = target.summary();
        if summary.authoritative_generation != IMPORT_GENERATION
            || summary.highest_observed_generation != IMPORT_GENERATION
            || summary.marker_generation_ids != [IMPORT_GENERATION]
            || summary.reservation_generation_ids != [IMPORT_GENERATION]
            || !summary.staging_marker_generation_ids.is_empty()
            || !summary.uncommitted_generation_ids.is_empty()
        {
            return invalid(
                "target must still be the untouched Windows migration generation 1 with only reservation 1 and marker 1 retained",
            );
        }
        if summary.log_verification != summary.committed_prefix_verification
            || summary.log_verification.recoverable_tail.is_some()
        {
            return invalid(
                "target generation 1 has changed since Windows migration publication; cut over before routing new mutations",
            );
        }
        if summary.log_verification.live_keys != source.verification.live_keys {
            return invalid("target generation 1 live-key count differs from legacy source");
        }
        Ok(())
    }

    fn ensure_retained_snapshot(
        source: &Path,
        snapshot: &Path,
        retained: &Path,
    ) -> Result<(), LegacyCutoverWindowsError> {
        match fs::symlink_metadata(retained) {
            Ok(metadata) => {
                if !metadata.file_type().is_file() {
                    return invalid(format!(
                        "retained legacy source must be a real regular file: {}",
                        retained.display()
                    ));
                }
                if !files_equal(retained, snapshot)? {
                    return invalid(format!(
                        "retained legacy source already exists with different bytes: {}",
                        retained.display()
                    ));
                }
                return Ok(());
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => return Err(io_error(retained, error)),
        }

        let staging = unique_staging_path(source, "retained-copy")?;
        copy_synced_new(snapshot, &staging)?;
        if let Err(source_error) = move_no_replace_write_through(&staging, retained) {
            match fs::symlink_metadata(retained) {
                Ok(_) => {
                    return Err(
                        LegacyCutoverWindowsError::RetainedEvidenceDurabilityUncertain {
                            retained_path: retained.to_path_buf(),
                            source: source_error,
                        },
                    )
                }
                Err(error) if error.kind() == io::ErrorKind::NotFound => {
                    let _ = fs::remove_file(&staging);
                    return Err(io_error(retained, source_error));
                }
                Err(_) => {
                    return Err(
                        LegacyCutoverWindowsError::RetainedEvidenceDurabilityUncertain {
                            retained_path: retained.to_path_buf(),
                            source: source_error,
                        },
                    )
                }
            }
        }
        if !files_equal(retained, snapshot)? {
            return invalid("retained Windows legacy copy differs from the source snapshot");
        }
        Ok(())
    }

    fn canonical_legacy_source(path: &Path) -> Result<PathBuf, LegacyCutoverWindowsError> {
        let metadata = fs::symlink_metadata(path).map_err(|error| io_error(path, error))?;
        if !metadata.file_type().is_file() {
            return invalid(format!(
                "legacy source must be a real regular file rather than a symlink or non-file: {}",
                path.display()
            ));
        }
        fs::canonicalize(path).map_err(|error| io_error(path, error))
    }

    fn require_clean_source(path: &Path) -> Result<InspectionReport, LegacyCutoverWindowsError> {
        let report = LogEngine::inspect(path, true)?;
        if report.verification.recoverable_tail.is_some()
            || report.verification.file_bytes != report.verification.valid_bytes
        {
            return invalid(
                "legacy source must be a complete clean append-log image before Windows pathname cutover",
            );
        }
        Ok(report)
    }

    fn capture_snapshot(
        source: &Path,
        expected: &InspectionReport,
    ) -> Result<NamedTempFile, LegacyCutoverWindowsError> {
        let mut input = File::open(source).map_err(|error| io_error(source, error))?;
        let mut snapshot = NamedTempFile::new().map_err(|error| LegacyCutoverWindowsError::Io {
            path: PathBuf::from("<temporary Windows legacy cutover snapshot>"),
            source: error,
        })?;
        let snapshot_path = snapshot.path().to_path_buf();
        io::copy(&mut input, snapshot.as_file_mut())
            .map_err(|error| io_error(&snapshot_path, error))?;
        snapshot
            .as_file_mut()
            .sync_all()
            .map_err(|error| io_error(&snapshot_path, error))?;
        let captured = LogEngine::inspect(&snapshot_path, true)?;
        if &captured != expected {
            return Err(LegacyCutoverWindowsError::SourceChangedBeforeCutover);
        }
        Ok(snapshot)
    }

    fn sibling_path(source: &Path, suffix: &str) -> Result<PathBuf, LegacyCutoverWindowsError> {
        let name = source.file_name().ok_or_else(|| {
            LegacyCutoverWindowsError::Invalid(format!(
                "legacy source has no final path component: {}",
                source.display()
            ))
        })?;
        let mut sibling = OsString::from(name);
        sibling.push(suffix);
        Ok(source.with_file_name(sibling))
    }

    fn unique_staging_path(
        source: &Path,
        kind: &str,
    ) -> Result<PathBuf, LegacyCutoverWindowsError> {
        let parent = source.parent().ok_or_else(|| {
            LegacyCutoverWindowsError::Invalid(format!(
                "legacy source has no parent directory: {}",
                source.display()
            ))
        })?;
        let name = source.file_name().ok_or_else(|| {
            LegacyCutoverWindowsError::Invalid(format!(
                "legacy source has no final component: {}",
                source.display()
            ))
        })?;
        let pid = std::process::id();
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |duration| duration.as_nanos());
        let counter = NEXT_STAGING_COUNTER.fetch_add(1, Ordering::Relaxed);
        let mut staging = OsString::from(".");
        staging.push(name);
        staging.push(format!(".{kind}-{pid}-{nanos:x}-{counter:016x}.staging"));
        Ok(parent.join(staging))
    }

    fn bounded_path_string(path: &Path, label: &str) -> Result<String, LegacyCutoverWindowsError> {
        let value = path.to_string_lossy().into_owned();
        if value.len() > MAX_SENTINEL_PATH_BYTES {
            return invalid(format!(
                "{label} path exceeds {MAX_SENTINEL_PATH_BYTES} encoded bytes"
            ));
        }
        Ok(value)
    }

    fn write_synced_new(path: &Path, bytes: &[u8]) -> Result<(), LegacyCutoverWindowsError> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
            .map_err(|error| io_error(path, error))?;
        file.write_all(bytes)
            .map_err(|error| io_error(path, error))?;
        file.sync_all().map_err(|error| io_error(path, error))
    }

    fn copy_synced_new(source: &Path, target: &Path) -> Result<(), LegacyCutoverWindowsError> {
        let mut input = File::open(source).map_err(|error| io_error(source, error))?;
        let mut output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(target)
            .map_err(|error| io_error(target, error))?;
        io::copy(&mut input, &mut output).map_err(|error| io_error(target, error))?;
        output.sync_all().map_err(|error| io_error(target, error))
    }

    fn source_matches_snapshot(
        source: &Path,
        snapshot: &Path,
    ) -> Result<bool, LegacyCutoverWindowsError> {
        let metadata = match fs::symlink_metadata(source) {
            Ok(metadata) => metadata,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
            Err(error) => return Err(io_error(source, error)),
        };
        if !metadata.file_type().is_file() {
            return Ok(false);
        }
        files_equal(source, snapshot)
    }

    fn files_equal(left: &Path, right: &Path) -> Result<bool, LegacyCutoverWindowsError> {
        let left_meta = fs::metadata(left).map_err(|error| io_error(left, error))?;
        let right_meta = fs::metadata(right).map_err(|error| io_error(right, error))?;
        if left_meta.len() != right_meta.len() {
            return Ok(false);
        }
        let mut left_file = File::open(left).map_err(|error| io_error(left, error))?;
        let mut right_file = File::open(right).map_err(|error| io_error(right, error))?;
        let mut left_buffer = [0_u8; COMPARE_BUFFER_BYTES];
        let mut right_buffer = [0_u8; COMPARE_BUFFER_BYTES];
        loop {
            let left_read = left_file
                .read(&mut left_buffer)
                .map_err(|error| io_error(left, error))?;
            let right_read = right_file
                .read(&mut right_buffer)
                .map_err(|error| io_error(right, error))?;
            if left_read != right_read || left_buffer[..left_read] != right_buffer[..right_read] {
                return Ok(false);
            }
            if left_read == 0 {
                return Ok(true);
            }
        }
    }

    fn exact_file_bytes(path: &Path, expected: &[u8]) -> Result<bool, LegacyCutoverWindowsError> {
        match fs::read(path) {
            Ok(bytes) => Ok(bytes == expected),
            Err(error) => Err(io_error(path, error)),
        }
    }

    fn require_exact_file_bytes(
        path: &Path,
        expected: &[u8],
        label: &str,
    ) -> Result<(), LegacyCutoverWindowsError> {
        if !exact_file_bytes(path, expected)? {
            return invalid(format!("{label} bytes differ from the staged sentinel"));
        }
        Ok(())
    }

    fn io_error(path: &Path, source: io::Error) -> LegacyCutoverWindowsError {
        LegacyCutoverWindowsError::Io {
            path: path.to_path_buf(),
            source,
        }
    }

    fn invalid<T>(message: impl Into<String>) -> Result<T, LegacyCutoverWindowsError> {
        Err(LegacyCutoverWindowsError::Invalid(message.into()))
    }
}
