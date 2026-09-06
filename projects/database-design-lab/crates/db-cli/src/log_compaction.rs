use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use db_core::{DbError, KvEngine};
use db_storage_log::{InspectionReport, LogEngine};
use serde::Serialize;
use thiserror::Error;

#[cfg(windows)]
use crate::windows_durable::move_no_replace_write_through;

pub const LOG_COMPACTION_PROTOCOL: &str = "append_log_compact_copy_v1";

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct LogCompactionReport {
    pub protocol: &'static str,
    pub file_format_version: u16,
    pub source_file_bytes: u64,
    pub source_record_count: u64,
    pub live_keys: usize,
    pub compacted_file_bytes: u64,
    pub compacted_record_count: u64,
    pub reclaimed_bytes: u64,
    pub staging_retained: bool,
}

#[derive(Debug, Error)]
pub enum LogCompactionError {
    #[error(transparent)]
    Database(#[from] DbError),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(
        "compaction output {output} became visible but Windows write-through publication returned an error: {source}; preserve the output as non-authoritative evidence and verify it before retrying"
    )]
    PublicationUncertain {
        output: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("invalid compaction request: {0}")]
    Invalid(String),
}

/// Builds and publishes a fresh non-destructive compact copy of a clean append-log file.
///
/// The source is always opened read-only. The output path must not already exist. The compacted
/// image is built under a sibling staging name and verified against the source live state. Unix
/// publishes the verified staging inode with the original no-overwrite hard-link contract. Windows
/// synchronizes the complete staging file and publishes the fresh output name with the audited
/// no-overwrite `MOVEFILE_WRITE_THROUGH` primitive. The published image is verified again before
/// staging cleanup when a second staging name remains.
pub fn compact_log_to_fresh_file(
    source: &Path,
    output: &Path,
) -> Result<LogCompactionReport, LogCompactionError> {
    let source = canonical_regular_file(source, "source append log")?;
    let output = canonical_fresh_output(output)?;
    if source == output {
        return invalid("source and output must be distinct paths");
    }
    let staging = staging_path(&output)?;
    if source == staging {
        return invalid("derived staging path aliases the source append log");
    }
    require_absent(&staging, "compaction staging path")?;

    let source_verification = LogEngine::verify(&source)?;
    if source_verification.recoverable_tail.is_some() {
        return invalid(
            "source has a recoverable incomplete final append; reopen/repair it explicitly before compaction",
        );
    }
    let source_inspection = LogEngine::inspect(&source, true)?;
    if source_inspection.verification != source_verification {
        return invalid("source changed between verification and replay inspection");
    }

    build_staging(&staging, &source_inspection)?;
    let compacted_inspection = match LogEngine::inspect(&staging, true) {
        Ok(report) => report,
        Err(error) => {
            let _ = fs::remove_file(&staging);
            return Err(error.into());
        }
    };
    if let Err(error) = validate_compacted_state(&source_inspection, &compacted_inspection) {
        let _ = fs::remove_file(&staging);
        return Err(error);
    }

    let source_after = match LogEngine::inspect(&source, true) {
        Ok(report) => report,
        Err(error) => {
            let _ = fs::remove_file(&staging);
            return Err(error.into());
        }
    };
    if source_after != source_inspection {
        let _ = fs::remove_file(&staging);
        return invalid("source changed while the compact copy was being constructed");
    }

    publish_staging(&staging, &output)?;

    let published = match LogEngine::inspect(&output, true) {
        Ok(report) if report == compacted_inspection => report,
        Ok(_) => {
            #[cfg(not(windows))]
            let _ = fs::remove_file(&output);
            let _ = fs::remove_file(&staging);
            return invalid("published compact output differs from its verified staging image");
        }
        Err(error) => {
            #[cfg(not(windows))]
            let _ = fs::remove_file(&output);
            let _ = fs::remove_file(&staging);
            return Err(error.into());
        }
    };

    let reclaimed_bytes = match source_verification
        .file_bytes
        .checked_sub(published.verification.file_bytes)
    {
        Some(bytes) => bytes,
        None => {
            #[cfg(not(windows))]
            let _ = fs::remove_file(&output);
            let _ = fs::remove_file(&staging);
            return invalid("compacted file is unexpectedly larger than its source append log");
        }
    };
    let staging_retained = match fs::remove_file(&staging) {
        Ok(()) => false,
        Err(error) if error.kind() == io::ErrorKind::NotFound => false,
        Err(_) => true,
    };

    Ok(LogCompactionReport {
        protocol: LOG_COMPACTION_PROTOCOL,
        file_format_version: published.verification.file_format_version,
        source_file_bytes: source_verification.file_bytes,
        source_record_count: source_verification.record_count,
        live_keys: source_verification.live_keys,
        compacted_file_bytes: published.verification.file_bytes,
        compacted_record_count: published.verification.record_count,
        reclaimed_bytes,
        staging_retained,
    })
}

fn publish_staging(staging: &Path, output: &Path) -> Result<(), LogCompactionError> {
    #[cfg(windows)]
    {
        use std::fs::OpenOptions;

        let file = OpenOptions::new()
            .write(true)
            .open(staging)
            .map_err(|source| io_error(staging, source))?;
        file.sync_all()
            .map_err(|source| io_error(staging, source))?;
        drop(file);

        if let Err(source) = move_no_replace_write_through(staging, output) {
            match fs::symlink_metadata(output) {
                Ok(_) => {
                    return Err(LogCompactionError::PublicationUncertain {
                        output: output.to_path_buf(),
                        source,
                    });
                }
                Err(error) if error.kind() == io::ErrorKind::NotFound => {
                    let _ = fs::remove_file(staging);
                    return Err(io_error(output, source));
                }
                Err(_) => {
                    return Err(LogCompactionError::PublicationUncertain {
                        output: output.to_path_buf(),
                        source,
                    });
                }
            }
        }
        Ok(())
    }

    #[cfg(not(windows))]
    {
        if let Err(error) = fs::hard_link(staging, output) {
            let _ = fs::remove_file(staging);
            return Err(io_error(output, error));
        }
        Ok(())
    }
}

fn build_staging(path: &Path, source: &InspectionReport) -> Result<(), LogCompactionError> {
    let result = (|| {
        let mut compacted = LogEngine::create_new(path)?;
        for entry in &source.entries {
            let value = entry.value.as_ref().ok_or_else(|| {
                LogCompactionError::Invalid(
                    "internal compaction inspection omitted a requested live value".to_owned(),
                )
            })?;
            compacted.put(entry.key.as_slice(), value.as_slice())?;
        }
        Ok::<(), LogCompactionError>(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(path);
    }
    result
}

fn validate_compacted_state(
    source: &InspectionReport,
    compacted: &InspectionReport,
) -> Result<(), LogCompactionError> {
    if compacted.verification.recoverable_tail.is_some() {
        return invalid("compaction staging unexpectedly contains a recoverable tail");
    }
    if compacted.verification.live_keys != source.verification.live_keys
        || compacted.verification.record_count
            != u64::try_from(source.entries.len()).map_err(|_| {
                LogCompactionError::Invalid("live-key count does not fit u64".to_owned())
            })?
        || compacted.entries != source.entries
    {
        return invalid("compaction staging does not exactly reproduce the source live state");
    }
    Ok(())
}

fn canonical_regular_file(path: &Path, label: &str) -> Result<PathBuf, LogCompactionError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_file() {
        return invalid(format!(
            "{label} must be a real regular file rather than a symlink or non-file"
        ));
    }
    fs::canonicalize(path).map_err(|source| io_error(path, source))
}

fn canonical_fresh_output(path: &Path) -> Result<PathBuf, LogCompactionError> {
    require_absent(path, "compaction output")?;
    let file_name = path.file_name().ok_or_else(|| {
        LogCompactionError::Invalid(format!(
            "compaction output has no final path component: {}",
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
            "compaction output parent must be a real directory: {}",
            parent.display()
        ));
    }
    let parent = fs::canonicalize(parent).map_err(|source| io_error(parent, source))?;
    Ok(parent.join(file_name))
}

fn staging_path(output: &Path) -> Result<PathBuf, LogCompactionError> {
    let file_name = output.file_name().ok_or_else(|| {
        LogCompactionError::Invalid(format!(
            "compaction output has no final path component: {}",
            output.display()
        ))
    })?;
    let mut staging_name = OsString::from(".");
    staging_name.push(file_name);
    staging_name.push(".compacting");
    Ok(output.with_file_name(staging_name))
}

fn require_absent(path: &Path, label: &str) -> Result<(), LogCompactionError> {
    match fs::symlink_metadata(path) {
        Ok(_) => invalid(format!("{label} already exists: {}", path.display())),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(source) => Err(io_error(path, source)),
    }
}

fn io_error(path: &Path, source: io::Error) -> LogCompactionError {
    LogCompactionError::Io {
        path: path.to_path_buf(),
        source,
    }
}

fn invalid<T>(message: impl Into<String>) -> Result<T, LogCompactionError> {
    Err(LogCompactionError::Invalid(message.into()))
}
