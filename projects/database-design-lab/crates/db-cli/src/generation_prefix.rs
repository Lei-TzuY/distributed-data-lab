use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

use db_core::DbError;
use db_storage_log::{LogEngine, VerificationReport};
use tempfile::NamedTempFile;
use thiserror::Error;

use crate::generation_marker::{CommittedPrefix, Crc32Ieee};

const COPY_BUFFER_BYTES: usize = 64 * 1024;

#[derive(Debug, Error)]
pub enum CommittedPrefixVerifyError {
    #[error("invalid committed prefix proof: {0}")]
    Invalid(String),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error("source I/O error at {path}: {source}")]
    SourceIo {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("temporary prefix verification I/O error: {0}")]
    TemporaryIo(io::Error),
}

/// Re-verifies the exact marker-bound prefix without mutating the source log.
///
/// The prefix is streamed into a temporary verification file so the canonical append-log parser
/// remains the single structural authority. The source generation file is opened read-only.
pub fn verify_committed_prefix(
    path: &Path,
    proof: CommittedPrefix,
) -> Result<VerificationReport, CommittedPrefixVerifyError> {
    let metadata = fs::metadata(path).map_err(|source| source_io(path, source))?;
    if metadata.len() < proof.bytes {
        return invalid(format!(
            "marker binds {} bytes but source file has only {} bytes",
            proof.bytes,
            metadata.len()
        ));
    }

    let mut source = File::open(path).map_err(|error| source_io(path, error))?;
    let mut temporary = NamedTempFile::new().map_err(CommittedPrefixVerifyError::TemporaryIo)?;
    let mut remaining = proof.bytes;
    let mut crc = Crc32Ieee::new();
    let mut buffer = [0_u8; COPY_BUFFER_BYTES];

    while remaining > 0 {
        let wanted = usize::try_from(remaining.min(COPY_BUFFER_BYTES as u64))
            .expect("copy chunk is bounded by a usize-sized constant");
        let read = source
            .read(&mut buffer[..wanted])
            .map_err(|error| source_io(path, error))?;
        if read == 0 {
            return invalid(format!(
                "source reached EOF with {remaining} marker-bound prefix bytes still expected"
            ));
        }
        crc.update(&buffer[..read]);
        temporary
            .as_file_mut()
            .write_all(&buffer[..read])
            .map_err(CommittedPrefixVerifyError::TemporaryIo)?;
        remaining -= read as u64;
    }

    let computed_crc = crc.finalize();
    if computed_crc != proof.crc32 {
        return invalid(format!(
            "marker-bound prefix checksum mismatch: marker {:08x}, computed {computed_crc:08x}",
            proof.crc32
        ));
    }

    temporary
        .as_file_mut()
        .flush()
        .map_err(CommittedPrefixVerifyError::TemporaryIo)?;
    let temporary_path = temporary.into_temp_path();
    let verification = LogEngine::verify(&temporary_path)?;

    if verification.recoverable_tail.is_some() {
        return invalid(
            "marker-bound prefix is not a complete append-log image; it ends in a recoverable tail",
        );
    }
    if verification.file_bytes != proof.bytes || verification.valid_bytes != proof.bytes {
        return invalid(format!(
            "marker-bound prefix structural length mismatch: marker {}, file {}, valid {}",
            proof.bytes, verification.file_bytes, verification.valid_bytes
        ));
    }
    if verification.record_count != proof.record_count {
        return invalid(format!(
            "marker-bound prefix record count mismatch: marker {}, verified {}",
            proof.record_count, verification.record_count
        ));
    }
    if verification.next_sequence != proof.next_sequence {
        return invalid(format!(
            "marker-bound prefix next sequence mismatch: marker {}, verified {}",
            proof.next_sequence, verification.next_sequence
        ));
    }

    Ok(verification)
}

fn source_io(path: &Path, source: io::Error) -> CommittedPrefixVerifyError {
    CommittedPrefixVerifyError::SourceIo {
        path: path.to_path_buf(),
        source,
    }
}

fn invalid<T>(message: impl Into<String>) -> Result<T, CommittedPrefixVerifyError> {
    Err(CommittedPrefixVerifyError::Invalid(message.into()))
}

#[cfg(test)]
mod tests {
    use std::fs;

    use db_core::KvEngine;
    use db_storage_log::LogEngine;
    use tempfile::tempdir;

    use super::*;
    use crate::generation_marker::crc32_ieee;

    #[test]
    fn complete_historical_prefix_is_verified_without_touching_later_appends() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("generation.log");
        let prefix_report = {
            let mut engine = LogEngine::create_new(&path).expect("create generation");
            engine.put(b"a", b"one").expect("write compacted base");
            drop(engine);
            LogEngine::verify(&path).expect("verify compacted base")
        };
        let prefix_bytes = fs::read(&path).expect("read compacted base");
        let proof = CommittedPrefix {
            bytes: prefix_report.file_bytes,
            crc32: crc32_ieee(&prefix_bytes),
            record_count: prefix_report.record_count,
            next_sequence: prefix_report.next_sequence,
        };

        let mut engine = LogEngine::open(&path).expect("reopen after commit");
        engine.put(b"b", b"two").expect("post-commit append");
        drop(engine);

        let verified = verify_committed_prefix(&path, proof).expect("verify historical prefix");
        assert_eq!(verified.file_bytes, proof.bytes);
        assert_eq!(verified.record_count, 1);
        assert_eq!(verified.next_sequence, 2);
        assert!(verified.recoverable_tail.is_none());
    }

    #[test]
    fn prefix_cut_inside_a_record_is_rejected_even_with_matching_crc() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("generation.log");
        {
            let mut engine = LogEngine::create_new(&path).expect("create generation");
            engine.put(b"key", b"value").expect("put");
        }
        let bytes = fs::read(&path).expect("read generation");
        let cut = bytes.len() - 1;
        let proof = CommittedPrefix {
            bytes: cut as u64,
            crc32: crc32_ieee(&bytes[..cut]),
            record_count: 1,
            next_sequence: 2,
        };

        let error = verify_committed_prefix(&path, proof).expect_err("cut prefix must fail");
        assert!(error.to_string().contains("recoverable tail"));
    }

    #[test]
    fn prefix_checksum_mismatch_is_rejected_before_structural_admission() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("generation.log");
        {
            let mut engine = LogEngine::create_new(&path).expect("create generation");
            engine.put(b"key", b"value").expect("put");
        }
        let report = LogEngine::verify(&path).expect("verify generation");
        let proof = CommittedPrefix {
            bytes: report.file_bytes,
            crc32: 0,
            record_count: report.record_count,
            next_sequence: report.next_sequence,
        };

        let error = verify_committed_prefix(&path, proof).expect_err("wrong checksum must fail");
        assert!(error.to_string().contains("checksum mismatch"));
    }
}
