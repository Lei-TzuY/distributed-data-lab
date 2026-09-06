use std::ffi::OsString;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::Serialize;
use thiserror::Error;

use crate::generation_directory::{canonical_real_directory, GenerationDirectoryError};

pub const GENERATION_WRITER_LOCK_PROTOCOL: &str = "append_log_generation_writer_lock_v1";
pub const GENERATION_WRITER_LOCK_INSPECTION_PROTOCOL: &str =
    "append_log_generation_writer_lock_inspection_v1";
pub const GENERATION_WRITER_LOCK_CLEAR_PROTOCOL: &str =
    "append_log_generation_writer_lock_clear_v1";
pub const MAX_GENERATION_WRITER_LOCK_BYTES: usize = 4096;

static NEXT_ACQUISITION_COUNTER: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Error)]
pub enum GenerationWriterLockError {
    #[error(transparent)]
    Directory(#[from] GenerationDirectoryError),
    #[error("invalid generation writer lock request: {0}")]
    Invalid(String),
    #[error("generation writer lock is already held or stale: {path}")]
    Busy { path: PathBuf },
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct GenerationWriterLockInspection {
    pub protocol: &'static str,
    pub lock_path: String,
    pub present: bool,
    pub record_bytes: usize,
    pub record_hex: Option<String>,
    pub recorded_lock_protocol: Option<String>,
    pub recorded_pid: Option<u32>,
    pub acquisition_id: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct GenerationWriterLockClearSummary {
    pub protocol: &'static str,
    pub lock_path: String,
    pub removed_record_bytes: usize,
    pub removed_record_hex: String,
}

/// Cooperative cross-process writer exclusion for one generation directory.
///
/// The lock is a create-new sibling of the canonical generation directory, so the retained
/// generation namespace stays closed and evidence-only. A crashed process may leave the lock file
/// behind; that stale lock intentionally fails closed until an operator removes it after
/// independently confirming that no writer is alive. No PID-based or age-based lock stealing is
/// performed by this protocol.
pub struct GenerationWriterLease {
    directory: PathBuf,
    lock_path: PathBuf,
    owner_record: Vec<u8>,
    file: Option<File>,
}

impl std::fmt::Debug for GenerationWriterLease {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("GenerationWriterLease")
            .field("directory", &self.directory)
            .field("lock_path", &self.lock_path)
            .finish_non_exhaustive()
    }
}

impl GenerationWriterLease {
    #[must_use]
    pub fn directory(&self) -> &Path {
        &self.directory
    }

    #[must_use]
    pub fn lock_path(&self) -> &Path {
        &self.lock_path
    }

    #[must_use]
    pub fn owner_record(&self) -> &[u8] {
        &self.owner_record
    }
}

impl Drop for GenerationWriterLease {
    fn drop(&mut self) {
        let _ = self.file.take();
        if matches!(
            read_lock_record_if_present(&self.lock_path),
            Ok(Some(ref current)) if current == &self.owner_record
        ) {
            let _ = fs::remove_file(&self.lock_path);
        }
    }
}

pub fn acquire_generation_writer_lease(
    directory: &Path,
) -> Result<GenerationWriterLease, GenerationWriterLockError> {
    let directory = canonical_real_directory(directory)?;
    let lock_path = writer_lock_path(&directory)?;
    let mut file = match OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&lock_path)
    {
        Ok(file) => file,
        Err(source) if source.kind() == io::ErrorKind::AlreadyExists => {
            return Err(GenerationWriterLockError::Busy { path: lock_path });
        }
        Err(source) => {
            return Err(GenerationWriterLockError::Io {
                path: lock_path,
                source,
            });
        }
    };

    let owner_record = owner_record();
    if let Err(source) = file.write_all(&owner_record) {
        drop(file);
        let _ = fs::remove_file(&lock_path);
        return Err(GenerationWriterLockError::Io {
            path: lock_path,
            source,
        });
    }

    Ok(GenerationWriterLease {
        directory,
        lock_path,
        owner_record,
        file: Some(file),
    })
}

pub fn generation_writer_lock_path(directory: &Path) -> Result<PathBuf, GenerationWriterLockError> {
    let directory = canonical_real_directory(directory)?;
    writer_lock_path(&directory)
}

pub fn inspect_generation_writer_lock(
    directory: &Path,
) -> Result<GenerationWriterLockInspection, GenerationWriterLockError> {
    let directory = canonical_real_directory(directory)?;
    let lock_path = writer_lock_path(&directory)?;
    let record = read_lock_record_if_present(&lock_path)?;
    let (recorded_lock_protocol, recorded_pid, acquisition_id) = record
        .as_deref()
        .map(parse_owner_record)
        .unwrap_or((None, None, None));

    Ok(GenerationWriterLockInspection {
        protocol: GENERATION_WRITER_LOCK_INSPECTION_PROTOCOL,
        lock_path: lock_path.display().to_string(),
        present: record.is_some(),
        record_bytes: record.as_ref().map_or(0, Vec::len),
        record_hex: record.as_deref().map(encode_hex),
        recorded_lock_protocol,
        recorded_pid,
        acquisition_id,
    })
}

/// Explicitly removes a stale cooperative writer lock after external liveness confirmation.
///
/// `confirmed_no_live_writer` is intentionally load-bearing: this module never infers liveness from
/// PID, age, or lock contents. `expected_record_hex` must match the exact current lock bytes so an
/// operator cannot accidentally clear different lock evidence from what they inspected.
pub fn clear_stale_generation_writer_lock(
    directory: &Path,
    expected_record_hex: &str,
    confirmed_no_live_writer: bool,
) -> Result<GenerationWriterLockClearSummary, GenerationWriterLockError> {
    if !confirmed_no_live_writer {
        return invalid(
            "clearing a stale writer lock requires explicit confirmation that no coordinated writer is alive",
        );
    }
    let expected = decode_hex(expected_record_hex)?;
    if expected.len() > MAX_GENERATION_WRITER_LOCK_BYTES {
        return invalid(format!(
            "expected lock record exceeds {MAX_GENERATION_WRITER_LOCK_BYTES} bytes"
        ));
    }

    let directory = canonical_real_directory(directory)?;
    let lock_path = writer_lock_path(&directory)?;
    let current = read_lock_record_if_present(&lock_path)?.ok_or_else(|| {
        GenerationWriterLockError::Invalid(format!(
            "no generation writer lock exists at {}",
            lock_path.display()
        ))
    })?;
    if current != expected {
        return invalid(
            "generation writer lock bytes changed since inspection; inspect again before clearing",
        );
    }

    // Re-read immediately before removal. This does not claim adversarial atomic compare-and-delete;
    // the protocol is cooperative and the operator confirmation remains the liveness authority.
    let current_again = read_lock_record_if_present(&lock_path)?.ok_or_else(|| {
        GenerationWriterLockError::Invalid(format!(
            "generation writer lock disappeared before stale-clear at {}",
            lock_path.display()
        ))
    })?;
    if current_again != expected {
        return invalid(
            "generation writer lock bytes changed during stale-clear; inspect again before retrying",
        );
    }

    fs::remove_file(&lock_path).map_err(|source| io_error(&lock_path, source))?;
    match fs::symlink_metadata(&lock_path) {
        Err(source) if source.kind() == io::ErrorKind::NotFound => {}
        Ok(_) => return invalid("generation writer lock still exists after stale-clear removal"),
        Err(source) => return Err(io_error(&lock_path, source)),
    }

    Ok(GenerationWriterLockClearSummary {
        protocol: GENERATION_WRITER_LOCK_CLEAR_PROTOCOL,
        lock_path: lock_path.display().to_string(),
        removed_record_bytes: expected.len(),
        removed_record_hex: encode_hex(&expected),
    })
}

fn owner_record() -> Vec<u8> {
    let pid = std::process::id();
    let counter = NEXT_ACQUISITION_COUNTER.fetch_add(1, Ordering::Relaxed);
    let unix_nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| duration.as_nanos());
    format!(
        "protocol={GENERATION_WRITER_LOCK_PROTOCOL}\npid={pid}\nacquisition={pid}-{unix_nanos:x}-{counter:016x}\n"
    )
    .into_bytes()
}

fn parse_owner_record(record: &[u8]) -> (Option<String>, Option<u32>, Option<String>) {
    let Ok(text) = std::str::from_utf8(record) else {
        return (None, None, None);
    };
    let mut protocol = None;
    let mut pid = None;
    let mut acquisition = None;
    for line in text.lines() {
        if let Some(value) = line.strip_prefix("protocol=") {
            protocol = Some(value.to_owned());
        } else if let Some(value) = line.strip_prefix("pid=") {
            pid = value.parse().ok();
        } else if let Some(value) = line.strip_prefix("acquisition=") {
            acquisition = Some(value.to_owned());
        }
    }
    (protocol, pid, acquisition)
}

fn read_lock_record_if_present(path: &Path) -> Result<Option<Vec<u8>>, GenerationWriterLockError> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(source) if source.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(source) => return Err(io_error(path, source)),
    };
    if !metadata.file_type().is_file() {
        return invalid(format!(
            "generation writer lock must be a real regular file rather than a symlink or non-file: {}",
            path.display()
        ));
    }
    if metadata.len() > MAX_GENERATION_WRITER_LOCK_BYTES as u64 {
        return invalid(format!(
            "generation writer lock exceeds {MAX_GENERATION_WRITER_LOCK_BYTES} bytes: {}",
            path.display()
        ));
    }
    let bytes = fs::read(path).map_err(|source| io_error(path, source))?;
    if bytes.len() > MAX_GENERATION_WRITER_LOCK_BYTES {
        return invalid(format!(
            "generation writer lock grew beyond {MAX_GENERATION_WRITER_LOCK_BYTES} bytes while reading: {}",
            path.display()
        ));
    }
    Ok(Some(bytes))
}

fn writer_lock_path(directory: &Path) -> Result<PathBuf, GenerationWriterLockError> {
    let name = directory.file_name().ok_or_else(|| {
        GenerationWriterLockError::Invalid(format!(
            "generation directory has no lockable final component: {}",
            directory.display()
        ))
    })?;
    let mut lock_name = OsString::from(".");
    lock_name.push(name);
    lock_name.push(".append-log-writer.lock");
    Ok(directory.with_file_name(lock_name))
}

fn encode_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        encoded.push(char::from(HEX[usize::from(byte >> 4)]));
        encoded.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    encoded
}

fn decode_hex(encoded: &str) -> Result<Vec<u8>, GenerationWriterLockError> {
    if encoded.len() % 2 != 0 {
        return invalid("expected lock record hex must have an even number of characters");
    }
    if encoded.len() / 2 > MAX_GENERATION_WRITER_LOCK_BYTES {
        return invalid(format!(
            "expected lock record exceeds {MAX_GENERATION_WRITER_LOCK_BYTES} bytes"
        ));
    }
    let mut bytes = Vec::with_capacity(encoded.len() / 2);
    for pair in encoded.as_bytes().chunks_exact(2) {
        let high = hex_nibble(pair[0])?;
        let low = hex_nibble(pair[1])?;
        bytes.push((high << 4) | low);
    }
    Ok(bytes)
}

fn hex_nibble(byte: u8) -> Result<u8, GenerationWriterLockError> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        b'A'..=b'F' => Ok(byte - b'A' + 10),
        _ => invalid("expected lock record contains a non-hex character"),
    }
}

fn io_error(path: &Path, source: io::Error) -> GenerationWriterLockError {
    GenerationWriterLockError::Io {
        path: path.to_path_buf(),
        source,
    }
}

fn invalid<T>(message: impl Into<String>) -> Result<T, GenerationWriterLockError> {
    Err(GenerationWriterLockError::Invalid(message.into()))
}

#[cfg(test)]
mod tests {
    use std::fs;

    use tempfile::tempdir;

    use super::*;
    use crate::generation_directory::scan_generation_namespace;

    #[test]
    fn lease_is_exclusive_and_released_on_drop() {
        let root = tempdir().expect("temporary root");
        let directory = root.path().join("generations");
        fs::create_dir(&directory).expect("create generation directory");

        let first = acquire_generation_writer_lease(&directory).expect("acquire first lease");
        assert!(first.lock_path().is_file());
        let first_record = first.owner_record().to_vec();
        assert!(matches!(
            acquire_generation_writer_lease(&directory),
            Err(GenerationWriterLockError::Busy { .. })
        ));
        let lock_path = first.lock_path().to_path_buf();
        drop(first);
        assert!(!lock_path.exists());

        let second = acquire_generation_writer_lease(&directory).expect("acquire after release");
        assert_ne!(second.owner_record(), first_record.as_slice());
        drop(second);
    }

    #[test]
    fn lease_does_not_pollute_generation_namespace() {
        let root = tempdir().expect("temporary root");
        let directory = root.path().join("generations");
        fs::create_dir(&directory).expect("create generation directory");

        let lease = acquire_generation_writer_lease(&directory).expect("acquire lease");
        let namespace =
            scan_generation_namespace(lease.directory()).expect("scan retained namespace");
        assert!(namespace.generation_files.is_empty());
        assert!(namespace.marker_files.is_empty());
        assert!(namespace.staging_marker_files.is_empty());
        drop(lease);
    }

    #[test]
    fn stale_lock_fails_closed_without_lock_stealing() {
        let root = tempdir().expect("temporary root");
        let directory = root.path().join("generations");
        fs::create_dir(&directory).expect("create generation directory");
        let lock_path = generation_writer_lock_path(&directory).expect("derive lock path");
        fs::write(&lock_path, b"stale").expect("write stale lock");

        assert!(matches!(
            acquire_generation_writer_lease(&directory),
            Err(GenerationWriterLockError::Busy { .. })
        ));
        assert_eq!(
            fs::read(&lock_path).expect("read stale lock"),
            b"stale",
            "acquisition must not rewrite or steal stale lock evidence"
        );
    }

    #[test]
    fn stale_clear_requires_confirmation_and_exact_observed_bytes() {
        let root = tempdir().expect("temporary root");
        let directory = root.path().join("generations");
        fs::create_dir(&directory).expect("create generation directory");
        let lock_path = generation_writer_lock_path(&directory).expect("derive lock path");
        fs::write(&lock_path, b"stale-evidence").expect("write stale lock");

        let inspected = inspect_generation_writer_lock(&directory).expect("inspect stale lock");
        let expected = inspected.record_hex.expect("record hex");
        assert!(clear_stale_generation_writer_lock(&directory, &expected, false).is_err());
        assert!(lock_path.exists());
        assert!(clear_stale_generation_writer_lock(&directory, "00", true).is_err());
        assert!(lock_path.exists());

        let cleared = clear_stale_generation_writer_lock(&directory, &expected, true)
            .expect("clear matching stale lock");
        assert_eq!(cleared.removed_record_bytes, b"stale-evidence".len());
        assert!(!lock_path.exists());
        let absent = inspect_generation_writer_lock(&directory).expect("inspect absent lock");
        assert!(!absent.present);
    }

    #[cfg(unix)]
    #[test]
    fn lease_drop_does_not_remove_replaced_lock_evidence() {
        let root = tempdir().expect("temporary root");
        let directory = root.path().join("generations");
        fs::create_dir(&directory).expect("create generation directory");
        let lease = acquire_generation_writer_lease(&directory).expect("acquire lease");
        let lock_path = lease.lock_path().to_path_buf();
        fs::remove_file(&lock_path).expect("unlink live lock path for replacement test");
        fs::write(&lock_path, b"replacement-lock-evidence").expect("write replacement lock");
        drop(lease);
        assert_eq!(
            fs::read(&lock_path).expect("replacement lock survives old drop"),
            b"replacement-lock-evidence"
        );
    }
}
