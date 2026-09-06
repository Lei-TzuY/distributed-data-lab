use std::io;
use std::path::{Path, PathBuf};

use db_core::DbError;
use db_storage_log::InspectionReport;
use serde::Serialize;
use thiserror::Error;

use crate::generation_directory::{GenerationDirectoryError, GenerationVerificationSummary};
use crate::generation_lock::GenerationWriterLockError;

pub const LEGACY_CUTOVER_PROTOCOL: &str = "append_log_legacy_cutover_sentinel_unix_v1";
pub const LEGACY_CUTOVER_SENTINEL_PROTOCOL: &str = "append_log_legacy_cutover_sentinel_v1";

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct LegacyCutoverSummary {
    pub protocol: &'static str,
    pub legacy_path: String,
    pub retained_legacy_path: String,
    pub target_directory: String,
    pub target_generation: u64,
    pub source_file_bytes: u64,
    pub source_record_count: u64,
    pub live_keys: usize,
    pub final_generation: GenerationVerificationSummary,
}

#[derive(Debug, Serialize)]
struct LegacyCutoverSentinel<'a> {
    protocol: &'static str,
    target_directory: &'a str,
    retained_legacy_path: &'a str,
}

#[derive(Debug, Error)]
pub enum LegacyCutoverError {
    #[error(
        "legacy append-log cutover is unsupported on this platform; no filesystem access was performed"
    )]
    UnsupportedPlatform,
    #[error("invalid legacy append-log cutover: {0}")]
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
    #[error("legacy source changed before pathname cutover; no cutover sentinel was published")]
    SourceChangedBeforeCutover,
    #[error(
        "legacy pathname {legacy_path} now contains the cutover sentinel but retained source {retained_path} changed during cutover; preserve target and retained source and reconcile explicitly"
    )]
    RetainedSourceChangedAfterCutover {
        legacy_path: PathBuf,
        retained_path: PathBuf,
    },
    #[error(
        "legacy pathname {legacy_path} was replaced by the cutover sentinel but parent-directory durability could not be confirmed: {source}; preserve the retained source and target directory and verify recovery before proceeding"
    )]
    CutoverDurabilityUncertain {
        legacy_path: PathBuf,
        #[source]
        source: io::Error,
    },
}

/// Atomically retires a just-migrated Unix legacy pathname behind a non-log sentinel.
///
/// This is an explicit offline cutover step after `migrate_legacy_append_log`. The target must still
/// be the untouched imported generation 1 and reproduce the clean legacy source. The caller must
/// quiesce and close raw-path legacy writers for the operation. Successful cutover preserves the
/// original append-log inode through a sibling retained hard link, while the original pathname is
/// atomically replaced by a synced sentinel that new raw `LogEngine::open` calls reject.
pub fn cutover_migrated_legacy_append_log(
    legacy_source: &Path,
    target_directory: &Path,
) -> Result<LegacyCutoverSummary, LegacyCutoverError> {
    #[cfg(unix)]
    {
        unix::cutover_migrated_legacy_append_log(
            legacy_source,
            target_directory,
            #[cfg(test)]
            |_| Ok(()),
            #[cfg(test)]
            |_| Ok(()),
        )
    }

    #[cfg(not(unix))]
    {
        let _ = (legacy_source, target_directory);
        Err(LegacyCutoverError::UnsupportedPlatform)
    }
}

#[cfg(unix)]
mod unix {
    use std::ffi::OsString;
    use std::fs::{self, File, OpenOptions};
    use std::io::{Read, Write};

    use db_storage_log::LogEngine;
    use tempfile::NamedTempFile;

    use super::*;
    use crate::generation_directory::verify_generation_directory;
    use crate::generation_lock::acquire_generation_writer_lease;

    const IMPORT_GENERATION: u64 = 1;
    const COMPARE_BUFFER_BYTES: usize = 64 * 1024;
    const RETAINED_SUFFIX: &str = ".retired-append-log-v1";
    const STAGING_SUFFIX: &str = ".cutover-sentinel-staging-v1";
    const MAX_SENTINEL_PATH_BYTES: usize = 4096;

    pub(super) fn cutover_migrated_legacy_append_log(
        legacy_source: &Path,
        target_directory: &Path,
        #[cfg(test)] after_retained_link: impl FnOnce(&Path) -> Result<(), LegacyCutoverError>,
        #[cfg(test)] after_path_replacement: impl FnOnce(&Path) -> Result<(), LegacyCutoverError>,
    ) -> Result<LegacyCutoverSummary, LegacyCutoverError> {
        let source = canonical_legacy_source(legacy_source)?;
        let source_parent = source.parent().ok_or_else(|| {
            LegacyCutoverError::Invalid(format!(
                "legacy source has no parent directory: {}",
                source.display()
            ))
        })?;
        let source_state = require_clean_source(&source)?;
        let snapshot = capture_snapshot(&source, &source_state)?;
        if !files_equal(&source, snapshot.path())? {
            return Err(LegacyCutoverError::SourceChangedBeforeCutover);
        }

        let lease = acquire_generation_writer_lease(target_directory)?;
        let before = verify_generation_directory(lease.directory())?;
        validate_fresh_import_target(&source_state, &before)?;
        let target_state = LogEngine::inspect(before.authoritative_log_path(), true)?;
        if target_state.entries != source_state.entries {
            return invalid("target generation 1 does not reproduce the current legacy live state");
        }

        let retained_path = sibling_path(&source, RETAINED_SUFFIX)?;
        let staging_path = sibling_path(&source, STAGING_SUFFIX)?;
        require_absent(&retained_path, "retained legacy source")?;
        require_absent(&staging_path, "cutover sentinel staging path")?;

        let target_display = bounded_path_string(lease.directory(), "target generation directory")?;
        let retained_display = bounded_path_string(&retained_path, "retained legacy source")?;
        let sentinel = serde_json::to_vec_pretty(&LegacyCutoverSentinel {
            protocol: LEGACY_CUTOVER_SENTINEL_PROTOCOL,
            target_directory: &target_display,
            retained_legacy_path: &retained_display,
        })?;
        write_synced_new(&staging_path, &sentinel)?;

        if !source_matches_snapshot(&source, snapshot.path())? {
            cleanup_pre_cutover(&staging_path, None, source_parent);
            return Err(LegacyCutoverError::SourceChangedBeforeCutover);
        }

        sync_regular_file(&source)?;
        fs::hard_link(&source, &retained_path).map_err(|error| io_error(&retained_path, error))?;
        sync_directory(source_parent).map_err(|error| io_error(source_parent, error))?;

        #[cfg(test)]
        after_retained_link(&source)?;

        if !source_matches_snapshot(&source, snapshot.path())? {
            cleanup_pre_cutover(&staging_path, Some(&retained_path), source_parent);
            return Err(LegacyCutoverError::SourceChangedBeforeCutover);
        }

        fs::rename(&staging_path, &source).map_err(|error| io_error(&source, error))?;

        #[cfg(test)]
        after_path_replacement(&retained_path)?;

        if let Err(source_error) = sync_directory(source_parent) {
            return Err(LegacyCutoverError::CutoverDurabilityUncertain {
                legacy_path: source,
                source: source_error,
            });
        }

        require_exact_file_bytes(&source, &sentinel, "published cutover sentinel")?;
        if !files_equal(&retained_path, snapshot.path())? {
            return Err(LegacyCutoverError::RetainedSourceChangedAfterCutover {
                legacy_path: source,
                retained_path,
            });
        }

        let final_verified = verify_generation_directory(lease.directory())?;
        validate_fresh_import_target(&source_state, &final_verified)?;
        let final_state = LogEngine::inspect(final_verified.authoritative_log_path(), true)?;
        if final_state != target_state {
            return invalid(
                "target generation changed while legacy pathname cutover was in progress",
            );
        }

        Ok(LegacyCutoverSummary {
            protocol: LEGACY_CUTOVER_PROTOCOL,
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

    fn validate_fresh_import_target(
        source: &InspectionReport,
        target: &crate::generation_directory::VerifiedGenerationDirectory,
    ) -> Result<(), LegacyCutoverError> {
        let summary = target.summary();
        if summary.authoritative_generation != IMPORT_GENERATION {
            return invalid(format!(
                "target must still have imported generation {IMPORT_GENERATION} as authority; found generation {}",
                summary.authoritative_generation
            ));
        }
        if summary.log_verification != summary.committed_prefix_verification
            || summary.log_verification.recoverable_tail.is_some()
        {
            return invalid(
                "target generation 1 has changed since migration publication; cut over before routing new mutations",
            );
        }
        if summary.log_verification.live_keys != source.verification.live_keys {
            return invalid("target generation 1 live-key count differs from legacy source");
        }
        Ok(())
    }

    fn canonical_legacy_source(path: &Path) -> Result<PathBuf, LegacyCutoverError> {
        let metadata = fs::symlink_metadata(path).map_err(|error| io_error(path, error))?;
        if !metadata.file_type().is_file() {
            return invalid(format!(
                "legacy source must be a real regular file rather than a symlink or non-file: {}",
                path.display()
            ));
        }
        fs::canonicalize(path).map_err(|error| io_error(path, error))
    }

    fn require_clean_source(path: &Path) -> Result<InspectionReport, LegacyCutoverError> {
        let report = LogEngine::inspect(path, true)?;
        if report.verification.recoverable_tail.is_some()
            || report.verification.file_bytes != report.verification.valid_bytes
        {
            return invalid(
                "legacy source must be a complete clean append-log image before pathname cutover",
            );
        }
        Ok(report)
    }

    fn capture_snapshot(
        source: &Path,
        expected: &InspectionReport,
    ) -> Result<NamedTempFile, LegacyCutoverError> {
        let mut input = File::open(source).map_err(|error| io_error(source, error))?;
        let mut snapshot = NamedTempFile::new().map_err(|error| LegacyCutoverError::Io {
            path: PathBuf::from("<temporary legacy cutover snapshot>"),
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
            return Err(LegacyCutoverError::SourceChangedBeforeCutover);
        }
        Ok(snapshot)
    }

    fn sibling_path(source: &Path, suffix: &str) -> Result<PathBuf, LegacyCutoverError> {
        let name = source.file_name().ok_or_else(|| {
            LegacyCutoverError::Invalid(format!(
                "legacy source has no final path component: {}",
                source.display()
            ))
        })?;
        let mut sibling = OsString::from(name);
        sibling.push(suffix);
        Ok(source.with_file_name(sibling))
    }

    fn bounded_path_string(path: &Path, label: &str) -> Result<String, LegacyCutoverError> {
        let value = path.to_string_lossy().into_owned();
        if value.len() > MAX_SENTINEL_PATH_BYTES {
            return invalid(format!(
                "{label} path exceeds {MAX_SENTINEL_PATH_BYTES} encoded bytes"
            ));
        }
        Ok(value)
    }

    fn write_synced_new(path: &Path, bytes: &[u8]) -> Result<(), LegacyCutoverError> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
            .map_err(|error| io_error(path, error))?;
        file.write_all(bytes)
            .map_err(|error| io_error(path, error))?;
        file.sync_all().map_err(|error| io_error(path, error))
    }

    fn sync_regular_file(path: &Path) -> Result<(), LegacyCutoverError> {
        File::open(path)
            .map_err(|error| io_error(path, error))?
            .sync_all()
            .map_err(|error| io_error(path, error))
    }

    fn sync_directory(path: &Path) -> io::Result<()> {
        File::open(path)?.sync_all()
    }

    fn require_absent(path: &Path, label: &str) -> Result<(), LegacyCutoverError> {
        match fs::symlink_metadata(path) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Ok(_) => invalid(format!("{label} already exists: {}", path.display())),
            Err(error) => Err(io_error(path, error)),
        }
    }

    fn source_matches_snapshot(source: &Path, snapshot: &Path) -> Result<bool, LegacyCutoverError> {
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

    fn files_equal(left: &Path, right: &Path) -> Result<bool, LegacyCutoverError> {
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

    fn require_exact_file_bytes(
        path: &Path,
        expected: &[u8],
        label: &str,
    ) -> Result<(), LegacyCutoverError> {
        let metadata = fs::symlink_metadata(path).map_err(|error| io_error(path, error))?;
        if !metadata.file_type().is_file() {
            return invalid(format!(
                "{label} is not a real regular file: {}",
                path.display()
            ));
        }
        let actual = fs::read(path).map_err(|error| io_error(path, error))?;
        if actual != expected {
            return invalid(format!("{label} bytes changed after publication"));
        }
        Ok(())
    }

    fn cleanup_pre_cutover(staging: &Path, retained: Option<&Path>, parent: &Path) {
        let _ = fs::remove_file(staging);
        if let Some(retained) = retained {
            let _ = fs::remove_file(retained);
        }
        let _ = sync_directory(parent);
    }

    fn io_error(path: &Path, source: io::Error) -> LegacyCutoverError {
        LegacyCutoverError::Io {
            path: path.to_path_buf(),
            source,
        }
    }

    fn invalid<T>(message: impl Into<String>) -> Result<T, LegacyCutoverError> {
        Err(LegacyCutoverError::Invalid(message.into()))
    }

    #[cfg(test)]
    mod tests {
        use db_core::KvEngine;
        use tempfile::tempdir;

        use super::*;
        use crate::generation_migration::migrate_legacy_append_log;

        #[test]
        fn source_drift_after_retained_link_never_publishes_sentinel() {
            let root = tempdir().expect("temporary root");
            let source = root.path().join("legacy.db");
            let target = root.path().join("generations");
            {
                let mut engine = LogEngine::create_new(&source).expect("create legacy source");
                engine.put(b"a", b"one").expect("put initial value");
            }
            migrate_legacy_append_log(&source, &target).expect("migrate legacy source");

            let error = cutover_migrated_legacy_append_log(
                &source,
                &target,
                |source_path| {
                    let mut engine = LogEngine::open(source_path).expect("open source for drift");
                    engine.put(b"late", b"write").expect("inject late write");
                    Ok(())
                },
                |_| Ok(()),
            )
            .expect_err("late source write must abort cutover");

            assert!(matches!(
                error,
                LegacyCutoverError::SourceChangedBeforeCutover
            ));
            assert!(LogEngine::inspect(&source, true).is_ok());
            assert!(!sibling_path(&source, RETAINED_SUFFIX)
                .expect("retained path")
                .exists());
            assert!(!sibling_path(&source, STAGING_SUFFIX)
                .expect("staging path")
                .exists());
        }
    }
}
