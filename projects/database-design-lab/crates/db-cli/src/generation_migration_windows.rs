use std::path::Path;

use crate::generation_migration::{
    LegacyGenerationMigrationError, LegacyGenerationMigrationSummary,
};

pub const LEGACY_GENERATION_MIGRATION_WINDOWS_PROTOCOL: &str =
    "append_log_legacy_to_generation_migration_windows_v1";

#[cfg(windows)]
pub fn migrate_legacy_append_log_windows(
    source: &Path,
    target_directory: &Path,
) -> Result<LegacyGenerationMigrationSummary, LegacyGenerationMigrationError> {
    windows::migrate(source, target_directory)
}

#[cfg(not(windows))]
pub fn migrate_legacy_append_log_windows(
    source: &Path,
    target_directory: &Path,
) -> Result<LegacyGenerationMigrationSummary, LegacyGenerationMigrationError> {
    let _ = (source, target_directory);
    Err(LegacyGenerationMigrationError::UnsupportedPlatform)
}

#[cfg(windows)]
mod windows {
    use std::fs::{self, File};
    use std::io::{self, Read};
    use std::path::{Path, PathBuf};

    use db_storage_log::{InspectionReport, LogEngine};
    use tempfile::{Builder, NamedTempFile};

    use super::*;
    use crate::generation_compaction_windows::publish_compacted_generation_marker_windows;
    use crate::generation_directory::{
        canonical_generation_name, canonical_reservation_name, verify_generation_directory,
    };
    use crate::generation_lock::acquire_generation_writer_lease;
    use crate::generation_reservation::{
        publish_generation_reservation_windows, GenerationReservationError,
    };
    use crate::log_compaction::compact_log_to_fresh_file;
    use crate::windows_durable::move_no_replace_write_through;

    const IMPORT_GENERATION: u64 = 1;
    const COMPARE_BUFFER_BYTES: usize = 64 * 1024;

    pub(super) fn migrate(
        source: &Path,
        target_directory: &Path,
    ) -> Result<LegacyGenerationMigrationSummary, LegacyGenerationMigrationError> {
        let source = canonical_legacy_source(source)?;
        let captured = require_clean_source(&source)?;
        let snapshot = capture_snapshot(&source, &captured)?;
        if !files_equal(&source, snapshot.path())? {
            return Err(LegacyGenerationMigrationError::SourceChangedDuringSnapshot);
        }

        let (target_parent, target_directory) = canonical_fresh_target(target_directory)?;
        publish_fresh_target_directory(&target_parent, &target_directory)?;

        let lease = acquire_generation_writer_lease(&target_directory)?;
        let reservation =
            publish_generation_reservation_windows(lease.directory(), IMPORT_GENERATION)
                .map_err(map_reservation_error)?;

        let generation_log = canonical_generation_name(IMPORT_GENERATION);
        let generation_path = lease.directory().join(&generation_log);
        let compaction = compact_log_to_fresh_file(snapshot.path(), &generation_path)?;

        if !source_matches_snapshot(&source, snapshot.path())? {
            return Err(
                LegacyGenerationMigrationError::SourceChangedBeforePublication {
                    target: target_directory,
                },
            );
        }

        let compacted = LogEngine::inspect(&generation_path, true)?;
        validate_imported_state(IMPORT_GENERATION, &captured, &compacted)?;
        let publication = publish_compacted_generation_marker_windows(
            lease.directory(),
            IMPORT_GENERATION,
            &compacted,
        )?;

        let final_verified = verify_generation_directory(lease.directory())?;
        if final_verified.summary().authoritative_generation != IMPORT_GENERATION {
            return Err(LegacyGenerationMigrationError::FinalAuthority {
                found: final_verified.summary().authoritative_generation,
                expected: IMPORT_GENERATION,
            });
        }
        if final_verified.summary().reservation_generation_ids != [IMPORT_GENERATION] {
            return invalid(
                "Windows migration did not retain exactly the generation-1 durable reservation",
            );
        }
        if reservation != canonical_reservation_name(IMPORT_GENERATION) {
            return invalid("Windows migration reservation name disagrees with canonical frontier");
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
            protocol: LEGACY_GENERATION_MIGRATION_WINDOWS_PROTOCOL,
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

    fn publish_fresh_target_directory(
        parent: &Path,
        target: &Path,
    ) -> Result<(), LegacyGenerationMigrationError> {
        let staging = Builder::new()
            .prefix(".append-log-legacy-migration-")
            .tempdir_in(parent)
            .map_err(|source| io_error(parent, source))?;
        let staging_path = staging.path().to_path_buf();

        if let Err(source) = move_no_replace_write_through(&staging_path, target) {
            match fs::symlink_metadata(target) {
                Ok(_) => {
                    std::mem::forget(staging);
                    return Err(
                        LegacyGenerationMigrationError::TargetDirectoryDurabilityUncertain {
                            target: target.to_path_buf(),
                            source,
                        },
                    );
                }
                Err(error) if error.kind() == io::ErrorKind::NotFound => {
                    return Err(io_error(target, source));
                }
                Err(_) => {
                    std::mem::forget(staging);
                    return Err(
                        LegacyGenerationMigrationError::TargetDirectoryDurabilityUncertain {
                            target: target.to_path_buf(),
                            source,
                        },
                    );
                }
            }
        }

        // The staging path has been renamed away. Forget its RAII owner so drop never attempts to
        // remove the now-authoritative target through stale staging-path bookkeeping.
        std::mem::forget(staging);
        Ok(())
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

    fn map_reservation_error(error: GenerationReservationError) -> LegacyGenerationMigrationError {
        match error {
            GenerationReservationError::UnsupportedPlatform => {
                LegacyGenerationMigrationError::UnsupportedPlatform
            }
            GenerationReservationError::Lock(error) => LegacyGenerationMigrationError::Lock(error),
            GenerationReservationError::Directory(error) => {
                LegacyGenerationMigrationError::Directory(error)
            }
            GenerationReservationError::Io { path, source } => {
                LegacyGenerationMigrationError::Io { path, source }
            }
            GenerationReservationError::DurabilityUncertain {
                directory, source, ..
            } => LegacyGenerationMigrationError::TargetDirectoryDurabilityUncertain {
                target: directory,
                source,
            },
            GenerationReservationError::NotRetained { generation } => {
                LegacyGenerationMigrationError::Invalid(format!(
                    "Windows migration reservation {generation} was not retained"
                ))
            }
        }
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
}
