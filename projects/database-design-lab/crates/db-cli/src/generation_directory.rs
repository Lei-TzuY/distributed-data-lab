use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use db_core::DbError;
use db_storage_log::{LogEngine, VerificationReport};
use serde::Serialize;
use thiserror::Error;

use crate::generation_marker::{
    decode_commit_marker, CommitMarker, CommittedPrefix, APPEND_LOG_FORMAT_VERSION,
    COMMIT_MARKER_LEN, COMMIT_MARKER_VERSION,
};
use crate::generation_prefix::verify_committed_prefix;

pub const GENERATION_DIRECTORY_PROTOCOL: &str = "append_log_generation_directory_v3";
pub const GENERATION_PREFIX: &str = "generation-";
pub const GENERATION_SUFFIX: &str = ".log";
pub const COMMIT_PREFIX: &str = "commit-";
pub const COMMIT_SUFFIX: &str = ".marker";
pub const STAGING_COMMIT_PREFIX: &str = "staging-commit-";
pub const RESERVATION_PREFIX: &str = "reserve-";
pub const RESERVATION_SUFFIX: &str = ".frontier";
pub const GENERATION_ID_WIDTH: usize = 20;
pub const MAX_DIRECTORY_ENTRIES: usize = 8_192;

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct GenerationVerificationSummary {
    pub protocol: &'static str,
    pub marker_format_version: u16,
    pub authoritative_generation: u64,
    pub authoritative_log: String,
    pub highest_observed_generation: u64,
    pub marker_generation_ids: Vec<u64>,
    pub staging_marker_generation_ids: Vec<u64>,
    pub reservation_generation_ids: Vec<u64>,
    pub uncommitted_generation_ids: Vec<u64>,
    pub committed_prefix: CommittedPrefix,
    pub committed_prefix_verification: VerificationReport,
    pub log_verification: VerificationReport,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VerifiedGenerationDirectory {
    directory: PathBuf,
    summary: GenerationVerificationSummary,
}

impl VerifiedGenerationDirectory {
    pub fn directory(&self) -> &Path {
        &self.directory
    }

    pub fn summary(&self) -> &GenerationVerificationSummary {
        &self.summary
    }

    pub fn authoritative_log_path(&self) -> PathBuf {
        self.directory.join(&self.summary.authoritative_log)
    }

    pub fn next_generation_id(&self) -> Result<u64, GenerationDirectoryError> {
        self.summary
            .highest_observed_generation
            .checked_add(1)
            .ok_or_else(|| {
                GenerationDirectoryError::Invalid(
                    "cannot allocate a generation after u64::MAX".to_owned(),
                )
            })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GenerationNamespace {
    pub generation_files: BTreeMap<u64, PathBuf>,
    pub marker_files: BTreeMap<u64, PathBuf>,
    pub staging_marker_files: BTreeMap<u64, PathBuf>,
    pub reservation_files: BTreeMap<u64, PathBuf>,
}

impl GenerationNamespace {
    pub fn highest_observed_generation(&self) -> Option<u64> {
        self.generation_files
            .keys()
            .chain(self.marker_files.keys())
            .chain(self.staging_marker_files.keys())
            .chain(self.reservation_files.keys())
            .copied()
            .max()
    }
}

#[derive(Debug, Error)]
pub enum GenerationDirectoryError {
    #[error("invalid generation directory: {0}")]
    Invalid(String),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
}

pub fn verify_generation_directory(
    directory: &Path,
) -> Result<VerifiedGenerationDirectory, GenerationDirectoryError> {
    let directory = canonical_real_directory(directory)?;
    let namespace = scan_generation_namespace(&directory)?;

    let Some((&authoritative_generation, marker_path)) = namespace.marker_files.last_key_value()
    else {
        return invalid("no committed generation marker exists");
    };
    let marker = read_commit_marker(marker_path, authoritative_generation)?;
    if marker.log_format_version != APPEND_LOG_FORMAT_VERSION {
        return invalid(format!(
            "highest commit marker requires append-log format {}, expected {APPEND_LOG_FORMAT_VERSION}",
            marker.log_format_version
        ));
    }

    let log_path = namespace
        .generation_files
        .get(&authoritative_generation)
        .ok_or_else(|| {
            GenerationDirectoryError::Invalid(format!(
                "highest committed generation {authoritative_generation} has no generation log"
            ))
        })?;
    require_real_regular_file(log_path, "authoritative generation log")?;
    let log_verification = LogEngine::verify(log_path)?;
    if log_verification.file_format_version != marker.log_format_version {
        return invalid(format!(
            "authoritative log format {} disagrees with marker format {}",
            log_verification.file_format_version, marker.log_format_version
        ));
    }

    let committed_prefix_verification = verify_committed_prefix(log_path, marker.committed_prefix)
        .map_err(|error| {
            GenerationDirectoryError::Invalid(format!(
                "highest commit marker prefix proof failed: {error}"
            ))
        })?;

    if log_verification.valid_bytes < marker.committed_prefix.bytes {
        return invalid(format!(
            "authoritative log has only {} structurally valid bytes before its tail, but marker binds {} committed prefix bytes",
            log_verification.valid_bytes, marker.committed_prefix.bytes
        ));
    }
    if let Some(tail) = &log_verification.recoverable_tail {
        if tail.record_offset < marker.committed_prefix.bytes {
            return invalid(format!(
                "authoritative recoverable tail starts at byte {}, inside marker-bound committed prefix ending at byte {}",
                tail.record_offset, marker.committed_prefix.bytes
            ));
        }
    }

    let highest_observed_generation = namespace.highest_observed_generation().ok_or_else(|| {
        GenerationDirectoryError::Invalid("generation directory is empty".to_owned())
    })?;
    let marker_generation_ids = namespace.marker_files.keys().copied().collect();
    let staging_marker_generation_ids = namespace.staging_marker_files.keys().copied().collect();
    let reservation_generation_ids = namespace.reservation_files.keys().copied().collect();
    let uncommitted_generation_ids = namespace
        .generation_files
        .keys()
        .filter(|id| !namespace.marker_files.contains_key(*id))
        .copied()
        .collect();
    let authoritative_log = log_path
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or_else(|| {
            GenerationDirectoryError::Invalid(
                "authoritative generation filename is not UTF-8".to_owned(),
            )
        })?
        .to_owned();

    Ok(VerifiedGenerationDirectory {
        directory,
        summary: GenerationVerificationSummary {
            protocol: GENERATION_DIRECTORY_PROTOCOL,
            marker_format_version: COMMIT_MARKER_VERSION,
            authoritative_generation,
            authoritative_log,
            highest_observed_generation,
            marker_generation_ids,
            staging_marker_generation_ids,
            reservation_generation_ids,
            uncommitted_generation_ids,
            committed_prefix: marker.committed_prefix,
            committed_prefix_verification,
            log_verification,
        },
    })
}

pub fn scan_generation_namespace(
    directory: &Path,
) -> Result<GenerationNamespace, GenerationDirectoryError> {
    let mut generation_files = BTreeMap::new();
    let mut marker_files = BTreeMap::new();
    let mut staging_marker_files = BTreeMap::new();
    let mut reservation_files = BTreeMap::new();
    let entries = fs::read_dir(directory).map_err(|source| io_error(directory, source))?;

    for (index, entry) in entries.enumerate() {
        if index >= MAX_DIRECTORY_ENTRIES {
            return invalid(format!(
                "generation directory contains more than {MAX_DIRECTORY_ENTRIES} entries"
            ));
        }
        let entry = entry.map_err(|source| io_error(directory, source))?;
        let name = entry.file_name();
        let name = name.to_str().ok_or_else(|| {
            GenerationDirectoryError::Invalid(
                "generation directory contains a non-UTF-8 entry name".to_owned(),
            )
        })?;
        let path = entry.path();

        if let Some(id) = parse_canonical_generation_name(name)? {
            let _ = generation_files.insert(id, path);
            continue;
        }
        if let Some(id) = parse_canonical_commit_name(name)? {
            let _ = marker_files.insert(id, path);
            continue;
        }
        if let Some(id) = parse_canonical_staging_commit_name(name)? {
            let _ = staging_marker_files.insert(id, path);
            continue;
        }
        if let Some(id) = parse_canonical_reservation_name(name)? {
            require_empty_reservation_file(&path)?;
            let _ = reservation_files.insert(id, path);
            continue;
        }
        return invalid(format!("unexpected generation directory entry {name:?}"));
    }

    Ok(GenerationNamespace {
        generation_files,
        marker_files,
        staging_marker_files,
        reservation_files,
    })
}

pub fn parse_canonical_generation_name(
    name: &str,
) -> Result<Option<u64>, GenerationDirectoryError> {
    parse_canonical_id(name, GENERATION_PREFIX, GENERATION_SUFFIX, "generation log")
}

pub fn parse_canonical_commit_name(name: &str) -> Result<Option<u64>, GenerationDirectoryError> {
    parse_canonical_id(name, COMMIT_PREFIX, COMMIT_SUFFIX, "commit marker")
}

pub fn parse_canonical_staging_commit_name(
    name: &str,
) -> Result<Option<u64>, GenerationDirectoryError> {
    parse_canonical_id(
        name,
        STAGING_COMMIT_PREFIX,
        COMMIT_SUFFIX,
        "staging commit marker",
    )
}

pub fn parse_canonical_reservation_name(
    name: &str,
) -> Result<Option<u64>, GenerationDirectoryError> {
    parse_canonical_id(
        name,
        RESERVATION_PREFIX,
        RESERVATION_SUFFIX,
        "generation reservation",
    )
}

pub fn canonical_generation_name(id: u64) -> String {
    format!("{GENERATION_PREFIX}{id:020}{GENERATION_SUFFIX}")
}

pub fn canonical_marker_name(id: u64) -> String {
    format!("{COMMIT_PREFIX}{id:020}{COMMIT_SUFFIX}")
}

pub fn canonical_staging_marker_name(id: u64) -> String {
    format!("{STAGING_COMMIT_PREFIX}{id:020}{COMMIT_SUFFIX}")
}

pub fn canonical_reservation_name(id: u64) -> String {
    format!("{RESERVATION_PREFIX}{id:020}{RESERVATION_SUFFIX}")
}

pub fn canonical_real_directory(path: &Path) -> Result<PathBuf, GenerationDirectoryError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_dir() {
        return invalid(format!(
            "generation directory must be a real directory rather than a symlink or non-directory: {}",
            path.display()
        ));
    }
    fs::canonicalize(path).map_err(|source| io_error(path, source))
}

pub fn require_real_regular_file(path: &Path, label: &str) -> Result<(), GenerationDirectoryError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_file() {
        return invalid(format!(
            "{label} must be a real regular file rather than a symlink or non-file: {}",
            path.display()
        ));
    }
    Ok(())
}

fn require_empty_reservation_file(path: &Path) -> Result<(), GenerationDirectoryError> {
    require_real_regular_file(path, "generation reservation")?;
    let metadata = fs::metadata(path).map_err(|source| io_error(path, source))?;
    if metadata.len() != 0 {
        return invalid(format!(
            "generation reservation must contain zero bytes: {}",
            path.display()
        ));
    }
    Ok(())
}

fn parse_canonical_id(
    name: &str,
    prefix: &str,
    suffix: &str,
    kind: &str,
) -> Result<Option<u64>, GenerationDirectoryError> {
    if !name.starts_with(prefix) {
        return Ok(None);
    }
    let expected_len = prefix
        .len()
        .checked_add(GENERATION_ID_WIDTH)
        .and_then(|len| len.checked_add(suffix.len()))
        .ok_or_else(|| {
            GenerationDirectoryError::Invalid("canonical name length overflowed usize".to_owned())
        })?;
    if name.len() != expected_len || !name.ends_with(suffix) {
        return invalid(format!("malformed canonical {kind} name {name:?}"));
    }
    let digits = &name[prefix.len()..prefix.len() + GENERATION_ID_WIDTH];
    if !digits.bytes().all(|byte| byte.is_ascii_digit()) {
        return invalid(format!(
            "malformed canonical {kind} generation id in {name:?}"
        ));
    }
    let id = digits.parse::<u64>().map_err(|_| {
        GenerationDirectoryError::Invalid(format!("{kind} generation id does not fit u64"))
    })?;
    if id == 0 || format!("{id:020}") != digits {
        return invalid(format!("non-canonical {kind} generation id in {name:?}"));
    }
    Ok(Some(id))
}

fn read_commit_marker(
    path: &Path,
    filename_generation: u64,
) -> Result<CommitMarker, GenerationDirectoryError> {
    require_real_regular_file(path, "highest commit marker")?;
    let metadata = fs::metadata(path).map_err(|source| io_error(path, source))?;
    if metadata.len() != COMMIT_MARKER_LEN as u64 {
        return invalid(format!(
            "highest commit marker has {} bytes, expected {COMMIT_MARKER_LEN}",
            metadata.len()
        ));
    }

    let mut file = File::open(path).map_err(|source| io_error(path, source))?;
    let mut bytes = [0_u8; COMMIT_MARKER_LEN];
    file.read_exact(&mut bytes)
        .map_err(|source| io_error(path, source))?;
    decode_commit_marker(&bytes, filename_generation).map_err(|error| {
        GenerationDirectoryError::Invalid(format!("highest commit marker {error}"))
    })
}

fn io_error(path: &Path, source: io::Error) -> GenerationDirectoryError {
    GenerationDirectoryError::Io {
        path: path.to_path_buf(),
        source,
    }
}

fn invalid<T>(message: impl Into<String>) -> Result<T, GenerationDirectoryError> {
    Err(GenerationDirectoryError::Invalid(message.into()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canonical_names_are_strict() {
        assert_eq!(
            parse_canonical_generation_name("generation-00000000000000000042.log")
                .expect("parse generation"),
            Some(42)
        );
        assert_eq!(
            parse_canonical_commit_name("commit-00000000000000000042.marker")
                .expect("parse marker"),
            Some(42)
        );
        assert_eq!(
            parse_canonical_staging_commit_name("staging-commit-00000000000000000042.marker")
                .expect("parse staging marker"),
            Some(42)
        );
        assert_eq!(
            parse_canonical_reservation_name("reserve-00000000000000000042.frontier")
                .expect("parse reservation"),
            Some(42)
        );
        assert!(parse_canonical_generation_name("generation-42.log").is_err());
        assert!(parse_canonical_commit_name("commit-00000000000000000000.marker").is_err());
        assert!(parse_canonical_staging_commit_name("staging-commit-42.marker").is_err());
        assert!(parse_canonical_reservation_name("reserve-42.frontier").is_err());
    }

    #[test]
    fn next_generation_tracks_every_observed_namespace_id() {
        let directory = GenerationNamespace {
            generation_files: BTreeMap::from([(3, PathBuf::from("g3"))]),
            marker_files: BTreeMap::from([(2, PathBuf::from("m2"))]),
            staging_marker_files: BTreeMap::from([(7, PathBuf::from("s7"))]),
            reservation_files: BTreeMap::from([(11, PathBuf::from("r11"))]),
        };
        assert_eq!(directory.highest_observed_generation(), Some(11));
    }
}
