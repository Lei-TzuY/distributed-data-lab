use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use db_core::DbError;
use db_storage_log::{LogEngine, VerificationReport};
use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::generation_cutover::LEGACY_CUTOVER_SENTINEL_PROTOCOL;
use crate::generation_directory::{
    canonical_real_directory, verify_generation_directory, GenerationDirectoryError,
    GenerationVerificationSummary,
};

pub const LEGACY_CUTOVER_VERIFICATION_PROTOCOL: &str = "append_log_legacy_cutover_verification_v1";

const IMPORT_GENERATION: u64 = 1;
const RETAINED_SUFFIX: &str = ".retired-append-log-v1";
const MAX_SENTINEL_BYTES: usize = 16 * 1024;

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct LegacyCutoverVerificationSummary {
    pub protocol: &'static str,
    pub legacy_path: String,
    pub retained_legacy_path: String,
    pub target_directory: String,
    pub target_generation: u64,
    pub retained_verification: VerificationReport,
    pub final_generation: GenerationVerificationSummary,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct LegacyCutoverSentinel {
    protocol: String,
    target_directory: String,
    retained_legacy_path: String,
}

#[derive(Debug, Error)]
pub enum LegacyCutoverVerificationError {
    #[error("invalid legacy append-log cutover evidence: {0}")]
    Invalid(String),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error(transparent)]
    Directory(#[from] GenerationDirectoryError),
    #[error(transparent)]
    Json(#[from] serde_json::Error),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
}

/// Verifies a freshly completed legacy-path cutover without mutating any file.
///
/// The check is intentionally strict: it is for the handoff point immediately after migration and
/// cutover, before generation-aware writes or compaction advance the target. It proves that the
/// legacy pathname contains the expected sentinel, that the sentinel names the supplied canonical
/// target and derived retained sibling, that the retained source is a clean append log, and that the
/// untouched compacted generation-1 authority reproduces the retained legacy live state exactly.
pub fn verify_fresh_legacy_cutover(
    legacy_source: &Path,
    target_directory: &Path,
) -> Result<LegacyCutoverVerificationSummary, LegacyCutoverVerificationError> {
    let legacy_path = canonical_real_file(legacy_source, "legacy cutover sentinel")?;
    let sentinel_bytes = read_bounded(&legacy_path, MAX_SENTINEL_BYTES)?;
    let sentinel: LegacyCutoverSentinel = serde_json::from_slice(&sentinel_bytes)?;
    if sentinel.protocol != LEGACY_CUTOVER_SENTINEL_PROTOCOL {
        return invalid(format!(
            "legacy pathname sentinel protocol {:?} is not {:?}",
            sentinel.protocol, LEGACY_CUTOVER_SENTINEL_PROTOCOL
        ));
    }

    let target_directory = canonical_real_directory(target_directory)?;
    let retained_path = sibling_path(&legacy_path, RETAINED_SUFFIX)?;
    require_real_regular_file(&retained_path, "retained legacy source")?;

    let target_display = target_directory.to_string_lossy().into_owned();
    let retained_display = retained_path.to_string_lossy().into_owned();
    if sentinel.target_directory != target_display {
        return invalid(format!(
            "sentinel target directory {:?} does not match supplied canonical target {:?}",
            sentinel.target_directory, target_display
        ));
    }
    if sentinel.retained_legacy_path != retained_display {
        return invalid(format!(
            "sentinel retained path {:?} does not match derived retained path {:?}",
            sentinel.retained_legacy_path, retained_display
        ));
    }

    let retained = LogEngine::inspect(&retained_path, true)?;
    if retained.verification.recoverable_tail.is_some()
        || retained.verification.file_bytes != retained.verification.valid_bytes
    {
        return invalid("retained legacy source is not a complete clean append-log image");
    }

    let verified = verify_generation_directory(&target_directory)?;
    let summary = verified.summary();
    if summary.authoritative_generation != IMPORT_GENERATION
        || summary.highest_observed_generation != IMPORT_GENERATION
        || summary.marker_generation_ids != [IMPORT_GENERATION]
        || !summary.staging_marker_generation_ids.is_empty()
        || !summary.uncommitted_generation_ids.is_empty()
        || !(summary.reservation_generation_ids.is_empty()
            || summary.reservation_generation_ids == [IMPORT_GENERATION])
    {
        return invalid(
            "target is no longer the untouched imported generation 1; fresh cutover verification must run before routed mutation, reservation, or compaction",
        );
    }
    if summary.log_verification != summary.committed_prefix_verification
        || summary.log_verification.recoverable_tail.is_some()
    {
        return invalid(
            "target generation 1 has changed after migration publication; fresh cutover evidence is no longer valid",
        );
    }

    let authoritative = LogEngine::inspect(verified.authoritative_log_path(), true)?;
    if authoritative.verification != summary.log_verification {
        return invalid(
            "authoritative generation changed while fresh cutover verification was in progress",
        );
    }
    if authoritative.entries != retained.entries {
        return invalid(
            "untouched generation-1 authority does not reproduce the retained legacy live state",
        );
    }

    Ok(LegacyCutoverVerificationSummary {
        protocol: LEGACY_CUTOVER_VERIFICATION_PROTOCOL,
        legacy_path: legacy_path.to_string_lossy().into_owned(),
        retained_legacy_path: retained_display,
        target_directory: target_display,
        target_generation: IMPORT_GENERATION,
        retained_verification: retained.verification,
        final_generation: summary.clone(),
    })
}

fn canonical_real_file(
    path: &Path,
    label: &str,
) -> Result<PathBuf, LegacyCutoverVerificationError> {
    require_real_regular_file(path, label)?;
    fs::canonicalize(path).map_err(|source| io_error(path, source))
}

fn require_real_regular_file(
    path: &Path,
    label: &str,
) -> Result<(), LegacyCutoverVerificationError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_file() {
        return invalid(format!(
            "{label} must be a real regular file rather than a symlink or non-file: {}",
            path.display()
        ));
    }
    Ok(())
}

fn sibling_path(source: &Path, suffix: &str) -> Result<PathBuf, LegacyCutoverVerificationError> {
    let name = source.file_name().ok_or_else(|| {
        LegacyCutoverVerificationError::Invalid(format!(
            "legacy cutover sentinel has no final path component: {}",
            source.display()
        ))
    })?;
    let mut sibling = OsString::from(name);
    sibling.push(suffix);
    Ok(source.with_file_name(sibling))
}

fn read_bounded(path: &Path, limit: usize) -> Result<Vec<u8>, LegacyCutoverVerificationError> {
    let metadata = fs::metadata(path).map_err(|source| io_error(path, source))?;
    if metadata.len() > limit as u64 {
        return invalid(format!(
            "cutover sentinel exceeds {limit} bytes: {}",
            path.display()
        ));
    }
    let bytes = fs::read(path).map_err(|source| io_error(path, source))?;
    if bytes.len() > limit {
        return invalid(format!(
            "cutover sentinel grew beyond {limit} bytes while reading: {}",
            path.display()
        ));
    }
    Ok(bytes)
}

fn io_error(path: &Path, source: io::Error) -> LegacyCutoverVerificationError {
    LegacyCutoverVerificationError::Io {
        path: path.to_path_buf(),
        source,
    }
}

fn invalid<T>(message: impl Into<String>) -> Result<T, LegacyCutoverVerificationError> {
    Err(LegacyCutoverVerificationError::Invalid(message.into()))
}
