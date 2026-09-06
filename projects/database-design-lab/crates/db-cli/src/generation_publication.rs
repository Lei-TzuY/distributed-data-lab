use std::io;
use std::path::{Path, PathBuf};

use db_core::DbError;
use serde::Serialize;
use thiserror::Error;

use crate::generation_marker::CommittedPrefix;
use crate::generation_prefix::CommittedPrefixVerifyError;

pub const GENERATION_MARKER_PUBLICATION_PROTOCOL: &str =
    "append_log_generation_marker_publication_unix_v1";

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct GenerationPublicationSummary {
    pub protocol: &'static str,
    pub marker_format_version: u16,
    pub generation: u64,
    pub generation_log: String,
    pub marker: String,
    pub committed_prefix: CommittedPrefix,
    pub staging_retained: bool,
}

#[derive(Debug, Error)]
pub enum GenerationPublicationError {
    #[error("invalid generation marker publication: {0}")]
    Invalid(String),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error(transparent)]
    Prefix(#[from] CommittedPrefixVerifyError),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "commit marker {marker} is visible but parent-directory durability could not be confirmed: {source}; preserve the old generation and treat recovery as authoritative before retrying"
    )]
    DurabilityUncertain {
        marker: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "durable append-log generation marker publication is unsupported on this platform; no marker was written"
    )]
    UnsupportedPlatform,
}

/// Durably publishes a marker that makes an existing clean generation authoritative.
///
/// Unix hosts use the repository's marker-v2 durability protocol. Other platforms fail before
/// touching the supplied path because this repository does not yet claim an equivalent
/// parent-directory durability barrier there.
pub fn publish_generation_marker(
    directory: &Path,
    generation: u64,
) -> Result<GenerationPublicationSummary, GenerationPublicationError> {
    #[cfg(unix)]
    {
        unix::publish_generation_marker(
            directory,
            generation,
            #[cfg(test)]
            None,
        )
    }

    #[cfg(not(unix))]
    {
        let _ = (directory, generation);
        Err(GenerationPublicationError::UnsupportedPlatform)
    }
}

#[cfg(all(test, unix))]
pub(crate) use unix::{publish_generation_marker_with_fault, GenerationPublicationFaultPoint};

#[cfg(unix)]
mod unix {
    use std::fs::{self, File, OpenOptions};
    use std::io::{Read, Write};

    use db_storage_log::{LogEngine, VerificationReport};

    use super::*;
    use crate::generation_directory::{
        canonical_generation_name, canonical_marker_name, canonical_real_directory,
        canonical_staging_marker_name, require_real_regular_file, scan_generation_namespace,
        GenerationDirectoryError,
    };
    use crate::generation_marker::{
        decode_commit_marker, encode_commit_marker, CommitMarker, Crc32Ieee, COMMIT_MARKER_LEN,
        COMMIT_MARKER_VERSION,
    };
    use crate::generation_prefix::verify_committed_prefix;

    const CRC_BUFFER_BYTES: usize = 64 * 1024;

    #[cfg(test)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub(crate) enum GenerationPublicationFaultPoint {
        GenerationDurable,
        StagingMarkerPartiallyWritten,
        StagingMarkerDurable,
        FinalMarkerLinked,
        FinalDirectoryDurable,
    }

    #[cfg(test)]
    impl GenerationPublicationFaultPoint {
        const fn label(self) -> &'static str {
            match self {
                Self::GenerationDurable => "generation_durable",
                Self::StagingMarkerPartiallyWritten => "staging_marker_partially_written",
                Self::StagingMarkerDurable => "staging_marker_durable",
                Self::FinalMarkerLinked => "final_marker_linked",
                Self::FinalDirectoryDurable => "final_directory_durable",
            }
        }
    }

    #[cfg(test)]
    pub(crate) fn publish_generation_marker_with_fault(
        directory: &Path,
        generation: u64,
        fault: Option<GenerationPublicationFaultPoint>,
    ) -> Result<GenerationPublicationSummary, GenerationPublicationError> {
        publish_generation_marker(directory, generation, fault)
    }

    pub(super) fn publish_generation_marker(
        directory: &Path,
        generation: u64,
        #[cfg(test)] fault: Option<GenerationPublicationFaultPoint>,
    ) -> Result<GenerationPublicationSummary, GenerationPublicationError> {
        if generation == 0 {
            return invalid("generation id must be greater than zero");
        }

        let directory = canonical_real_directory(directory).map_err(map_directory_error)?;
        let namespace = scan_generation_namespace(&directory).map_err(map_directory_error)?;
        if namespace.marker_files.keys().any(|id| *id >= generation) {
            return invalid(format!(
                "generation {generation} is not newer than every existing committed generation"
            ));
        }

        let log_path = directory.join(canonical_generation_name(generation));
        let marker_path = directory.join(canonical_marker_name(generation));
        let staging_path = directory.join(canonical_staging_marker_name(generation));
        require_real_regular_file(&log_path, "generation log").map_err(map_directory_error)?;
        require_absent(&marker_path, "commit marker")?;
        remove_stale_staging_if_safe(&staging_path)?;

        let baseline = require_clean_generation(&log_path)?;
        let committed_prefix = derive_prefix_proof(&log_path, &baseline)?;
        let prefix_verification = verify_committed_prefix(&log_path, committed_prefix)?;
        if prefix_verification != baseline {
            return invalid(
                "derived committed-prefix verification disagrees with clean generation",
            );
        }

        sync_regular_file(&log_path)?;
        sync_directory(&directory).map_err(|source| io_error(&directory, source))?;
        require_exact_clean_generation(&log_path, &baseline, committed_prefix)?;
        #[cfg(test)]
        inject_fault(fault, GenerationPublicationFaultPoint::GenerationDurable)?;

        let encoded = encode_commit_marker(generation, committed_prefix).map_err(|error| {
            GenerationPublicationError::Invalid(format!("cannot encode commit marker: {error}"))
        })?;
        write_synced_staging(
            &staging_path,
            &encoded,
            #[cfg(test)]
            fault,
        )?;
        #[cfg(test)]
        inject_fault(fault, GenerationPublicationFaultPoint::StagingMarkerDurable)?;

        require_exact_clean_generation(&log_path, &baseline, committed_prefix)?;
        fs::hard_link(&staging_path, &marker_path)
            .map_err(|source| io_error(&marker_path, source))?;
        #[cfg(test)]
        inject_fault(fault, GenerationPublicationFaultPoint::FinalMarkerLinked)?;

        if let Err(source) = sync_directory(&directory) {
            return Err(GenerationPublicationError::DurabilityUncertain {
                marker: marker_path,
                source,
            });
        }
        #[cfg(test)]
        inject_fault(
            fault,
            GenerationPublicationFaultPoint::FinalDirectoryDurable,
        )?;

        verify_published_marker(&marker_path, generation, committed_prefix)?;
        let _ = verify_committed_prefix(&log_path, committed_prefix)?;

        let staging_retained = match fs::remove_file(&staging_path) {
            Ok(()) => false,
            Err(error) if error.kind() == io::ErrorKind::NotFound => false,
            Err(_) => true,
        };

        Ok(GenerationPublicationSummary {
            protocol: GENERATION_MARKER_PUBLICATION_PROTOCOL,
            marker_format_version: COMMIT_MARKER_VERSION,
            generation,
            generation_log: canonical_generation_name(generation),
            marker: canonical_marker_name(generation),
            committed_prefix,
            staging_retained,
        })
    }

    fn derive_prefix_proof(
        path: &Path,
        report: &VerificationReport,
    ) -> Result<CommittedPrefix, GenerationPublicationError> {
        let mut file = File::open(path).map_err(|source| io_error(path, source))?;
        let mut remaining = report.file_bytes;
        let mut hasher = Crc32Ieee::new();
        let mut buffer = [0_u8; CRC_BUFFER_BYTES];

        while remaining > 0 {
            let wanted = usize::try_from(remaining.min(CRC_BUFFER_BYTES as u64))
                .expect("CRC chunk is bounded by a usize-sized constant");
            let read = file
                .read(&mut buffer[..wanted])
                .map_err(|source| io_error(path, source))?;
            if read == 0 {
                return invalid(format!(
                    "generation reached EOF with {remaining} proof bytes still expected"
                ));
            }
            hasher.update(&buffer[..read]);
            remaining -= read as u64;
        }

        let mut extra = [0_u8; 1];
        let trailing = file
            .read(&mut extra)
            .map_err(|source| io_error(path, source))?;
        if trailing != 0 {
            return invalid("generation changed while its committed-prefix checksum was derived");
        }

        Ok(CommittedPrefix {
            bytes: report.file_bytes,
            crc32: hasher.finalize(),
            record_count: report.record_count,
            next_sequence: report.next_sequence,
        })
    }

    fn require_clean_generation(
        path: &Path,
    ) -> Result<VerificationReport, GenerationPublicationError> {
        let report = LogEngine::verify(path)?;
        if report.recoverable_tail.is_some() || report.file_bytes != report.valid_bytes {
            return invalid(
                "generation must be a complete clean append-log image before marker publication",
            );
        }
        Ok(report)
    }

    fn require_exact_clean_generation(
        path: &Path,
        baseline: &VerificationReport,
        proof: CommittedPrefix,
    ) -> Result<(), GenerationPublicationError> {
        let _ = verify_committed_prefix(path, proof)?;
        let current = require_clean_generation(path)?;
        if &current != baseline {
            return invalid("generation changed while commit marker publication was in progress");
        }
        Ok(())
    }

    fn write_synced_staging(
        path: &Path,
        encoded: &[u8; COMMIT_MARKER_LEN],
        #[cfg(test)] fault: Option<GenerationPublicationFaultPoint>,
    ) -> Result<(), GenerationPublicationError> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
            .map_err(|source| io_error(path, source))?;
        #[cfg(not(test))]
        file.write_all(encoded)
            .map_err(|source| io_error(path, source))?;
        #[cfg(test)]
        {
            let split = encoded.len() / 2;
            file.write_all(&encoded[..split])
                .map_err(|source| io_error(path, source))?;
            inject_fault(
                fault,
                GenerationPublicationFaultPoint::StagingMarkerPartiallyWritten,
            )?;
            file.write_all(&encoded[split..])
                .map_err(|source| io_error(path, source))?;
        }
        file.sync_all().map_err(|source| io_error(path, source))
    }

    #[cfg(test)]
    fn inject_fault(
        selected: Option<GenerationPublicationFaultPoint>,
        current: GenerationPublicationFaultPoint,
    ) -> Result<(), GenerationPublicationError> {
        if selected == Some(current) {
            return invalid(format!(
                "injected generation publication fault at {}",
                current.label()
            ));
        }
        Ok(())
    }

    fn sync_regular_file(path: &Path) -> Result<(), GenerationPublicationError> {
        let file = File::open(path).map_err(|source| io_error(path, source))?;
        file.sync_all().map_err(|source| io_error(path, source))
    }

    fn sync_directory(path: &Path) -> io::Result<()> {
        File::open(path)?.sync_all()
    }

    fn verify_published_marker(
        path: &Path,
        generation: u64,
        committed_prefix: CommittedPrefix,
    ) -> Result<CommitMarker, GenerationPublicationError> {
        require_real_regular_file(path, "published commit marker").map_err(map_directory_error)?;
        let bytes = fs::read(path).map_err(|source| io_error(path, source))?;
        let marker = decode_commit_marker(&bytes, generation).map_err(|error| {
            GenerationPublicationError::Invalid(format!("published commit marker: {error}"))
        })?;
        if marker.committed_prefix != committed_prefix {
            return invalid("published commit marker prefix differs from staged proof");
        }
        Ok(marker)
    }

    fn require_absent(path: &Path, label: &str) -> Result<(), GenerationPublicationError> {
        match fs::symlink_metadata(path) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Ok(_) => invalid(format!("{label} already exists: {}", path.display())),
            Err(source) => Err(io_error(path, source)),
        }
    }

    fn remove_stale_staging_if_safe(path: &Path) -> Result<(), GenerationPublicationError> {
        match fs::symlink_metadata(path) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Ok(metadata) if metadata.file_type().is_file() => {
                fs::remove_file(path).map_err(|source| io_error(path, source))
            }
            Ok(_) => invalid(format!(
                "staging marker exists but is not a real regular file: {}",
                path.display()
            )),
            Err(source) => Err(io_error(path, source)),
        }
    }

    fn map_directory_error(error: GenerationDirectoryError) -> GenerationPublicationError {
        match error {
            GenerationDirectoryError::Invalid(message) => {
                GenerationPublicationError::Invalid(message)
            }
            GenerationDirectoryError::Database(error) => {
                GenerationPublicationError::Database(error)
            }
            GenerationDirectoryError::Io { path, source } => {
                GenerationPublicationError::Io { path, source }
            }
        }
    }

    fn io_error(path: &Path, source: io::Error) -> GenerationPublicationError {
        GenerationPublicationError::Io {
            path: path.to_path_buf(),
            source,
        }
    }

    fn invalid<T>(message: impl Into<String>) -> Result<T, GenerationPublicationError> {
        Err(GenerationPublicationError::Invalid(message.into()))
    }
}
