use std::io;
use std::path::{Path, PathBuf};

#[cfg(windows)]
use std::ffi::OsString;
#[cfg(windows)]
use std::fs;

use serde::Serialize;
use thiserror::Error;

#[cfg(windows)]
use crate::generation_directory::{
    require_real_regular_file, scan_generation_namespace, verify_generation_directory,
};
use crate::generation_directory::{GenerationDirectoryError, GenerationVerificationSummary};
#[cfg(windows)]
use crate::generation_lock::acquire_generation_writer_lease;
use crate::generation_lock::GenerationWriterLockError;
#[cfg(windows)]
use crate::windows_durable::move_no_replace_write_through;

pub const GENERATION_CLEANUP_WINDOWS_PROTOCOL: &str = "append_log_generation_cleanup_windows_v1";

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct WindowsGenerationCleanupSummary {
    pub protocol: &'static str,
    pub authoritative_generation: u64,
    pub retired_marker_generation_ids: Vec<u64>,
    pub retired_generation_ids: Vec<u64>,
    pub retired_staging_marker_generation_ids: Vec<u64>,
    pub retained_staging_marker_generation_ids: Vec<u64>,
    pub retained_uncommitted_generation_ids: Vec<u64>,
    pub final_generation: GenerationVerificationSummary,
}

#[derive(Debug, Error)]
pub enum WindowsGenerationCleanupError {
    #[error("Windows append-log generation cleanup is unsupported on this platform; no filesystem access was performed")]
    UnsupportedPlatform,
    #[error(transparent)]
    Lock(#[from] GenerationWriterLockError),
    #[error(transparent)]
    Directory(#[from] GenerationDirectoryError),
    #[error("invalid Windows generation cleanup state: {0}")]
    Invalid(String),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "Windows generation cleanup may have moved {phase} from {source_path} to {target_path} even though write-through publication reported an error: {source}; preserve both paths and re-verify before retrying"
    )]
    RetirementUncertain {
        phase: &'static str,
        source_path: PathBuf,
        target_path: PathBuf,
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
}

/// Retires obsolete committed history from the strict generation namespace on Windows.
///
/// Retired bytes are moved to deterministic sibling quarantine names with the repository's audited
/// no-overwrite `MOVEFILE_WRITE_THROUGH` primitive. They are intentionally retained rather than
/// physically deleted because the project does not yet claim durable Windows delete-entry semantics.
pub fn cleanup_obsolete_generations_windows(
    directory: &Path,
) -> Result<WindowsGenerationCleanupSummary, WindowsGenerationCleanupError> {
    #[cfg(windows)]
    {
        cleanup_windows(directory)
    }

    #[cfg(not(windows))]
    {
        let _ = directory;
        Err(WindowsGenerationCleanupError::UnsupportedPlatform)
    }
}

#[cfg(windows)]
fn cleanup_windows(
    directory: &Path,
) -> Result<WindowsGenerationCleanupSummary, WindowsGenerationCleanupError> {
    let lease = acquire_generation_writer_lease(directory)?;
    let before = verify_generation_directory(lease.directory())?;
    let witness = AuthorityWitness::from_summary(before.summary());
    let authoritative_generation = witness.generation;
    let namespace = scan_generation_namespace(lease.directory())?;

    let retired_marker_generation_ids: Vec<u64> = namespace
        .marker_files
        .keys()
        .copied()
        .filter(|id| *id < authoritative_generation)
        .collect();
    let retired_generation_ids: Vec<u64> = namespace
        .generation_files
        .keys()
        .copied()
        .filter(|id| *id < authoritative_generation)
        .collect();
    let retired_staging_marker_generation_ids: Vec<u64> = namespace
        .staging_marker_files
        .keys()
        .copied()
        .filter(|id| *id <= authoritative_generation)
        .collect();

    let marker_moves = build_moves(
        lease.directory(),
        &namespace.marker_files,
        &retired_marker_generation_ids,
        "commit",
        "marker",
        "obsolete commit marker",
    )?;
    let staging_moves = build_moves(
        lease.directory(),
        &namespace.staging_marker_files,
        &retired_staging_marker_generation_ids,
        "staging-commit",
        "marker",
        "obsolete staging commit marker",
    )?;
    let generation_moves = build_moves(
        lease.directory(),
        &namespace.generation_files,
        &retired_generation_ids,
        "generation",
        "log",
        "obsolete generation log",
    )?;

    require_same_authority(lease.directory(), &witness, "pre-retirement verification")?;

    for plan in marker_moves.iter().chain(staging_moves.iter()) {
        move_checked(plan, "obsolete marker/staging history")?;
    }

    require_same_authority(
        lease.directory(),
        &witness,
        "post-marker pre-generation verification",
    )?;

    for plan in &generation_moves {
        move_checked(plan, "obsolete generation history")?;
    }

    let final_verified = require_same_authority(
        lease.directory(),
        &witness,
        "final post-retirement verification",
    )?;
    let final_namespace = scan_generation_namespace(lease.directory())?;

    for id in &retired_marker_generation_ids {
        if final_namespace.marker_files.contains_key(id) {
            return invalid(format!(
                "retired marker generation {id} remains in the strict namespace"
            ));
        }
    }
    for id in &retired_generation_ids {
        if final_namespace.generation_files.contains_key(id) {
            return invalid(format!(
                "retired generation log {id} remains in the strict namespace"
            ));
        }
    }
    for id in &retired_staging_marker_generation_ids {
        if final_namespace.staging_marker_files.contains_key(id) {
            return invalid(format!(
                "retired staging marker generation {id} remains in the strict namespace"
            ));
        }
    }

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

    Ok(WindowsGenerationCleanupSummary {
        protocol: GENERATION_CLEANUP_WINDOWS_PROTOCOL,
        authoritative_generation,
        retired_marker_generation_ids,
        retired_generation_ids,
        retired_staging_marker_generation_ids,
        retained_staging_marker_generation_ids,
        retained_uncommitted_generation_ids,
        final_generation: final_verified,
    })
}

#[cfg(windows)]
#[derive(Debug, Clone, PartialEq, Eq)]
struct AuthorityWitness {
    generation: u64,
    authoritative_log: String,
    committed_prefix: crate::generation_marker::CommittedPrefix,
    log_verification: db_storage_log::VerificationReport,
}

#[cfg(windows)]
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

#[cfg(windows)]
struct RetirementMove {
    source: PathBuf,
    target: PathBuf,
}

#[cfg(windows)]
fn build_moves(
    directory: &Path,
    paths: &std::collections::BTreeMap<u64, PathBuf>,
    ids: &[u64],
    kind: &str,
    extension: &str,
    label: &str,
) -> Result<Vec<RetirementMove>, WindowsGenerationCleanupError> {
    let mut moves = Vec::with_capacity(ids.len());
    for id in ids {
        let source = paths
            .get(id)
            .expect("retirement id came from namespace")
            .clone();
        require_real_regular_file(&source, label)?;
        let target = quarantine_path(directory, *id, kind, extension)?;
        require_absent(&target, "history quarantine target")?;
        moves.push(RetirementMove { source, target });
    }
    Ok(moves)
}

#[cfg(windows)]
fn require_same_authority(
    directory: &Path,
    expected: &AuthorityWitness,
    stage: &'static str,
) -> Result<GenerationVerificationSummary, WindowsGenerationCleanupError> {
    let current = verify_generation_directory(directory)?;
    let observed = AuthorityWitness::from_summary(current.summary());
    if &observed != expected {
        return Err(WindowsGenerationCleanupError::AuthorityChanged {
            generation: expected.generation,
            stage,
        });
    }
    Ok(current.summary().clone())
}

#[cfg(windows)]
fn quarantine_path(
    directory: &Path,
    generation: u64,
    kind: &str,
    extension: &str,
) -> Result<PathBuf, WindowsGenerationCleanupError> {
    let parent = directory.parent().ok_or_else(|| {
        WindowsGenerationCleanupError::Invalid(
            "generation directory has no parent for sibling retirement evidence".to_owned(),
        )
    })?;
    let base = directory.file_name().ok_or_else(|| {
        WindowsGenerationCleanupError::Invalid(
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
    plan: &RetirementMove,
    phase: &'static str,
) -> Result<(), WindowsGenerationCleanupError> {
    match move_no_replace_write_through(&plan.source, &plan.target) {
        Ok(()) => Ok(()),
        Err(source) => {
            let source_exists = fs::symlink_metadata(&plan.source).is_ok();
            let target_exists = fs::symlink_metadata(&plan.target).is_ok();
            if source_exists && !target_exists {
                Err(io_error(&plan.source, source))
            } else {
                Err(WindowsGenerationCleanupError::RetirementUncertain {
                    phase,
                    source_path: plan.source.clone(),
                    target_path: plan.target.clone(),
                    source,
                })
            }
        }
    }
}

#[cfg(windows)]
fn require_absent(path: &Path, label: &str) -> Result<(), WindowsGenerationCleanupError> {
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Ok(_) => invalid(format!("{label} already exists: {}", path.display())),
        Err(source) => Err(io_error(path, source)),
    }
}

#[cfg(windows)]
fn io_error(path: &Path, source: io::Error) -> WindowsGenerationCleanupError {
    WindowsGenerationCleanupError::Io {
        path: path.to_path_buf(),
        source,
    }
}

#[cfg(windows)]
fn invalid<T>(message: impl Into<String>) -> Result<T, WindowsGenerationCleanupError> {
    Err(WindowsGenerationCleanupError::Invalid(message.into()))
}
