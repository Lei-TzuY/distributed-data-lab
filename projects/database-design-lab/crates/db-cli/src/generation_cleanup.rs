use std::io;
use std::path::{Path, PathBuf};

use serde::Serialize;
use thiserror::Error;

use crate::generation_directory::{
    scan_generation_namespace, verify_generation_directory, GenerationDirectoryError,
    GenerationVerificationSummary,
};
use crate::generation_lock::{acquire_generation_writer_lease, GenerationWriterLockError};

pub const GENERATION_CLEANUP_PROTOCOL: &str = "append_log_generation_cleanup_unix_v1";

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct GenerationCleanupSummary {
    pub protocol: &'static str,
    pub authoritative_generation: u64,
    pub removed_marker_generation_ids: Vec<u64>,
    pub removed_generation_ids: Vec<u64>,
    pub removed_staging_marker_generation_ids: Vec<u64>,
    pub retained_staging_marker_generation_ids: Vec<u64>,
    pub retained_uncommitted_generation_ids: Vec<u64>,
    pub final_generation: GenerationVerificationSummary,
}

#[derive(Debug, Error)]
pub enum GenerationCleanupError {
    #[error("append-log generation cleanup is unsupported on this platform; no retained artifact was removed")]
    UnsupportedPlatform,
    #[error(transparent)]
    Lock(#[from] GenerationWriterLockError),
    #[error(transparent)]
    Directory(#[from] GenerationDirectoryError),
    #[error("invalid generation cleanup state: {0}")]
    Invalid(String),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "generation cleanup changed or observed drift in authoritative generation {generation} during {stage}; cleanup stopped fail-closed"
    )]
    AuthorityChanged {
        generation: u64,
        stage: &'static str,
    },
    #[error(
        "generation cleanup removed {phase} names but parent-directory durability could not be confirmed at {directory}: {source}"
    )]
    DurabilityUncertain {
        phase: &'static str,
        directory: PathBuf,
        #[source]
        source: io::Error,
    },
}

/// Removes only retained history whose generation ids remain below the durable allocation frontier.
///
/// On Unix the operation holds the cooperative generation writer lease, verifies the current
/// highest committed generation, removes every lower final marker plus staging markers at or below
/// current authority, synchronizes the directory, re-verifies authority, then removes every lower
/// generation log and synchronizes again. Higher staging markers and higher uncommitted generation
/// logs are deliberately retained because their ids are allocation-frontier evidence; a compact
/// candidate may also still be under construction outside the lease-protected publication section.
///
/// Non-Unix targets fail before filesystem access because this protocol does not claim an
/// equivalent parent-directory deletion durability barrier there.
pub fn cleanup_obsolete_generations(
    directory: &Path,
) -> Result<GenerationCleanupSummary, GenerationCleanupError> {
    #[cfg(unix)]
    {
        cleanup_obsolete_generations_unix(directory)
    }

    #[cfg(not(unix))]
    {
        let _ = directory;
        Err(GenerationCleanupError::UnsupportedPlatform)
    }
}

#[cfg(unix)]
fn cleanup_obsolete_generations_unix(
    directory: &Path,
) -> Result<GenerationCleanupSummary, GenerationCleanupError> {
    use std::fs::{self, File};

    let lease = acquire_generation_writer_lease(directory)?;
    let before = verify_generation_directory(lease.directory())?;
    let witness = AuthorityWitness::from_summary(before.summary());
    let authoritative_generation = witness.generation;
    let namespace = scan_generation_namespace(lease.directory())?;

    let removed_marker_generation_ids: Vec<u64> = namespace
        .marker_files
        .keys()
        .copied()
        .filter(|id| *id < authoritative_generation)
        .collect();
    let removed_generation_ids: Vec<u64> = namespace
        .generation_files
        .keys()
        .copied()
        .filter(|id| *id < authoritative_generation)
        .collect();
    let removed_staging_marker_generation_ids: Vec<u64> = namespace
        .staging_marker_files
        .keys()
        .copied()
        .filter(|id| *id <= authoritative_generation)
        .collect();

    // Validate the complete deletion plan before removing the first directory entry.
    for id in &removed_marker_generation_ids {
        let path = namespace
            .marker_files
            .get(id)
            .expect("planned marker id came from namespace");
        require_real_regular_file(path, "obsolete commit marker")?;
    }
    for id in &removed_staging_marker_generation_ids {
        let path = namespace
            .staging_marker_files
            .get(id)
            .expect("planned staging id came from namespace");
        require_real_regular_file(path, "obsolete staging commit marker")?;
    }
    for id in &removed_generation_ids {
        let path = namespace
            .generation_files
            .get(id)
            .expect("planned generation id came from namespace");
        require_real_regular_file(path, "obsolete generation log")?;
    }

    require_same_authority(lease.directory(), &witness, "pre-delete verification")?;

    for id in &removed_marker_generation_ids {
        let path = namespace
            .marker_files
            .get(id)
            .expect("planned marker id came from namespace");
        fs::remove_file(path).map_err(|source| io_error(path, source))?;
    }
    for id in &removed_staging_marker_generation_ids {
        let path = namespace
            .staging_marker_files
            .get(id)
            .expect("planned staging id came from namespace");
        fs::remove_file(path).map_err(|source| io_error(path, source))?;
    }
    if !removed_marker_generation_ids.is_empty()
        || !removed_staging_marker_generation_ids.is_empty()
    {
        sync_directory(lease.directory()).map_err(|source| {
            GenerationCleanupError::DurabilityUncertain {
                phase: "obsolete marker/staging cleanup",
                directory: lease.directory().to_path_buf(),
                source,
            }
        })?;
    }

    require_same_authority(
        lease.directory(),
        &witness,
        "post-marker pre-generation verification",
    )?;

    for id in &removed_generation_ids {
        let path = namespace
            .generation_files
            .get(id)
            .expect("planned generation id came from namespace");
        fs::remove_file(path).map_err(|source| io_error(path, source))?;
    }
    if !removed_generation_ids.is_empty() {
        sync_directory(lease.directory()).map_err(|source| {
            GenerationCleanupError::DurabilityUncertain {
                phase: "obsolete generation cleanup",
                directory: lease.directory().to_path_buf(),
                source,
            }
        })?;
    }

    let final_verified = require_same_authority(
        lease.directory(),
        &witness,
        "final post-cleanup verification",
    )?;
    let final_namespace = scan_generation_namespace(lease.directory())?;
    let retained_staging_marker_generation_ids = final_namespace
        .staging_marker_files
        .keys()
        .copied()
        .collect();
    let retained_uncommitted_generation_ids = final_namespace
        .generation_files
        .keys()
        .filter(|id| !final_namespace.marker_files.contains_key(*id))
        .copied()
        .collect();

    fn sync_directory(path: &Path) -> io::Result<()> {
        File::open(path)?.sync_all()
    }

    Ok(GenerationCleanupSummary {
        protocol: GENERATION_CLEANUP_PROTOCOL,
        authoritative_generation,
        removed_marker_generation_ids,
        removed_generation_ids,
        removed_staging_marker_generation_ids,
        retained_staging_marker_generation_ids,
        retained_uncommitted_generation_ids,
        final_generation: final_verified,
    })
}

#[cfg(unix)]
#[derive(Debug, Clone, PartialEq, Eq)]
struct AuthorityWitness {
    generation: u64,
    authoritative_log: String,
    committed_prefix: crate::generation_marker::CommittedPrefix,
    log_verification: db_storage_log::VerificationReport,
}

#[cfg(unix)]
impl AuthorityWitness {
    fn from_summary(summary: &GenerationVerificationSummary) -> Self {
        Self {
            generation: summary.authoritative_generation,
            authoritative_log: summary.authoritative_log.clone(),
            committed_prefix: summary.committed_prefix,
            log_verification: summary.log_verification.clone(),
        }
    }
}

#[cfg(unix)]
fn require_same_authority(
    directory: &Path,
    expected: &AuthorityWitness,
    stage: &'static str,
) -> Result<GenerationVerificationSummary, GenerationCleanupError> {
    let current = verify_generation_directory(directory)?;
    let observed = AuthorityWitness::from_summary(current.summary());
    if &observed != expected {
        return Err(GenerationCleanupError::AuthorityChanged {
            generation: expected.generation,
            stage,
        });
    }
    Ok(current.summary().clone())
}

#[cfg(unix)]
fn require_real_regular_file(path: &Path, label: &str) -> Result<(), GenerationCleanupError> {
    let metadata = std::fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_file() {
        return Err(GenerationCleanupError::Invalid(format!(
            "{label} must be a real regular file rather than a symlink or non-file: {}",
            path.display()
        )));
    }
    Ok(())
}

#[cfg(unix)]
fn io_error(path: &Path, source: io::Error) -> GenerationCleanupError {
    GenerationCleanupError::Io {
        path: path.to_path_buf(),
        source,
    }
}
