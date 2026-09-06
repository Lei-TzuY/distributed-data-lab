//! A minimal but real persistent engine: a checksummed, append-only KV record log.
//!
//! The engine is intentionally single-process and single-writer. Every successful mutation calls
//! `sync_data` before it changes the in-memory index or returns. Reopen replays complete records and
//! truncates only a structurally valid incomplete final append. Fully present checksum failures,
//! invalid lengths, invalid versions, sequence discontinuities, and unexplained tail bytes fail
//! closed.

use std::collections::BTreeMap;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use crc32fast::Hasher;
use db_core::{
    validate_key, validate_key_value, ByteString, ConcurrencyMode, CrashRecovery, DbError,
    DistributionMode, EngineCapabilities, KvEngine, LogicalModel, Persistence, Result,
    StorageArchitecture, MAX_KEY_BYTES, MAX_VALUE_BYTES,
};
use same_file::Handle;
use serde::Serialize;

const FILE_MAGIC: [u8; 8] = *b"DBLABKV\0";
const FILE_VERSION: u16 = 1;
const FILE_HEADER_LEN: usize = 16;
const FILE_HEADER_LEN_U64: u64 = FILE_HEADER_LEN as u64;

const RECORD_MAGIC: [u8; 4] = *b"KVLG";
const RECORD_VERSION: u8 = 1;
const RECORD_HEADER_LEN: usize = 32;
const RECORD_HEADER_LEN_U64: u64 = RECORD_HEADER_LEN as u64;

const KIND_PUT: u8 = 1;
const KIND_DELETE: u8 = 2;

const GENERATION_PREFIX: &str = "generation-";
const GENERATION_SUFFIX: &str = ".log";
const GENERATION_ID_WIDTH: usize = 20;

/// Returns whether `path` uses the canonical generation-owned append-log filename.
///
/// Canonical generation ids are nonzero `u64` values rendered as exactly 20 decimal digits.
#[must_use]
pub fn is_canonical_generation_path(path: impl AsRef<Path>) -> bool {
    let Some(name) = path.as_ref().file_name().and_then(|name| name.to_str()) else {
        return false;
    };
    let Some(digits) = name
        .strip_prefix(GENERATION_PREFIX)
        .and_then(|name| name.strip_suffix(GENERATION_SUFFIX))
    else {
        return false;
    };
    if digits.len() != GENERATION_ID_WIDTH || !digits.bytes().all(|byte| byte.is_ascii_digit()) {
        return false;
    }
    let Ok(id) = digits.parse::<u64>() else {
        return false;
    };
    id != 0 && format!("{id:020}") == digits
}

/// Description of a final append that is incomplete but safe to discard as a unit.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct TruncatedTail {
    /// Offset at which the incomplete record starts.
    pub record_offset: u64,
    /// Bytes present from `record_offset` through physical EOF.
    pub available_bytes: u64,
    /// Total record bytes required when the complete header declares a trustworthy length.
    pub required_bytes: Option<u64>,
}

/// Read-only validation result for an append-log file.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct VerificationReport {
    /// File format version.
    pub file_format_version: u16,
    /// Physical bytes observed during verification.
    pub file_bytes: u64,
    /// Prefix ending after the last complete valid record.
    pub valid_bytes: u64,
    /// Number of complete mutation records.
    pub record_count: u64,
    /// Number of live keys after replay.
    pub live_keys: usize,
    /// Sequence number required for the next append.
    pub next_sequence: u64,
    /// Incomplete final append, if present. Verification reports but never repairs it.
    pub recoverable_tail: Option<TruncatedTail>,
}

/// One live key shown by read-only inspection.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct InspectionEntry {
    /// Binary key encoded as hexadecimal in JSON.
    pub key: ByteString,
    /// Current value size.
    pub value_bytes: usize,
    /// Current value when value output was explicitly requested.
    pub value: Option<ByteString>,
}

/// Read-only replay and inspection result.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct InspectionReport {
    /// Structural validation metadata.
    pub verification: VerificationReport,
    /// Live entries in bytewise key order.
    pub entries: Vec<InspectionEntry>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PathOwnership {
    Standalone,
    GenerationManaged,
}

/// Checksummed append-only engine backed by one file and an in-memory replay index.
pub struct LogEngine {
    path: PathBuf,
    path_ownership: PathOwnership,
    file: Option<File>,
    values: BTreeMap<Vec<u8>, Vec<u8>>,
    next_sequence: u64,
    record_count: u64,
    acknowledged_valid_bytes: u64,
    acknowledged_prefix_hasher: Hasher,
    recovered_tail: Option<TruncatedTail>,
    poisoned: bool,
}

impl std::fmt::Debug for LogEngine {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LogEngine")
            .field("path", &self.path)
            .field("path_ownership", &self.path_ownership)
            .field("live_keys", &self.values.len())
            .field("next_sequence", &self.next_sequence)
            .field("record_count", &self.record_count)
            .field("recovered_tail", &self.recovered_tail)
            .field("poisoned", &self.poisoned)
            .finish_non_exhaustive()
    }
}

impl LogEngine {
    /// Opens an existing standalone engine or atomically reserves a new standalone file.
    ///
    /// Canonical `generation-{id:020}.log` paths are reserved for the generation ownership layer
    /// and fail closed here. A pre-existing zero-length or partial-header file is rejected. A
    /// structurally valid incomplete final record is truncated back to its record boundary and
    /// synchronized.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        reject_standalone_generation_path(&path)?;
        Self::open_with_ownership(path, PathOwnership::Standalone)
    }

    /// Opens a canonical generation-owned append log after external ownership checks.
    ///
    /// This constructor deliberately bypasses the standalone canonical-name guard. The caller must
    /// already hold the generation writer lease and must have verified that this exact path is the
    /// authoritative generation. Ordinary append-log users must call [`Self::open`] instead.
    pub fn open_managed_generation(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        require_managed_generation_path(&path)?;
        Self::open_with_ownership(path, PathOwnership::GenerationManaged)
    }

    fn open_with_ownership(path: PathBuf, path_ownership: PathOwnership) -> Result<Self> {
        let (file, created) = open_or_create(&path)?;
        Self::from_file(path, path_ownership, file, created)
    }

    /// Creates a new standalone engine and fails atomically if the path already exists.
    ///
    /// Canonical `generation-{id:020}.log` paths are reserved for the generation ownership layer
    /// and fail closed here. This is the required constructor for differential experiments that
    /// must not inherit state from an earlier run.
    pub fn create_new(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        reject_standalone_generation_path(&path)?;
        Self::create_new_with_ownership(path, PathOwnership::Standalone)
    }

    /// Creates a canonical generation-owned append log after external ownership checks.
    ///
    /// The caller is responsible for the generation reservation/lease/publication protocol.
    /// Ordinary append-log users must call [`Self::create_new`] instead.
    pub fn create_new_managed_generation(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        require_managed_generation_path(&path)?;
        Self::create_new_with_ownership(path, PathOwnership::GenerationManaged)
    }

    fn create_new_with_ownership(path: PathBuf, path_ownership: PathOwnership) -> Result<Self> {
        let file = create_new_file(&path)?;
        Self::from_file(path, path_ownership, file, true)
    }

    fn from_file(
        path: PathBuf,
        path_ownership: PathOwnership,
        mut file: File,
        created: bool,
    ) -> Result<Self> {
        if created {
            file.write_all(&encode_file_header())?;
            file.sync_all()?;
        }

        let scan = scan_file(&mut file)?;
        if scan.recoverable_tail.is_some() {
            file.set_len(scan.valid_bytes)?;
            file.sync_all()?;
        }
        let acknowledged_prefix_hasher = hash_file_prefix(&mut file, scan.valid_bytes)?;
        file.seek(SeekFrom::End(0))?;

        Ok(Self {
            path,
            path_ownership,
            file: Some(file),
            values: scan.values,
            next_sequence: scan.next_sequence,
            record_count: scan.record_count,
            acknowledged_valid_bytes: scan.valid_bytes,
            acknowledged_prefix_hasher,
            recovered_tail: scan.recoverable_tail,
            poisoned: false,
        })
    }

    /// Verifies and replays a file without modifying it.
    pub fn verify(path: impl AsRef<Path>) -> Result<VerificationReport> {
        let mut file = File::open(path)?;
        Ok(scan_file(&mut file)?.verification())
    }

    /// Verifies and returns live entries without modifying the file.
    pub fn inspect(path: impl AsRef<Path>, include_values: bool) -> Result<InspectionReport> {
        let mut file = File::open(path)?;
        let scan = scan_file(&mut file)?;
        let verification = scan.verification();
        let entries = scan
            .values
            .into_iter()
            .map(|(key, value)| InspectionEntry {
                key: ByteString::from(key),
                value_bytes: value.len(),
                value: include_values.then(|| ByteString::from(value)),
            })
            .collect();
        Ok(InspectionReport {
            verification,
            entries,
        })
    }

    /// Path from which this engine was opened.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Number of live keys in the replay index.
    #[must_use]
    pub fn len(&self) -> usize {
        self.values.len()
    }

    /// Whether the replay index contains no live keys.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }

    /// Tail repaired during the most recent open or reopen.
    #[must_use]
    pub fn recovered_tail(&self) -> Option<&TruncatedTail> {
        self.recovered_tail.as_ref()
    }

    fn ensure_usable(&self) -> Result<()> {
        if self.poisoned || self.file.is_none() {
            Err(DbError::Poisoned)
        } else {
            Ok(())
        }
    }

    fn append(&mut self, kind: RecordKind, key: &[u8], value: &[u8]) -> Result<()> {
        self.ensure_usable()?;
        let sequence = self.next_sequence;
        let following_sequence = sequence.checked_add(1).ok_or_else(|| DbError::Corruption {
            offset: 0,
            reason: "record sequence number exhausted".to_owned(),
        })?;
        let following_record_count =
            self.record_count
                .checked_add(1)
                .ok_or_else(|| DbError::Corruption {
                    offset: 0,
                    reason: "record counter exhausted".to_owned(),
                })?;
        let record = encode_record(kind, sequence, key, value)?;
        let record_bytes = u64::try_from(record.len()).map_err(|_| {
            DbError::InvalidInput("encoded record length does not fit u64".to_owned())
        })?;
        let following_acknowledged_valid_bytes = self
            .acknowledged_valid_bytes
            .checked_add(record_bytes)
            .ok_or_else(|| corruption(0, "acknowledged prefix length overflowed u64"))?;

        let append_result = (|| -> Result<()> {
            let file = self
                .file
                .as_mut()
                .ok_or_else(|| DbError::Io(io::Error::other("append file is not open")))?;
            let observed_eof = file.seek(SeekFrom::End(0))?;
            if observed_eof != self.acknowledged_valid_bytes {
                return Err(corruption(
                    observed_eof,
                    format!(
                        "append-log physical EOF changed before mutation: observed {observed_eof}, previously acknowledged {}",
                        self.acknowledged_valid_bytes
                    ),
                ));
            }
            let observed_prefix_hasher = hash_file_prefix(file, self.acknowledged_valid_bytes)?;
            if observed_prefix_hasher.finalize()
                != self.acknowledged_prefix_hasher.clone().finalize()
            {
                return Err(corruption(
                    0,
                    "acknowledged record changed before mutation: durable prefix fingerprint mismatch",
                ));
            }
            let observed_eof_after_hash = file.seek(SeekFrom::End(0))?;
            if observed_eof_after_hash != self.acknowledged_valid_bytes {
                return Err(corruption(
                    observed_eof_after_hash,
                    format!(
                        "append-log physical EOF changed while validating mutation: observed {observed_eof_after_hash}, previously acknowledged {}",
                        self.acknowledged_valid_bytes
                    ),
                ));
            }
            file.write_all(&record)?;
            file.sync_data()?;
            Ok(())
        })();

        if let Err(error) = append_result {
            self.poisoned = true;
            return Err(error);
        }

        self.acknowledged_prefix_hasher.update(&record);
        self.acknowledged_valid_bytes = following_acknowledged_valid_bytes;
        self.next_sequence = following_sequence;
        self.record_count = following_record_count;
        Ok(())
    }
}

impl KvEngine for LogEngine {
    fn capabilities(&self) -> EngineCapabilities {
        EngineCapabilities {
            name: "append-log-v1",
            logical_model: LogicalModel::KeyValue,
            storage_architecture: StorageArchitecture::AppendLog,
            concurrency: ConcurrencyMode::CallerSerialized,
            persistence: Persistence::Persistent,
            crash_recovery: CrashRecovery::TruncatedFinalAppend,
            distribution: DistributionMode::Standalone,
            ordered_range_scan: false,
            max_key_bytes: MAX_KEY_BYTES,
            max_value_bytes: MAX_VALUE_BYTES,
        }
    }

    fn put(&mut self, key: &[u8], value: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key_value(key, value)?;
        let previous = self.values.get(key).cloned();
        self.append(RecordKind::Put, key, value)?;
        self.values.insert(key.to_vec(), value.to_vec());
        Ok(previous)
    }

    fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        self.ensure_usable()?;
        Ok(self.values.get(key).cloned())
    }

    fn delete(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        let previous = self.values.get(key).cloned();
        self.append(RecordKind::Delete, key, &[])?;
        self.values.remove(key);
        Ok(previous)
    }

    fn reopen(&mut self) -> Result<()> {
        let ownership_check = match self.path_ownership {
            PathOwnership::Standalone => reject_standalone_generation_path(&self.path),
            PathOwnership::GenerationManaged => require_managed_generation_path(&self.path),
        };
        if let Err(error) = ownership_check {
            self.file.take();
            self.poisoned = true;
            return Err(error);
        }
        if let Err(error) = reject_symbolic_link(&self.path) {
            self.poisoned = true;
            return Err(error);
        }

        let mut candidate = match OpenOptions::new().read(true).write(true).open(&self.path) {
            Ok(file) => file,
            Err(error) => {
                self.poisoned = true;
                return Err(DbError::Io(error));
            }
        };

        if let Some(current) = self.file.as_ref() {
            match same_file_identity(current, &candidate) {
                Ok(true) => {}
                Ok(false) => {
                    self.poisoned = true;
                    return Err(DbError::InvalidInput(format!(
                        "append-log backing file identity changed before reopen: {}",
                        self.path.display()
                    )));
                }
                Err(error) => {
                    self.file.take();
                    self.poisoned = true;
                    return Err(error);
                }
            }
        }

        let observed = match scan_file(&mut candidate) {
            Ok(scan) => scan,
            Err(error) => {
                self.poisoned = true;
                return Err(error);
            }
        };
        if observed.record_count < self.record_count {
            self.poisoned = true;
            return Err(corruption(
                0,
                format!(
                    "append-log record count regressed across reopen: observed {}, previously acknowledged {}",
                    observed.record_count, self.record_count
                ),
            ));
        }
        if observed.valid_bytes < self.acknowledged_valid_bytes {
            self.poisoned = true;
            return Err(corruption(
                observed.valid_bytes,
                "acknowledged record changed before reopen: valid prefix became shorter",
            ));
        }
        let observed_prefix_hasher =
            match hash_file_prefix(&mut candidate, self.acknowledged_valid_bytes) {
                Ok(hasher) => hasher,
                Err(error) => {
                    self.poisoned = true;
                    return Err(error);
                }
            };
        if observed_prefix_hasher.finalize() != self.acknowledged_prefix_hasher.clone().finalize() {
            self.poisoned = true;
            return Err(corruption(
                0,
                "acknowledged record changed before reopen: durable prefix fingerprint mismatch",
            ));
        }

        let reopened = Self::from_file(self.path.clone(), self.path_ownership, candidate, false);
        match reopened {
            Ok(reopened) => {
                *self = reopened;
                Ok(())
            }
            Err(error) => {
                self.poisoned = true;
                Err(error)
            }
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RecordKind {
    Put,
    Delete,
}

impl RecordKind {
    const fn encoded(self) -> u8 {
        match self {
            Self::Put => KIND_PUT,
            Self::Delete => KIND_DELETE,
        }
    }

    fn decode(encoded: u8, offset: u64) -> Result<Self> {
        match encoded {
            KIND_PUT => Ok(Self::Put),
            KIND_DELETE => Ok(Self::Delete),
            _ => Err(corruption(
                offset + 5,
                format!("unknown record kind {encoded}"),
            )),
        }
    }
}

struct ScanResult {
    file_bytes: u64,
    valid_bytes: u64,
    record_count: u64,
    next_sequence: u64,
    recoverable_tail: Option<TruncatedTail>,
    values: BTreeMap<Vec<u8>, Vec<u8>>,
}

impl ScanResult {
    fn verification(&self) -> VerificationReport {
        VerificationReport {
            file_format_version: FILE_VERSION,
            file_bytes: self.file_bytes,
            valid_bytes: self.valid_bytes,
            record_count: self.record_count,
            live_keys: self.values.len(),
            next_sequence: self.next_sequence,
            recoverable_tail: self.recoverable_tail.clone(),
        }
    }
}

struct RecordMetadata {
    kind: RecordKind,
    sequence: u64,
    key_len: usize,
    value_len: usize,
    total_len: u64,
    expected_record_crc: u32,
}

fn reject_standalone_generation_path(path: &Path) -> Result<()> {
    if is_canonical_generation_path(path) {
        return Err(DbError::InvalidInput(format!(
            "standalone append-log constructor refuses canonical generation path {}; generation ownership code must use the managed-generation constructor after acquiring the generation writer lease",
            path.display()
        )));
    }
    Ok(())
}

fn require_managed_generation_path(path: &Path) -> Result<()> {
    if !is_canonical_generation_path(path) {
        return Err(DbError::InvalidInput(format!(
            "managed-generation append-log constructor requires canonical generation-{{id:020}}.log path: {}",
            path.display()
        )));
    }
    Ok(())
}

fn reject_symbolic_link(path: &Path) -> Result<()> {
    match std::fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() => Err(DbError::InvalidInput(format!(
            "append-log ownership path must not be a symbolic link: {}",
            path.display()
        ))),
        Ok(_) => Ok(()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(DbError::Io(error)),
    }
}

fn same_file_identity(left: &File, right: &File) -> Result<bool> {
    let left = Handle::from_file(left.try_clone()?)?;
    let right = Handle::from_file(right.try_clone()?)?;
    Ok(left == right)
}

fn hash_file_prefix(file: &mut File, prefix_bytes: u64) -> Result<Hasher> {
    file.seek(SeekFrom::Start(0))?;
    let mut remaining = prefix_bytes;
    let mut buffer = [0_u8; 8192];
    let mut hasher = Hasher::new();

    while remaining != 0 {
        let chunk_bytes = remaining.min(buffer.len() as u64);
        let chunk_len = usize::try_from(chunk_bytes)
            .map_err(|_| corruption(0, "prefix hash chunk length does not fit usize"))?;
        file.read_exact(&mut buffer[..chunk_len])?;
        hasher.update(&buffer[..chunk_len]);
        remaining -= chunk_bytes;
    }

    Ok(hasher)
}

fn open_or_create(path: &Path) -> Result<(File, bool)> {
    match create_new_file(path) {
        Ok(file) => Ok((file, true)),
        Err(DbError::Io(error)) if error.kind() == io::ErrorKind::AlreadyExists => {
            reject_symbolic_link(path)?;
            let file = OpenOptions::new().read(true).write(true).open(path)?;
            Ok((file, false))
        }
        Err(error) => Err(error),
    }
}

fn create_new_file(path: &Path) -> Result<File> {
    Ok(OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .open(path)?)
}

fn encode_file_header() -> [u8; FILE_HEADER_LEN] {
    let mut header = [0_u8; FILE_HEADER_LEN];
    header[..8].copy_from_slice(&FILE_MAGIC);
    header[8..10].copy_from_slice(&FILE_VERSION.to_le_bytes());
    header[10..12].copy_from_slice(&(FILE_HEADER_LEN as u16).to_le_bytes());
    let checksum = crc32fast::hash(&header[..12]);
    header[12..16].copy_from_slice(&checksum.to_le_bytes());
    header
}

fn encode_record(kind: RecordKind, sequence: u64, key: &[u8], value: &[u8]) -> Result<Vec<u8>> {
    validate_key_value(key, value)?;
    if kind == RecordKind::Delete && !value.is_empty() {
        return Err(DbError::InvalidInput(
            "delete record cannot contain a value".to_owned(),
        ));
    }

    let key_len = u32::try_from(key.len())
        .map_err(|_| DbError::InvalidInput("key length does not fit u32".to_owned()))?;
    let value_len = u32::try_from(value.len())
        .map_err(|_| DbError::InvalidInput("value length does not fit u32".to_owned()))?;
    let payload_len = key.len().checked_add(value.len()).ok_or_else(|| {
        DbError::InvalidInput("record payload length overflowed usize".to_owned())
    })?;
    let capacity = RECORD_HEADER_LEN.checked_add(payload_len).ok_or_else(|| {
        DbError::InvalidInput("encoded record length overflowed usize".to_owned())
    })?;

    let mut header = [0_u8; RECORD_HEADER_LEN];
    header[..4].copy_from_slice(&RECORD_MAGIC);
    header[4] = RECORD_VERSION;
    header[5] = kind.encoded();
    header[6..8].copy_from_slice(&0_u16.to_le_bytes());
    header[8..16].copy_from_slice(&sequence.to_le_bytes());
    header[16..20].copy_from_slice(&key_len.to_le_bytes());
    header[20..24].copy_from_slice(&value_len.to_le_bytes());
    let header_checksum = crc32fast::hash(&header[..24]);
    header[24..28].copy_from_slice(&header_checksum.to_le_bytes());

    let mut record_hasher = Hasher::new();
    record_hasher.update(&header[..28]);
    record_hasher.update(key);
    record_hasher.update(value);
    header[28..32].copy_from_slice(&record_hasher.finalize().to_le_bytes());

    let mut record = Vec::with_capacity(capacity);
    record.extend_from_slice(&header);
    record.extend_from_slice(key);
    record.extend_from_slice(value);
    Ok(record)
}

fn scan_file(file: &mut File) -> Result<ScanResult> {
    let file_bytes = file.metadata()?.len();
    if file_bytes < FILE_HEADER_LEN_U64 {
        return Err(corruption(
            0,
            format!("truncated file header: found {file_bytes} bytes, need {FILE_HEADER_LEN}"),
        ));
    }

    file.seek(SeekFrom::Start(0))?;
    let mut file_header = [0_u8; FILE_HEADER_LEN];
    file.read_exact(&mut file_header)?;
    validate_file_header(&file_header)?;

    let mut offset = FILE_HEADER_LEN_U64;
    let mut expected_sequence = 1_u64;
    let mut record_count = 0_u64;
    let mut values = BTreeMap::new();

    while offset < file_bytes {
        let remaining = file_bytes
            .checked_sub(offset)
            .ok_or_else(|| corruption(offset, "file offset exceeded file size"))?;
        if remaining < RECORD_HEADER_LEN_U64 {
            let available = usize::try_from(remaining)
                .map_err(|_| corruption(offset, "tail length does not fit usize"))?;
            let mut partial = [0_u8; RECORD_HEADER_LEN];
            file.seek(SeekFrom::Start(offset))?;
            file.read_exact(&mut partial[..available])?;
            validate_partial_header(&partial[..available], offset, expected_sequence)?;
            return Ok(ScanResult {
                file_bytes,
                valid_bytes: offset,
                record_count,
                next_sequence: expected_sequence,
                recoverable_tail: Some(TruncatedTail {
                    record_offset: offset,
                    available_bytes: remaining,
                    required_bytes: None,
                }),
                values,
            });
        }

        let mut header = [0_u8; RECORD_HEADER_LEN];
        file.seek(SeekFrom::Start(offset))?;
        file.read_exact(&mut header)?;
        let metadata = parse_record_header(&header, offset, expected_sequence)?;
        let record_end = checked_record_end(offset, metadata.total_len)?;
        if record_end > file_bytes {
            return Ok(ScanResult {
                file_bytes,
                valid_bytes: offset,
                record_count,
                next_sequence: expected_sequence,
                recoverable_tail: Some(TruncatedTail {
                    record_offset: offset,
                    available_bytes: remaining,
                    required_bytes: Some(metadata.total_len),
                }),
                values,
            });
        }

        let payload_len = metadata
            .key_len
            .checked_add(metadata.value_len)
            .ok_or_else(|| corruption(offset, "record payload length overflowed usize"))?;
        let mut payload = vec![0_u8; payload_len];
        file.read_exact(&mut payload)?;

        let mut record_hasher = Hasher::new();
        record_hasher.update(&header[..28]);
        record_hasher.update(&payload);
        let actual_record_crc = record_hasher.finalize();
        if actual_record_crc != metadata.expected_record_crc {
            return Err(corruption(
                offset,
                format!(
                    "record checksum mismatch: expected {:08x}, computed {:08x}",
                    metadata.expected_record_crc, actual_record_crc
                ),
            ));
        }

        let key = payload[..metadata.key_len].to_vec();
        match metadata.kind {
            RecordKind::Put => {
                values.insert(key, payload[metadata.key_len..].to_vec());
            }
            RecordKind::Delete => {
                values.remove(key.as_slice());
            }
        }

        offset = record_end;
        expected_sequence = metadata
            .sequence
            .checked_add(1)
            .ok_or_else(|| corruption(offset, "record sequence number exhausted"))?;
        record_count = record_count
            .checked_add(1)
            .ok_or_else(|| corruption(offset, "record counter exhausted"))?;
    }

    Ok(ScanResult {
        file_bytes,
        valid_bytes: offset,
        record_count,
        next_sequence: expected_sequence,
        recoverable_tail: None,
        values,
    })
}

fn validate_file_header(header: &[u8; FILE_HEADER_LEN]) -> Result<()> {
    if header[..8] != FILE_MAGIC {
        return Err(corruption(0, "file magic mismatch"));
    }
    let expected_crc = read_u32(&header[12..16]);
    let actual_crc = crc32fast::hash(&header[..12]);
    if expected_crc != actual_crc {
        return Err(corruption(
            12,
            format!(
                "file header checksum mismatch: expected {expected_crc:08x}, computed {actual_crc:08x}"
            ),
        ));
    }
    let version = read_u16(&header[8..10]);
    if version != FILE_VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "append-log file",
            found: u64::from(version),
            supported: u64::from(FILE_VERSION),
        });
    }
    let header_len = read_u16(&header[10..12]);
    if usize::from(header_len) != FILE_HEADER_LEN {
        return Err(corruption(
            10,
            format!("invalid file header length {header_len}"),
        ));
    }
    Ok(())
}

fn parse_record_header(
    header: &[u8; RECORD_HEADER_LEN],
    offset: u64,
    expected_sequence: u64,
) -> Result<RecordMetadata> {
    if header[..4] != RECORD_MAGIC {
        return Err(corruption(offset, "record magic mismatch"));
    }
    let expected_header_crc = read_u32(&header[24..28]);
    let actual_header_crc = crc32fast::hash(&header[..24]);
    if expected_header_crc != actual_header_crc {
        return Err(corruption(
            offset + 24,
            format!(
                "record header checksum mismatch: expected {expected_header_crc:08x}, computed {actual_header_crc:08x}"
            ),
        ));
    }
    if header[4] != RECORD_VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "append-log record",
            found: u64::from(header[4]),
            supported: u64::from(RECORD_VERSION),
        });
    }
    let kind = RecordKind::decode(header[5], offset)?;
    let flags = read_u16(&header[6..8]);
    if flags != 0 {
        return Err(corruption(
            offset + 6,
            format!("unsupported record flags {flags:#06x}"),
        ));
    }
    let sequence = read_u64(&header[8..16]);
    if sequence != expected_sequence {
        return Err(corruption(
            offset + 8,
            format!("record sequence {sequence}, expected {expected_sequence}"),
        ));
    }

    let key_len_u32 = read_u32(&header[16..20]);
    let value_len_u32 = read_u32(&header[20..24]);
    let key_len = usize::try_from(key_len_u32)
        .map_err(|_| corruption(offset + 16, "key length does not fit usize"))?;
    let value_len = usize::try_from(value_len_u32)
        .map_err(|_| corruption(offset + 20, "value length does not fit usize"))?;
    validate_record_lengths(kind, key_len, value_len, offset)?;

    let payload_len_u64 = u64::from(key_len_u32)
        .checked_add(u64::from(value_len_u32))
        .ok_or_else(|| corruption(offset, "record payload length overflowed u64"))?;
    let total_len = RECORD_HEADER_LEN_U64
        .checked_add(payload_len_u64)
        .ok_or_else(|| corruption(offset, "record total length overflowed u64"))?;

    Ok(RecordMetadata {
        kind,
        sequence,
        key_len,
        value_len,
        total_len,
        expected_record_crc: read_u32(&header[28..32]),
    })
}

fn validate_partial_header(bytes: &[u8], offset: u64, expected_sequence: u64) -> Result<()> {
    let magic_bytes = bytes.len().min(RECORD_MAGIC.len());
    if bytes[..magic_bytes] != RECORD_MAGIC[..magic_bytes] {
        return Err(corruption(
            offset,
            "unrecognized bytes after last valid record",
        ));
    }
    if bytes.len() >= 5 && bytes[4] != RECORD_VERSION {
        return Err(corruption(
            offset + 4,
            "invalid record version in partial tail",
        ));
    }
    if bytes.len() >= 6 {
        RecordKind::decode(bytes[5], offset)?;
    }
    validate_available_prefix(bytes, 6, &0_u16.to_le_bytes(), offset, "record flags")?;
    validate_available_prefix(
        bytes,
        8,
        &expected_sequence.to_le_bytes(),
        offset,
        "record sequence",
    )?;

    if bytes.len() >= 20 {
        let key_len = usize::try_from(read_u32(&bytes[16..20]))
            .map_err(|_| corruption(offset + 16, "key length does not fit usize"))?;
        if key_len > MAX_KEY_BYTES {
            return Err(corruption(
                offset + 16,
                format!("declared key length {key_len} exceeds maximum {MAX_KEY_BYTES}"),
            ));
        }
    }
    if bytes.len() >= 24 {
        let kind = RecordKind::decode(bytes[5], offset)?;
        let key_len = usize::try_from(read_u32(&bytes[16..20]))
            .map_err(|_| corruption(offset + 16, "key length does not fit usize"))?;
        let value_len = usize::try_from(read_u32(&bytes[20..24]))
            .map_err(|_| corruption(offset + 20, "value length does not fit usize"))?;
        validate_record_lengths(kind, key_len, value_len, offset)?;
    }
    if bytes.len() >= 28 {
        let expected_crc = read_u32(&bytes[24..28]);
        let actual_crc = crc32fast::hash(&bytes[..24]);
        if expected_crc != actual_crc {
            return Err(corruption(
                offset + 24,
                "partial record header checksum mismatch",
            ));
        }
    }
    Ok(())
}

fn validate_available_prefix(
    bytes: &[u8],
    field_offset: usize,
    expected: &[u8],
    record_offset: u64,
    field_name: &str,
) -> Result<()> {
    if bytes.len() <= field_offset {
        return Ok(());
    }
    let available = (bytes.len() - field_offset).min(expected.len());
    if bytes[field_offset..field_offset + available] != expected[..available] {
        let offset_delta = u64::try_from(field_offset)
            .map_err(|_| corruption(record_offset, "field offset does not fit u64"))?;
        return Err(corruption(
            record_offset + offset_delta,
            format!("invalid {field_name} prefix in partial tail"),
        ));
    }
    Ok(())
}

fn validate_record_lengths(
    kind: RecordKind,
    key_len: usize,
    value_len: usize,
    offset: u64,
) -> Result<()> {
    if key_len > MAX_KEY_BYTES {
        return Err(corruption(
            offset + 16,
            format!("declared key length {key_len} exceeds maximum {MAX_KEY_BYTES}"),
        ));
    }
    if value_len > MAX_VALUE_BYTES {
        return Err(corruption(
            offset + 20,
            format!("declared value length {value_len} exceeds maximum {MAX_VALUE_BYTES}"),
        ));
    }
    if kind == RecordKind::Delete && value_len != 0 {
        return Err(corruption(
            offset + 20,
            format!("delete record declares nonzero value length {value_len}"),
        ));
    }
    key_len
        .checked_add(value_len)
        .ok_or_else(|| corruption(offset, "record payload length overflowed usize"))?;
    Ok(())
}

fn checked_record_end(offset: u64, total_len: u64) -> Result<u64> {
    offset
        .checked_add(total_len)
        .ok_or_else(|| corruption(offset, "record end offset overflowed u64"))
}

fn read_u16(bytes: &[u8]) -> u16 {
    u16::from_le_bytes([bytes[0], bytes[1]])
}

fn read_u32(bytes: &[u8]) -> u32 {
    u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

fn read_u64(bytes: &[u8]) -> u64 {
    u64::from_le_bytes([
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
    ])
}

fn corruption(offset: u64, reason: impl Into<String>) -> DbError {
    DbError::Corruption {
        offset,
        reason: reason.into(),
    }
}

#[cfg(test)]
mod tests;
