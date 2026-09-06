#[cfg(windows)]
use std::fs::{self, File, OpenOptions};
#[cfg(windows)]
use std::io::{self, Read, Write};
use std::path::Path;

#[cfg(windows)]
use db_storage_log::{InspectionReport, LogEngine, VerificationReport};

use crate::generation_compaction::{
    OfflineGenerationCompactSwitchError, OfflineGenerationCompactSwitchSummary,
};
#[cfg(windows)]
use crate::generation_directory::{
    canonical_generation_name, canonical_marker_name, canonical_real_directory,
    canonical_staging_marker_name, require_real_regular_file, scan_generation_namespace,
    verify_generation_directory, GenerationDirectoryError, GenerationVerificationSummary,
};
#[cfg(windows)]
use crate::generation_lock::acquire_generation_writer_lease;
#[cfg(windows)]
use crate::generation_marker::{
    decode_commit_marker, encode_commit_marker, CommitMarker, CommittedPrefix, Crc32Ieee,
    COMMIT_MARKER_LEN, COMMIT_MARKER_VERSION,
};
#[cfg(windows)]
use crate::generation_prefix::verify_committed_prefix;
#[cfg(windows)]
use crate::generation_publication::{GenerationPublicationError, GenerationPublicationSummary};
#[cfg(windows)]
use crate::generation_reservation::reserve_next_generation;
#[cfg(windows)]
use crate::log_compaction::compact_log_to_fresh_file;
#[cfg(windows)]
use crate::windows_durable::move_no_replace_write_through;

pub const OFFLINE_GENERATION_COMPACT_SWITCH_WINDOWS_PROTOCOL: &str =
    "append_log_offline_generation_compact_switch_windows_v1";
pub const GENERATION_MARKER_PUBLICATION_WINDOWS_PROTOCOL: &str =
    "append_log_generation_marker_publication_windows_v1";

#[cfg(windows)]
const CRC_BUFFER_BYTES: usize = 64 * 1024;

/// Windows authoritative compact switch.
///
/// This operation intentionally keeps Windows marker publication private to the composed switch.
/// The candidate generation is produced by `compact_log_to_fresh_file`, whose Windows path publishes
/// the canonical candidate name with the audited no-overwrite `MOVEFILE_WRITE_THROUGH` primitive.
/// Only after that successful publication does this function enter the shared writer lease, re-check
/// old authority and source state, and publish the final marker with the same audited namespace
/// primitive. The standalone marker publisher therefore remains fail-closed on Windows for arbitrary
/// pre-existing generation files whose filename durability is not proven by this operation.
#[cfg(windows)]
pub fn compact_switch_generation_offline_windows(
    directory: &Path,
) -> Result<OfflineGenerationCompactSwitchSummary, OfflineGenerationCompactSwitchError> {
    let reservation = reserve_next_generation(directory)?;
    let before = verify_generation_directory(directory)?;
    let old_generation = before.summary().authoritative_generation;
    if reservation.generation <= old_generation {
        return Err(
            OfflineGenerationCompactSwitchError::ReservedGenerationObsolete {
                reserved_generation: reservation.generation,
                authoritative_generation: old_generation,
            },
        );
    }

    let old_generation_log = before.summary().authoritative_log.clone();
    let source_path = before.authoritative_log_path();
    let source_state = LogEngine::inspect(&source_path, true)?;
    let source_authority = SourceAuthorityWitness::from_summary(before.summary());
    let new_generation = reservation.generation;
    let new_generation_log = canonical_generation_name(new_generation);
    let new_path = before.directory().join(&new_generation_log);

    // On Windows this call synchronizes the complete staging image and publishes the fresh canonical
    // generation name with MOVEFILE_WRITE_THROUGH before returning success.
    let compaction = compact_log_to_fresh_file(&source_path, &new_path)?;

    // Only the authority-changing section is exclusive. A routed writer may run while the compact
    // candidate is being built; this lease-held recheck detects that drift before marker authority.
    let lease = acquire_generation_writer_lease(before.directory())?;
    let before_publication = verify_generation_directory(lease.directory())?;
    let current_authority = SourceAuthorityWitness::from_summary(before_publication.summary());
    let current_source_state = LogEngine::inspect(&source_path, true)?;
    if current_authority != source_authority || current_source_state != source_state {
        return Err(OfflineGenerationCompactSwitchError::SourceChanged {
            orphan_generation: new_generation,
        });
    }

    let compact_state = LogEngine::inspect(&new_path, true)?;
    validate_compact_state(new_generation, &source_state, &compact_state)?;

    let publication = publish_compacted_generation_marker_windows(
        lease.directory(),
        new_generation,
        &compact_state,
    )?;
    let final_verified = verify_generation_directory(lease.directory())?;
    if final_verified.summary().authoritative_generation != new_generation {
        return Err(OfflineGenerationCompactSwitchError::FinalAuthority {
            found: final_verified.summary().authoritative_generation,
            expected: new_generation,
        });
    }

    let final_state = LogEngine::inspect(&new_path, true)?;
    if final_state != compact_state {
        return Err(OfflineGenerationCompactSwitchError::FinalState {
            generation: new_generation,
        });
    }

    Ok(OfflineGenerationCompactSwitchSummary {
        protocol: OFFLINE_GENERATION_COMPACT_SWITCH_WINDOWS_PROTOCOL,
        old_generation,
        new_generation,
        old_generation_log,
        new_generation_log,
        reservation,
        compaction,
        publication,
        final_generation: final_verified.summary().clone(),
    })
}

#[cfg(not(windows))]
pub fn compact_switch_generation_offline_windows(
    directory: &Path,
) -> Result<OfflineGenerationCompactSwitchSummary, OfflineGenerationCompactSwitchError> {
    let _ = directory;
    Err(OfflineGenerationCompactSwitchError::UnsupportedPlatform)
}

#[cfg(windows)]
pub(crate) fn publish_compacted_generation_marker_windows(
    directory: &Path,
    generation: u64,
    expected_compact: &InspectionReport,
) -> Result<GenerationPublicationSummary, GenerationPublicationError> {
    if generation == 0 {
        return invalid_publication("generation id must be greater than zero");
    }

    let directory = canonical_real_directory(directory).map_err(map_directory_error)?;
    let namespace = scan_generation_namespace(&directory).map_err(map_directory_error)?;
    if namespace.marker_files.keys().any(|id| *id >= generation) {
        return invalid_publication(format!(
            "generation {generation} is not newer than every existing committed generation"
        ));
    }
    if !namespace.reservation_files.contains_key(&generation) {
        return invalid_publication(format!(
            "generation {generation} has no retained durable reservation"
        ));
    }

    let log_path = directory.join(canonical_generation_name(generation));
    let marker_path = directory.join(canonical_marker_name(generation));
    let staging_path = directory.join(canonical_staging_marker_name(generation));
    require_real_regular_file(&log_path, "generation log").map_err(map_directory_error)?;
    require_absent(&marker_path, "commit marker")?;
    remove_stale_staging_if_safe(&staging_path)?;

    let baseline = require_clean_generation(&log_path)?;
    if baseline != expected_compact.verification {
        return invalid_publication(
            "Windows compact candidate changed after write-through publication",
        );
    }
    let committed_prefix = derive_prefix_proof(&log_path, &baseline)?;
    let prefix_verification = verify_committed_prefix(&log_path, committed_prefix)?;
    if prefix_verification != baseline {
        return invalid_publication(
            "derived committed-prefix verification disagrees with clean generation",
        );
    }

    // The compact-copy operation already published the canonical generation name with
    // MOVEFILE_WRITE_THROUGH. Re-sync its complete contents before constructing marker authority.
    let generation_file = OpenOptions::new()
        .write(true)
        .open(&log_path)
        .map_err(|source| publication_io_error(&log_path, source))?;
    generation_file
        .sync_all()
        .map_err(|source| publication_io_error(&log_path, source))?;
    drop(generation_file);
    require_exact_compact_generation(&log_path, expected_compact, committed_prefix)?;

    let encoded = encode_commit_marker(generation, committed_prefix).map_err(|error| {
        GenerationPublicationError::Invalid(format!("cannot encode commit marker: {error}"))
    })?;
    write_synced_staging(&staging_path, &encoded)?;
    require_exact_compact_generation(&log_path, expected_compact, committed_prefix)?;

    if let Err(source) = move_no_replace_write_through(&staging_path, &marker_path) {
        match fs::symlink_metadata(&marker_path) {
            Ok(_) => {
                return Err(GenerationPublicationError::DurabilityUncertain {
                    marker: marker_path,
                    source,
                });
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                let _ = fs::remove_file(&staging_path);
                return Err(publication_io_error(&marker_path, source));
            }
            Err(_) => {
                return Err(GenerationPublicationError::DurabilityUncertain {
                    marker: marker_path,
                    source,
                });
            }
        }
    }

    verify_published_marker(&marker_path, generation, committed_prefix)?;
    let retained_prefix = verify_committed_prefix(&log_path, committed_prefix)?;
    if retained_prefix != baseline {
        return invalid_publication("published marker prefix no longer matches compact generation");
    }
    let retained_state = LogEngine::inspect(&log_path, true)?;
    if &retained_state != expected_compact {
        return invalid_publication("compact generation changed after marker publication");
    }

    let staging_retained = match fs::symlink_metadata(&staging_path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => false,
        Ok(_) => true,
        Err(_) => true,
    };

    Ok(GenerationPublicationSummary {
        protocol: GENERATION_MARKER_PUBLICATION_WINDOWS_PROTOCOL,
        marker_format_version: COMMIT_MARKER_VERSION,
        generation,
        generation_log: canonical_generation_name(generation),
        marker: canonical_marker_name(generation),
        committed_prefix,
        staging_retained,
    })
}

#[cfg(windows)]
#[derive(Debug, Clone, PartialEq, Eq)]
struct SourceAuthorityWitness {
    authoritative_generation: u64,
    committed_prefix: CommittedPrefix,
    log_verification: VerificationReport,
}

#[cfg(windows)]
impl SourceAuthorityWitness {
    fn from_summary(summary: &GenerationVerificationSummary) -> Self {
        Self {
            authoritative_generation: summary.authoritative_generation,
            committed_prefix: summary.committed_prefix,
            log_verification: summary.log_verification.clone(),
        }
    }
}

#[cfg(windows)]
fn validate_compact_state(
    generation: u64,
    source: &InspectionReport,
    compacted: &InspectionReport,
) -> Result<(), OfflineGenerationCompactSwitchError> {
    if compacted.verification.recoverable_tail.is_some()
        || compacted.verification.live_keys != source.verification.live_keys
        || compacted.entries != source.entries
    {
        return Err(OfflineGenerationCompactSwitchError::TargetChanged { generation });
    }
    Ok(())
}

#[cfg(windows)]
fn derive_prefix_proof(
    path: &Path,
    report: &VerificationReport,
) -> Result<CommittedPrefix, GenerationPublicationError> {
    let mut file = File::open(path).map_err(|source| publication_io_error(path, source))?;
    let mut remaining = report.file_bytes;
    let mut hasher = Crc32Ieee::new();
    let mut buffer = [0_u8; CRC_BUFFER_BYTES];

    while remaining > 0 {
        let wanted = usize::try_from(remaining.min(CRC_BUFFER_BYTES as u64))
            .expect("CRC chunk is bounded by a usize-sized constant");
        let read = file
            .read(&mut buffer[..wanted])
            .map_err(|source| publication_io_error(path, source))?;
        if read == 0 {
            return invalid_publication(format!(
                "generation reached EOF with {remaining} proof bytes still expected"
            ));
        }
        hasher.update(&buffer[..read]);
        remaining -= read as u64;
    }

    let mut extra = [0_u8; 1];
    let trailing = file
        .read(&mut extra)
        .map_err(|source| publication_io_error(path, source))?;
    if trailing != 0 {
        return invalid_publication(
            "generation changed while its committed-prefix checksum was derived",
        );
    }

    Ok(CommittedPrefix {
        bytes: report.file_bytes,
        crc32: hasher.finalize(),
        record_count: report.record_count,
        next_sequence: report.next_sequence,
    })
}

#[cfg(windows)]
fn require_clean_generation(path: &Path) -> Result<VerificationReport, GenerationPublicationError> {
    let report = LogEngine::verify(path)?;
    if report.recoverable_tail.is_some() || report.file_bytes != report.valid_bytes {
        return invalid_publication(
            "generation must be a complete clean append-log image before marker publication",
        );
    }
    Ok(report)
}

#[cfg(windows)]
fn require_exact_compact_generation(
    path: &Path,
    expected: &InspectionReport,
    proof: CommittedPrefix,
) -> Result<(), GenerationPublicationError> {
    let _ = verify_committed_prefix(path, proof)?;
    let current = LogEngine::inspect(path, true)?;
    if &current != expected {
        return invalid_publication(
            "generation changed while commit marker publication was in progress",
        );
    }
    Ok(())
}

#[cfg(windows)]
fn write_synced_staging(
    path: &Path,
    encoded: &[u8; COMMIT_MARKER_LEN],
) -> Result<(), GenerationPublicationError> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|source| publication_io_error(path, source))?;
    file.write_all(encoded)
        .map_err(|source| publication_io_error(path, source))?;
    file.sync_all()
        .map_err(|source| publication_io_error(path, source))
}

#[cfg(windows)]
fn verify_published_marker(
    path: &Path,
    generation: u64,
    committed_prefix: CommittedPrefix,
) -> Result<CommitMarker, GenerationPublicationError> {
    require_real_regular_file(path, "published commit marker").map_err(map_directory_error)?;
    let bytes = fs::read(path).map_err(|source| publication_io_error(path, source))?;
    let marker = decode_commit_marker(&bytes, generation).map_err(|error| {
        GenerationPublicationError::Invalid(format!("published commit marker: {error}"))
    })?;
    if marker.committed_prefix != committed_prefix {
        return invalid_publication("published commit marker prefix differs from staged proof");
    }
    Ok(marker)
}

#[cfg(windows)]
fn require_absent(path: &Path, label: &str) -> Result<(), GenerationPublicationError> {
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Ok(_) => invalid_publication(format!("{label} already exists: {}", path.display())),
        Err(source) => Err(publication_io_error(path, source)),
    }
}

#[cfg(windows)]
fn remove_stale_staging_if_safe(path: &Path) -> Result<(), GenerationPublicationError> {
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Ok(metadata) if metadata.file_type().is_file() => {
            fs::remove_file(path).map_err(|source| publication_io_error(path, source))
        }
        Ok(_) => invalid_publication(format!(
            "staging marker exists but is not a real regular file: {}",
            path.display()
        )),
        Err(source) => Err(publication_io_error(path, source)),
    }
}

#[cfg(windows)]
fn map_directory_error(error: GenerationDirectoryError) -> GenerationPublicationError {
    match error {
        GenerationDirectoryError::Invalid(message) => GenerationPublicationError::Invalid(message),
        GenerationDirectoryError::Database(error) => GenerationPublicationError::Database(error),
        GenerationDirectoryError::Io { path, source } => {
            GenerationPublicationError::Io { path, source }
        }
    }
}

#[cfg(windows)]
fn publication_io_error(path: &Path, source: io::Error) -> GenerationPublicationError {
    GenerationPublicationError::Io {
        path: path.to_path_buf(),
        source,
    }
}

#[cfg(windows)]
fn invalid_publication<T>(message: impl Into<String>) -> Result<T, GenerationPublicationError> {
    Err(GenerationPublicationError::Invalid(message.into()))
}
