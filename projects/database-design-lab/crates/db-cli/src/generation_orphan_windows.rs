use std::io;
use std::path::{Path, PathBuf};

#[cfg(windows)]
use std::ffi::OsString;
#[cfg(windows)]
use std::fs::{self, File};
#[cfg(windows)]
use std::io::Read;

use serde::Serialize;
use thiserror::Error;

#[cfg(windows)]
use crate::generation_directory::{
    canonical_generation_name, canonical_reservation_name, canonical_staging_marker_name,
    scan_generation_namespace, verify_generation_directory,
};
use crate::generation_directory::{GenerationDirectoryError, GenerationVerificationSummary};
#[cfg(windows)]
use crate::generation_lock::acquire_generation_writer_lease;
use crate::generation_lock::GenerationWriterLockError;
#[cfg(windows)]
use crate::generation_marker::Crc32Ieee;
#[cfg(windows)]
use crate::generation_orphan::inspect_generation_orphan;
use crate::generation_orphan::{GenerationFileFingerprint, GenerationOrphanError};
#[cfg(windows)]
use crate::windows_durable::move_no_replace_write_through;

pub const GENERATION_ORPHAN_RETIRE_WINDOWS_PROTOCOL: &str =
    "append_log_generation_orphan_retire_windows_v1";
#[cfg(windows)]
const FINGERPRINT_BUFFER_BYTES: usize = 64 * 1024;

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct WindowsGenerationOrphanRetirementSummary {
    pub protocol: &'static str,
    pub authoritative_generation: u64,
    pub retired_generation: u64,
    pub retired_orphan_fingerprint: GenerationFileFingerprint,
    pub retired_staging_fingerprint: Option<GenerationFileFingerprint>,
    pub reservation: String,
    pub orphan_quarantine: PathBuf,
    pub staging_quarantine: Option<PathBuf>,
    pub final_generation: GenerationVerificationSummary,
}

#[derive(Debug, Error)]
pub enum WindowsGenerationOrphanRetirementError {
    #[error("Windows append-log generation orphan retirement is unsupported on this platform; no filesystem access was performed")]
    UnsupportedPlatform,
    #[error("generation orphan retirement requires --confirm-generation-builder-stopped")]
    ConfirmationRequired,
    #[error(transparent)]
    Lock(#[from] GenerationWriterLockError),
    #[error(transparent)]
    Directory(#[from] GenerationDirectoryError),
    #[error(transparent)]
    Orphan(#[from] GenerationOrphanError),
    #[error("invalid Windows generation orphan retirement state: {0}")]
    Invalid(String),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "Windows orphan retirement may have moved {phase} from {source_path} to {target_path} even though write-through publication reported an error: {source}; preserve both paths and re-inspect before retrying"
    )]
    RetirementUncertain {
        phase: &'static str,
        source_path: PathBuf,
        target_path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "expected authoritative generation {expected}, found {found}; Windows orphan retirement stopped fail-closed"
    )]
    AuthorityChanged { expected: u64, found: u64 },
    #[error("inspected orphan or staging evidence changed before Windows retirement")]
    InspectionChanged,
}

pub fn retire_generation_orphan_windows(
    directory: &Path,
    generation: u64,
    expected_authority: u64,
    expected_orphan_fingerprint: GenerationFileFingerprint,
    expected_staging_fingerprint: Option<GenerationFileFingerprint>,
    confirm_generation_builder_stopped: bool,
) -> Result<WindowsGenerationOrphanRetirementSummary, WindowsGenerationOrphanRetirementError> {
    if !confirm_generation_builder_stopped {
        return Err(WindowsGenerationOrphanRetirementError::ConfirmationRequired);
    }

    #[cfg(windows)]
    {
        retire_windows(
            directory,
            generation,
            expected_authority,
            expected_orphan_fingerprint,
            expected_staging_fingerprint,
        )
    }

    #[cfg(not(windows))]
    {
        let _ = (
            directory,
            generation,
            expected_authority,
            expected_orphan_fingerprint,
            expected_staging_fingerprint,
        );
        Err(WindowsGenerationOrphanRetirementError::UnsupportedPlatform)
    }
}

#[cfg(windows)]
fn retire_windows(
    directory: &Path,
    generation: u64,
    expected_authority: u64,
    expected_orphan_fingerprint: GenerationFileFingerprint,
    expected_staging_fingerprint: Option<GenerationFileFingerprint>,
) -> Result<WindowsGenerationOrphanRetirementSummary, WindowsGenerationOrphanRetirementError> {
    let lease = acquire_generation_writer_lease(directory)?;
    let inspected = inspect_generation_orphan(lease.directory(), generation)?;
    require_expected_inspection(
        &inspected,
        expected_authority,
        expected_orphan_fingerprint,
        expected_staging_fingerprint,
    )?;

    let orphan_path = lease
        .directory()
        .join(canonical_generation_name(generation));
    let staging_path = lease
        .directory()
        .join(canonical_staging_marker_name(generation));
    let orphan_quarantine = quarantine_path(lease.directory(), generation, "generation", "log")?;
    let staging_quarantine = if expected_staging_fingerprint.is_some() {
        Some(quarantine_path(
            lease.directory(),
            generation,
            "staging-commit",
            "marker",
        )?)
    } else {
        None
    };

    require_absent(&orphan_quarantine, "orphan quarantine target")?;
    if let Some(path) = &staging_quarantine {
        require_absent(path, "staging quarantine target")?;
    }

    require_authority(lease.directory(), expected_authority)?;
    if let (Some(expected), Some(target)) = (expected_staging_fingerprint, &staging_quarantine) {
        move_checked(&staging_path, target, "abandoned staging marker")?;
        require_fingerprint(target, expected)?;
        require_absent_source(&staging_path, "staging marker")?;
        require_authority(lease.directory(), expected_authority)?;
        require_fingerprint(&orphan_path, expected_orphan_fingerprint)?;
    }

    move_checked(
        &orphan_path,
        &orphan_quarantine,
        "abandoned generation candidate",
    )?;
    require_fingerprint(&orphan_quarantine, expected_orphan_fingerprint)?;
    require_absent_source(&orphan_path, "generation candidate")?;

    let final_verified = require_authority(lease.directory(), expected_authority)?;
    let final_namespace = scan_generation_namespace(lease.directory())?;
    if final_namespace.generation_files.contains_key(&generation)
        || final_namespace
            .staging_marker_files
            .contains_key(&generation)
    {
        return invalid(format!(
            "retired generation {generation} still appears in the authoritative namespace"
        ));
    }
    if !final_namespace.reservation_files.contains_key(&generation) {
        return invalid(format!(
            "retirement lost durable reservation for generation {generation}"
        ));
    }
    if final_verified.summary().highest_observed_generation < generation {
        return invalid(format!(
            "retirement lost allocation frontier {generation}; highest observed generation is {}",
            final_verified.summary().highest_observed_generation
        ));
    }

    Ok(WindowsGenerationOrphanRetirementSummary {
        protocol: GENERATION_ORPHAN_RETIRE_WINDOWS_PROTOCOL,
        authoritative_generation: expected_authority,
        retired_generation: generation,
        retired_orphan_fingerprint: expected_orphan_fingerprint,
        retired_staging_fingerprint: expected_staging_fingerprint,
        reservation: canonical_reservation_name(generation),
        orphan_quarantine,
        staging_quarantine,
        final_generation: final_verified.summary().clone(),
    })
}

#[cfg(windows)]
fn require_expected_inspection(
    inspected: &crate::generation_orphan::GenerationOrphanInspection,
    expected_authority: u64,
    expected_orphan_fingerprint: GenerationFileFingerprint,
    expected_staging_fingerprint: Option<GenerationFileFingerprint>,
) -> Result<(), WindowsGenerationOrphanRetirementError> {
    if inspected.authoritative_generation != expected_authority
        || inspected.orphan_fingerprint != expected_orphan_fingerprint
        || inspected.staging_fingerprint != expected_staging_fingerprint
    {
        return Err(WindowsGenerationOrphanRetirementError::InspectionChanged);
    }
    Ok(())
}

#[cfg(windows)]
fn require_authority(
    directory: &Path,
    expected: u64,
) -> Result<
    crate::generation_directory::VerifiedGenerationDirectory,
    WindowsGenerationOrphanRetirementError,
> {
    let verified = verify_generation_directory(directory)?;
    let found = verified.summary().authoritative_generation;
    if found != expected {
        return Err(WindowsGenerationOrphanRetirementError::AuthorityChanged { expected, found });
    }
    Ok(verified)
}

#[cfg(windows)]
fn quarantine_path(
    directory: &Path,
    generation: u64,
    kind: &str,
    extension: &str,
) -> Result<PathBuf, WindowsGenerationOrphanRetirementError> {
    let parent = directory.parent().ok_or_else(|| {
        WindowsGenerationOrphanRetirementError::Invalid(
            "generation directory has no parent for sibling retirement evidence".to_owned(),
        )
    })?;
    let base = directory.file_name().ok_or_else(|| {
        WindowsGenerationOrphanRetirementError::Invalid(
            "generation directory has no final path component".to_owned(),
        )
    })?;
    let mut name = OsString::from(".");
    name.push(base);
    name.push(format!(".retired-{kind}-{generation:020}.{extension}"));
    Ok(parent.join(name))
}

#[cfg(windows)]
fn move_checked(
    source_path: &Path,
    target_path: &Path,
    phase: &'static str,
) -> Result<(), WindowsGenerationOrphanRetirementError> {
    match move_no_replace_write_through(source_path, target_path) {
        Ok(()) => Ok(()),
        Err(source) => {
            let source_exists = fs::symlink_metadata(source_path).is_ok();
            let target_exists = fs::symlink_metadata(target_path).is_ok();
            if source_exists && !target_exists {
                Err(io_error(source_path, source))
            } else {
                Err(
                    WindowsGenerationOrphanRetirementError::RetirementUncertain {
                        phase,
                        source_path: source_path.to_path_buf(),
                        target_path: target_path.to_path_buf(),
                        source,
                    },
                )
            }
        }
    }
}

#[cfg(windows)]
fn require_absent(path: &Path, label: &str) -> Result<(), WindowsGenerationOrphanRetirementError> {
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Ok(_) => invalid(format!("{label} already exists: {}", path.display())),
        Err(source) => Err(io_error(path, source)),
    }
}

#[cfg(windows)]
fn require_absent_source(
    path: &Path,
    label: &str,
) -> Result<(), WindowsGenerationOrphanRetirementError> {
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Ok(_) => invalid(format!(
            "{label} still exists after write-through retirement"
        )),
        Err(source) => Err(io_error(path, source)),
    }
}

#[cfg(windows)]
fn fingerprint_file(
    path: &Path,
) -> Result<GenerationFileFingerprint, WindowsGenerationOrphanRetirementError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_file() {
        return invalid(format!(
            "retirement evidence must be a real regular file: {}",
            path.display()
        ));
    }
    let mut file = File::open(path).map_err(|source| io_error(path, source))?;
    let mut buffer = [0_u8; FINGERPRINT_BUFFER_BYTES];
    let mut bytes = 0_u64;
    let mut hasher = Crc32Ieee::new();
    loop {
        let read = file
            .read(&mut buffer)
            .map_err(|source| io_error(path, source))?;
        if read == 0 {
            break;
        }
        bytes = bytes.checked_add(read as u64).ok_or_else(|| {
            WindowsGenerationOrphanRetirementError::Invalid(
                "retirement evidence byte count overflowed u64".to_owned(),
            )
        })?;
        hasher.update(&buffer[..read]);
    }
    Ok(GenerationFileFingerprint {
        bytes,
        crc32: hasher.finalize(),
    })
}

#[cfg(windows)]
fn require_fingerprint(
    path: &Path,
    expected: GenerationFileFingerprint,
) -> Result<(), WindowsGenerationOrphanRetirementError> {
    let found = fingerprint_file(path)?;
    if found != expected {
        return invalid(format!(
            "retirement evidence fingerprint changed: expected bytes={} crc32={:08x}, found bytes={} crc32={:08x}",
            expected.bytes, expected.crc32, found.bytes, found.crc32
        ));
    }
    Ok(())
}

#[cfg(windows)]
fn io_error(path: &Path, source: io::Error) -> WindowsGenerationOrphanRetirementError {
    WindowsGenerationOrphanRetirementError::Io {
        path: path.to_path_buf(),
        source,
    }
}

#[cfg(windows)]
fn invalid<T>(message: impl Into<String>) -> Result<T, WindowsGenerationOrphanRetirementError> {
    Err(WindowsGenerationOrphanRetirementError::Invalid(
        message.into(),
    ))
}
