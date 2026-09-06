use std::io;
use std::path::{Path, PathBuf};

use db_core::DbError;
use db_storage_log::InspectionReport;
use serde::Serialize;
use thiserror::Error;

use crate::generation_directory::{GenerationDirectoryError, GenerationVerificationSummary};
use crate::generation_lock::GenerationWriterLockError;
use crate::generation_publication::{GenerationPublicationError, GenerationPublicationSummary};
use crate::log_compaction::{LogCompactionError, LogCompactionReport};

pub const LEGACY_GENERATION_MIGRATION_PROTOCOL: &str =
    "append_log_legacy_to_generation_migration_unix_v1";

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct LegacyGenerationMigrationSummary {
    pub protocol: &'static str,
    pub source_file_format_version: u16,
    pub source_file_bytes: u64,
    pub source_record_count: u64,
    pub live_keys: usize,
    pub target_directory: String,
    pub generation: u64,
    pub generation_log: String,
    pub compaction: LogCompactionReport,
    pub publication: GenerationPublicationSummary,
    pub final_generation: GenerationVerificationSummary,
}

#[derive(Debug, Error)]
pub enum LegacyGenerationMigrationError {
    #[error(
        "legacy append-log migration is unsupported on this platform; no filesystem access was performed"
    )]
    UnsupportedPlatform,
    #[error("invalid legacy append-log migration: {0}")]
    Invalid(String),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error(transparent)]
    Directory(#[from] GenerationDirectoryError),
    #[error(transparent)]
    Lock(#[from] GenerationWriterLockError),
    #[error(transparent)]
    Compaction(#[from] LogCompactionError),
    #[error(transparent)]
    Publication(#[from] GenerationPublicationError),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "migration target directory {target} is visible but parent-directory durability could not be confirmed: {source}; the legacy source remains authoritative"
    )]
    TargetDirectoryDurabilityUncertain {
        target: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "legacy source changed while its migration snapshot was captured; target directory was not created"
    )]
    SourceChangedDuringSnapshot,
    #[error(
        "legacy source changed before generation publication; target {target} remains non-authoritative and the legacy source remains authoritative"
    )]
    SourceChangedBeforePublication { target: PathBuf },
    #[error(
        "legacy source changed after generation publication; target {target} contains a committed snapshot but migration cutover is not proven; preserve both and reconcile explicitly"
    )]
    SourceChangedAfterPublication { target: PathBuf },
    #[error(
        "post-migration verification selected generation {found}, expected imported generation {expected}"
    )]
    FinalAuthority { found: u64, expected: u64 },
    #[error("migrated generation {generation} does not reproduce the captured legacy live state")]
    FinalState { generation: u64 },
}

/// Imports a clean legacy one-file append log into a fresh generation directory without mutating
/// or deleting the legacy source.
///
/// This is an offline migration primitive. The caller must quiesce raw-path legacy writers for the
/// full operation. The implementation captures an exact temporary source snapshot, constructs the
/// new generation from that snapshot, and compares the live legacy source byte-for-byte immediately
/// before and after durable generation publication. A successful return therefore proves that the
/// imported generation matched the retained legacy bytes throughout the observed cutover window;
/// it does not prevent a non-cooperating legacy writer from mutating the old path after return.
pub fn migrate_legacy_append_log(
    source: &Path,
    target_directory: &Path,
) -> Result<LegacyGenerationMigrationSummary, LegacyGenerationMigrationError> {
    #[cfg(unix)]
    {
        unix::migrate_legacy_append_log(
            source,
            target_directory,
            #[cfg(test)]
            |_| Ok(()),
            #[cfg(test)]
            |_| Ok(()),
        )
    }

    #[cfg(not(unix))]
    {
        let _ = (source, target_directory);
        Err(LegacyGenerationMigrationError::UnsupportedPlatform)
    }
}

#[cfg(unix)]
mod unix {
    use std::fs::{self, File};
    use std::io::Read;

    use db_storage_log::LogEngine;
    use tempfile::NamedTempFile;

    use super::*;
    use crate::generation_directory::{canonical_generation_name, verify_generation_directory};
    use crate::generation_lock::acquire_generation_writer_lease;
    use crate::generation_publication::publish_generation_marker;
    use crate::log_compaction::compact_log_to_fresh_file;

    const IMPORT_GENERATION: u64 = 1;
    const COMPARE_BUFFER_BYTES: usize = 64 * 1024;

    pub(super) fn migrate_legacy_append_log(
        source: &Path,
        target_directory: &Path,
        #[cfg(test)] after_compaction: impl FnOnce(&Path) -> Result<(), LegacyGenerationMigrationError>,
        #[cfg(test)] after_publication: impl FnOnce(&Path) -> Result<(), LegacyGenerationMigrationError>,
    ) -> Result<LegacyGenerationMigrationSummary, LegacyGenerationMigrationError> {
        let source = canonical_legacy_source(source)?;
        let captured = require_clean_source(&source)?;
        let snapshot = capture_snapshot(&source, &captured)?;
        if !files_equal(&source, snapshot.path())? {
            return Err(LegacyGenerationMigrationError::SourceChangedDuringSnapshot);
        }

        let (target_parent, target_directory) = canonical_fresh_target(target_directory)?;
        fs::create_dir(&target_directory).map_err(|source| io_error(&target_directory, source))?;
        sync_directory(&target_directory).map_err(|source| io_error(&target_directory, source))?;
        if let Err(source) = sync_directory(&target_parent) {
            return Err(
                LegacyGenerationMigrationError::TargetDirectoryDurabilityUncertain {
                    target: target_directory,
                    source,
                },
            );
        }

        let generation_log = canonical_generation_name(IMPORT_GENERATION);
        let generation_path = target_directory.join(&generation_log);
        let compaction = compact_log_to_fresh_file(snapshot.path(), &generation_path)?;
        #[cfg(test)]
        after_compaction(&source)?;

        let lease = acquire_generation_writer_lease(&target_directory)?;
        if !source_matches_snapshot(&source, snapshot.path())? {
            return Err(
                LegacyGenerationMigrationError::SourceChangedBeforePublication {
                    target: target_directory,
                },
            );
        }

        let compacted = LogEngine::inspect(&generation_path, true)?;
        validate_imported_state(IMPORT_GENERATION, &captured, &compacted)?;
        let publication = publish_generation_marker(lease.directory(), IMPORT_GENERATION)?;
        #[cfg(test)]
        after_publication(&source)?;

        let final_verified = verify_generation_directory(lease.directory())?;
        if final_verified.summary().authoritative_generation != IMPORT_GENERATION {
            return Err(LegacyGenerationMigrationError::FinalAuthority {
                found: final_verified.summary().authoritative_generation,
                expected: IMPORT_GENERATION,
            });
        }
        let final_state = LogEngine::inspect(&generation_path, true)?;
        validate_imported_state(IMPORT_GENERATION, &captured, &final_state)?;

        if !source_matches_snapshot(&source, snapshot.path())? {
            return Err(
                LegacyGenerationMigrationError::SourceChangedAfterPublication {
                    target: target_directory,
                },
            );
        }

        Ok(LegacyGenerationMigrationSummary {
            protocol: LEGACY_GENERATION_MIGRATION_PROTOCOL,
            source_file_format_version: captured.verification.file_format_version,
            source_file_bytes: captured.verification.file_bytes,
            source_record_count: captured.verification.record_count,
            live_keys: captured.verification.live_keys,
            target_directory: target_directory.to_string_lossy().into_owned(),
            generation: IMPORT_GENERATION,
            generation_log,
            compaction,
            publication,
            final_generation: final_verified.summary().clone(),
        })
    }

    fn canonical_legacy_source(path: &Path) -> Result<PathBuf, LegacyGenerationMigrationError> {
        let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
        if !metadata.file_type().is_file() {
            return invalid(format!(
                "legacy source must be a real regular file rather than a symlink or non-file: {}",
                path.display()
            ));
        }
        fs::canonicalize(path).map_err(|source| io_error(path, source))
    }

    fn require_clean_source(
        path: &Path,
    ) -> Result<InspectionReport, LegacyGenerationMigrationError> {
        let report = LogEngine::inspect(path, true)?;
        if report.verification.recoverable_tail.is_some()
            || report.verification.file_bytes != report.verification.valid_bytes
        {
            return invalid(
                "legacy source must be a complete clean append-log image; repair a recoverable tail explicitly before migration",
            );
        }
        Ok(report)
    }

    fn capture_snapshot(
        source: &Path,
        expected: &InspectionReport,
    ) -> Result<NamedTempFile, LegacyGenerationMigrationError> {
        let mut input = File::open(source).map_err(|error| io_error(source, error))?;
        let mut snapshot =
            NamedTempFile::new().map_err(|error| LegacyGenerationMigrationError::Io {
                path: PathBuf::from("<temporary legacy migration snapshot>"),
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
            return Err(LegacyGenerationMigrationError::SourceChangedDuringSnapshot);
        }
        Ok(snapshot)
    }

    fn canonical_fresh_target(
        path: &Path,
    ) -> Result<(PathBuf, PathBuf), LegacyGenerationMigrationError> {
        match fs::symlink_metadata(path) {
            Ok(_) => {
                return invalid(format!(
                    "migration target already exists: {}",
                    path.display()
                ))
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(source) => return Err(io_error(path, source)),
        }
        let name = path.file_name().ok_or_else(|| {
            LegacyGenerationMigrationError::Invalid(format!(
                "migration target has no final path component: {}",
                path.display()
            ))
        })?;
        let parent = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new("."));
        let metadata = fs::symlink_metadata(parent).map_err(|source| io_error(parent, source))?;
        if !metadata.file_type().is_dir() {
            return invalid(format!(
                "migration target parent must be a real directory rather than a symlink or non-directory: {}",
                parent.display()
            ));
        }
        let parent = fs::canonicalize(parent).map_err(|source| io_error(parent, source))?;
        Ok((parent.clone(), parent.join(name)))
    }

    fn source_matches_snapshot(
        source: &Path,
        snapshot: &Path,
    ) -> Result<bool, LegacyGenerationMigrationError> {
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

    fn files_equal(left: &Path, right: &Path) -> Result<bool, LegacyGenerationMigrationError> {
        let left_meta = fs::metadata(left).map_err(|source| io_error(left, source))?;
        let right_meta = fs::metadata(right).map_err(|source| io_error(right, source))?;
        if left_meta.len() != right_meta.len() {
            return Ok(false);
        }

        let mut left_file = File::open(left).map_err(|source| io_error(left, source))?;
        let mut right_file = File::open(right).map_err(|source| io_error(right, source))?;
        let mut left_buffer = [0_u8; COMPARE_BUFFER_BYTES];
        let mut right_buffer = [0_u8; COMPARE_BUFFER_BYTES];
        loop {
            let left_read = left_file
                .read(&mut left_buffer)
                .map_err(|source| io_error(left, source))?;
            let right_read = right_file
                .read(&mut right_buffer)
                .map_err(|source| io_error(right, source))?;
            if left_read != right_read || left_buffer[..left_read] != right_buffer[..right_read] {
                return Ok(false);
            }
            if left_read == 0 {
                return Ok(true);
            }
        }
    }

    fn validate_imported_state(
        generation: u64,
        source: &InspectionReport,
        imported: &InspectionReport,
    ) -> Result<(), LegacyGenerationMigrationError> {
        if imported.verification.recoverable_tail.is_some()
            || imported.verification.live_keys != source.verification.live_keys
            || imported.entries != source.entries
        {
            return Err(LegacyGenerationMigrationError::FinalState { generation });
        }
        Ok(())
    }

    fn sync_directory(path: &Path) -> io::Result<()> {
        File::open(path)?.sync_all()
    }

    fn io_error(path: &Path, source: io::Error) -> LegacyGenerationMigrationError {
        LegacyGenerationMigrationError::Io {
            path: path.to_path_buf(),
            source,
        }
    }

    fn invalid<T>(message: impl Into<String>) -> Result<T, LegacyGenerationMigrationError> {
        Err(LegacyGenerationMigrationError::Invalid(message.into()))
    }

    #[cfg(test)]
    mod tests {
        use db_core::KvEngine;
        use tempfile::tempdir;

        use super::*;
        use crate::generation_directory::{canonical_marker_name, verify_generation_directory};

        #[test]
        fn late_legacy_write_before_publication_never_commits_stale_import() {
            let root = tempdir().expect("temporary root");
            let source = root.path().join("legacy.db");
            let target = root.path().join("generations");
            {
                let mut engine = LogEngine::create_new(&source).expect("create legacy source");
                engine.put(b"a", b"one").expect("put initial value");
            }

            let error = migrate_legacy_append_log(
                &source,
                &target,
                |source_path| {
                    let mut engine = LogEngine::open(source_path).expect("open legacy source");
                    engine.put(b"late", b"write").expect("inject late write");
                    Ok(())
                },
                |_| Ok(()),
            )
            .expect_err("late legacy write must abort migration");

            assert!(matches!(
                error,
                LegacyGenerationMigrationError::SourceChangedBeforePublication { .. }
            ));
            assert!(target.join(canonical_generation_name(1)).is_file());
            assert!(!target.join(canonical_marker_name(1)).exists());
            assert!(verify_generation_directory(&target).is_err());
            let source_state = LogEngine::inspect(&source, true).expect("inspect legacy source");
            assert_eq!(source_state.entries.len(), 2);
        }
    }
}
