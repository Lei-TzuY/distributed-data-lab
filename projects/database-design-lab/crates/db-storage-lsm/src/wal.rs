use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use crc32fast::Hasher;
use db_core::{validate_key_value, DbError, Result, MAX_KEY_BYTES, MAX_VALUE_BYTES};
use same_file::Handle;
use serde::Serialize;

pub(super) const INITIAL_WAL_ID: u64 = 1;
pub(super) const INITIAL_FIRST_SEQUENCE: u64 = 1;
pub(super) const WAL_HEADER_LEN: usize = 40;
const WAL_HEADER_LEN_U64: u64 = WAL_HEADER_LEN as u64;
const WAL_MAGIC: [u8; 8] = *b"DBLSMWAL";
const WAL_FORMAT_VERSION: u16 = 1;

pub(super) const RECORD_HEADER_LEN: usize = 32;
const RECORD_HEADER_LEN_U64: u64 = RECORD_HEADER_LEN as u64;
const RECORD_MAGIC: [u8; 4] = *b"LSMR";
const RECORD_VERSION: u8 = 1;
const KIND_PUT: u8 = 1;
const KIND_DELETE: u8 = 2;

pub(super) fn file_name(wal_id: u64) -> String {
    format!("wal-{wal_id:016}.log")
}

/// Description of an incomplete final WAL record that open can discard safely.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct RecoveredWalTail {
    /// Byte offset at which the incomplete record begins.
    pub record_offset: u64,
    /// Bytes physically present from the record boundary through EOF.
    pub available_bytes: u64,
    /// Complete encoded record size when a validated full header declares it.
    pub required_bytes: Option<u64>,
}

/// Read-only structural summary of the authoritative write-ahead log segment.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct WalVerification {
    /// WAL format version.
    pub format_version: u16,
    /// Canonical WAL segment id encoded in both the file name and header.
    pub wal_id: u64,
    /// Sequence required for the first mutation in this segment.
    pub first_sequence: u64,
    /// Physical bytes observed.
    pub file_bytes: u64,
    /// Prefix ending after the last complete valid record.
    pub valid_bytes: u64,
    /// Number of complete mutation records in this segment.
    pub record_count: u64,
    /// Sequence required for the next mutation.
    pub next_sequence: u64,
    /// Structurally valid incomplete final record, if present.
    pub recoverable_tail: Option<RecoveredWalTail>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum MutationKind {
    Put,
    Delete,
}

impl MutationKind {
    const fn encoded(self) -> u8 {
        match self {
            Self::Put => KIND_PUT,
            Self::Delete => KIND_DELETE,
        }
    }

    fn decode(encoded: u8, record_offset: u64) -> Result<Self> {
        match encoded {
            KIND_PUT => Ok(Self::Put),
            KIND_DELETE => Ok(Self::Delete),
            _ => Err(corruption(
                record_offset + 5,
                format!("unknown WAL mutation kind {encoded}"),
            )),
        }
    }
}

pub(super) struct Mutation {
    pub(super) sequence: u64,
    pub(super) key: Vec<u8>,
    pub(super) value: Option<Vec<u8>>,
}

pub(super) struct Wal {
    path: PathBuf,
    file: File,
    wal_id: u64,
    first_sequence: u64,
    next_sequence: u64,
    record_count: u64,
    acknowledged_valid_bytes: u64,
    acknowledged_prefix_hasher: Hasher,
    open_examined_bytes: u64,
    recovered_tail: Option<RecoveredWalTail>,
}

impl std::fmt::Debug for Wal {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Wal")
            .field("wal_id", &self.wal_id)
            .field("first_sequence", &self.first_sequence)
            .field("next_sequence", &self.next_sequence)
            .field("record_count", &self.record_count)
            .field("recovered_tail", &self.recovered_tail)
            .finish_non_exhaustive()
    }
}

impl Wal {
    pub(super) fn create_new(path: &Path, wal_id: u64, first_sequence: u64) -> Result<Self> {
        if wal_id == 0 || first_sequence == 0 {
            return Err(corruption(
                0,
                "WAL id and first sequence must both be nonzero",
            ));
        }
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .open(path)?;
        file.write_all(&encode_wal_header(wal_id, first_sequence))?;
        file.sync_all()?;
        let acknowledged_prefix_hasher = hash_file_prefix(&mut file, WAL_HEADER_LEN_U64)?;
        Ok(Self {
            path: path.to_path_buf(),
            file,
            wal_id,
            first_sequence,
            next_sequence: first_sequence,
            record_count: 0,
            acknowledged_valid_bytes: WAL_HEADER_LEN_U64,
            acknowledged_prefix_hasher,
            open_examined_bytes: WAL_HEADER_LEN_U64,
            recovered_tail: None,
        })
    }

    pub(super) fn open(
        path: &Path,
        expected_wal_id: u64,
        expected_first_sequence: u64,
        apply: impl FnMut(Mutation) -> Result<()>,
    ) -> Result<Self> {
        let mut file = OpenOptions::new().read(true).write(true).open(path)?;
        let scan = scan_file(&mut file, expected_wal_id, expected_first_sequence, apply)?;
        if scan.recoverable_tail.is_some() {
            file.set_len(scan.valid_bytes)?;
            file.sync_all()?;
        }
        let acknowledged_prefix_hasher = hash_file_prefix(&mut file, scan.valid_bytes)?;
        file.seek(SeekFrom::End(0))?;
        Ok(Self {
            path: path.to_path_buf(),
            file,
            wal_id: expected_wal_id,
            first_sequence: expected_first_sequence,
            next_sequence: scan.next_sequence,
            record_count: scan.record_count,
            acknowledged_valid_bytes: scan.valid_bytes,
            acknowledged_prefix_hasher,
            open_examined_bytes: scan.file_bytes,
            recovered_tail: scan.recoverable_tail,
        })
    }

    pub(super) fn verify(
        path: &Path,
        expected_wal_id: u64,
        expected_first_sequence: u64,
        apply: impl FnMut(Mutation) -> Result<()>,
    ) -> Result<WalVerification> {
        let mut file = File::open(path)?;
        Ok(
            scan_file(&mut file, expected_wal_id, expected_first_sequence, apply)?
                .verification(expected_wal_id, expected_first_sequence),
        )
    }

    pub(super) fn append(&mut self, kind: MutationKind, key: &[u8], value: &[u8]) -> Result<u64> {
        self.append_with_post_sync_hook(kind, key, value, || Ok(()))
    }

    fn append_with_post_sync_hook(
        &mut self,
        kind: MutationKind,
        key: &[u8],
        value: &[u8],
        post_sync_hook: impl FnOnce() -> Result<()>,
    ) -> Result<u64> {
        let sequence = self.next_sequence;
        let next_sequence = sequence
            .checked_add(1)
            .ok_or_else(|| corruption(0, "WAL sequence exhausted before appending a mutation"))?;
        let next_record_count = self
            .record_count
            .checked_add(1)
            .ok_or_else(|| corruption(0, "WAL record count overflowed u64"))?;
        let encoded = encode_record(kind, sequence, key, value)?;
        let encoded_bytes = u64::try_from(encoded.len())
            .map_err(|_| corruption(0, "encoded WAL record length does not fit u64"))?;
        let next_acknowledged_valid_bytes = self
            .acknowledged_valid_bytes
            .checked_add(encoded_bytes)
            .ok_or_else(|| corruption(0, "acknowledged WAL byte boundary overflowed u64"))?;

        self.ensure_authoritative_path()?;
        let observed_eof = self.file.seek(SeekFrom::End(0))?;
        if observed_eof != self.acknowledged_valid_bytes {
            return Err(corruption(
                observed_eof,
                format!(
                    "active WAL physical EOF changed before mutation: observed {observed_eof}, previously acknowledged {}",
                    self.acknowledged_valid_bytes
                ),
            ));
        }
        let observed_prefix_hasher =
            hash_file_prefix(&mut self.file, self.acknowledged_valid_bytes)?;
        if observed_prefix_hasher.finalize() != self.acknowledged_prefix_hasher.clone().finalize() {
            return Err(corruption(
                0,
                "acknowledged WAL record changed before mutation: durable prefix fingerprint mismatch",
            ));
        }
        let observed_eof_after_hash = self.file.seek(SeekFrom::End(0))?;
        if observed_eof_after_hash != self.acknowledged_valid_bytes {
            return Err(corruption(
                observed_eof_after_hash,
                format!(
                    "active WAL physical EOF changed while validating mutation: observed {observed_eof_after_hash}, previously acknowledged {}",
                    self.acknowledged_valid_bytes
                ),
            ));
        }
        self.file.write_all(&encoded)?;
        self.file.sync_data()?;
        post_sync_hook()?;
        self.ensure_authoritative_path()?;
        self.acknowledged_prefix_hasher.update(&encoded);
        self.next_sequence = next_sequence;
        self.record_count = next_record_count;
        self.acknowledged_valid_bytes = next_acknowledged_valid_bytes;
        Ok(sequence)
    }

    fn ensure_authoritative_path(&self) -> Result<()> {
        let root = self.path.parent().ok_or_else(|| {
            corruption(0, "active WAL authoritative path has no engine root parent")
        })?;
        let root_metadata = std::fs::symlink_metadata(root).map_err(|error| {
            corruption(
                0,
                format!("active LSM engine root unavailable before mutation: {error}"),
            )
        })?;
        if !root_metadata.file_type().is_dir() {
            return Err(corruption(
                0,
                "active LSM engine root is not a direct directory",
            ));
        }

        let metadata = std::fs::symlink_metadata(&self.path).map_err(|error| {
            corruption(
                0,
                format!("active WAL authoritative path unavailable before mutation: {error}"),
            )
        })?;
        if !metadata.file_type().is_file() {
            return Err(corruption(
                0,
                "active WAL authoritative path is not a direct regular file",
            ));
        }
        let authoritative = File::open(&self.path).map_err(|error| {
            corruption(
                0,
                format!("active WAL authoritative path unavailable before mutation: {error}"),
            )
        })?;
        let live_handle = Handle::from_file(self.file.try_clone()?)?;
        let authoritative_handle = Handle::from_file(authoritative)?;
        if live_handle != authoritative_handle {
            return Err(corruption(
                0,
                "active WAL path now resolves to a different file object",
            ));
        }
        Ok(())
    }

    pub(super) fn recovered_tail(&self) -> Option<&RecoveredWalTail> {
        self.recovered_tail.as_ref()
    }

    pub(super) const fn wal_id(&self) -> u64 {
        self.wal_id
    }

    pub(super) const fn first_sequence(&self) -> u64 {
        self.first_sequence
    }

    pub(super) const fn next_sequence(&self) -> u64 {
        self.next_sequence
    }

    pub(super) const fn record_count(&self) -> u64 {
        self.record_count
    }

    pub(super) const fn open_examined_bytes(&self) -> u64 {
        self.open_examined_bytes
    }
}

struct ScanResult {
    file_bytes: u64,
    valid_bytes: u64,
    record_count: u64,
    next_sequence: u64,
    recoverable_tail: Option<RecoveredWalTail>,
}

impl ScanResult {
    fn verification(&self, wal_id: u64, first_sequence: u64) -> WalVerification {
        WalVerification {
            format_version: WAL_FORMAT_VERSION,
            wal_id,
            first_sequence,
            file_bytes: self.file_bytes,
            valid_bytes: self.valid_bytes,
            record_count: self.record_count,
            next_sequence: self.next_sequence,
            recoverable_tail: self.recoverable_tail.clone(),
        }
    }
}

struct RecordMetadata {
    kind: MutationKind,
    sequence: u64,
    key_len: usize,
    value_len: usize,
    total_len: u64,
    expected_record_crc: u32,
}

fn scan_file(
    file: &mut File,
    expected_wal_id: u64,
    expected_first_sequence: u64,
    mut apply: impl FnMut(Mutation) -> Result<()>,
) -> Result<ScanResult> {
    let file_bytes = file.metadata()?.len();
    if file_bytes < WAL_HEADER_LEN_U64 {
        return Err(corruption(
            0,
            format!("truncated LSM WAL header: found {file_bytes} bytes, need {WAL_HEADER_LEN}"),
        ));
    }

    file.seek(SeekFrom::Start(0))?;
    let mut wal_header = [0_u8; WAL_HEADER_LEN];
    file.read_exact(&mut wal_header)?;
    validate_wal_header(&wal_header, expected_wal_id, expected_first_sequence)?;

    let mut offset = WAL_HEADER_LEN_U64;
    let mut expected_sequence = expected_first_sequence;
    let mut record_count = 0_u64;
    while offset < file_bytes {
        let remaining = file_bytes
            .checked_sub(offset)
            .ok_or_else(|| corruption(offset, "WAL offset exceeded physical file size"))?;
        if remaining < RECORD_HEADER_LEN_U64 {
            let available = usize::try_from(remaining)
                .map_err(|_| corruption(offset, "WAL tail length does not fit usize"))?;
            let mut partial = [0_u8; RECORD_HEADER_LEN];
            file.seek(SeekFrom::Start(offset))?;
            file.read_exact(&mut partial[..available])?;
            validate_partial_record_header(&partial[..available], offset, expected_sequence)?;
            return Ok(ScanResult {
                file_bytes,
                valid_bytes: offset,
                record_count,
                next_sequence: expected_sequence,
                recoverable_tail: Some(RecoveredWalTail {
                    record_offset: offset,
                    available_bytes: remaining,
                    required_bytes: None,
                }),
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
                recoverable_tail: Some(RecoveredWalTail {
                    record_offset: offset,
                    available_bytes: remaining,
                    required_bytes: Some(metadata.total_len),
                }),
            });
        }

        let payload_len = metadata
            .key_len
            .checked_add(metadata.value_len)
            .ok_or_else(|| corruption(offset, "WAL payload length overflowed usize"))?;
        let mut payload = vec![0_u8; payload_len];
        file.read_exact(&mut payload)?;
        let mut hasher = Hasher::new();
        hasher.update(&header[..28]);
        hasher.update(&payload);
        let actual_record_crc = hasher.finalize();
        if actual_record_crc != metadata.expected_record_crc {
            return Err(corruption(
                offset + 28,
                format!(
                    "WAL record checksum mismatch: expected {:08x}, computed {actual_record_crc:08x}",
                    metadata.expected_record_crc
                ),
            ));
        }

        let key = payload[..metadata.key_len].to_vec();
        let value = match metadata.kind {
            MutationKind::Put => Some(payload[metadata.key_len..].to_vec()),
            MutationKind::Delete => None,
        };
        apply(Mutation {
            sequence: metadata.sequence,
            key,
            value,
        })?;

        offset = record_end;
        expected_sequence = metadata
            .sequence
            .checked_add(1)
            .ok_or_else(|| corruption(offset, "WAL sequence exhausted during replay"))?;
        record_count = record_count
            .checked_add(1)
            .ok_or_else(|| corruption(offset, "WAL record count overflowed during replay"))?;
    }

    Ok(ScanResult {
        file_bytes,
        valid_bytes: offset,
        record_count,
        next_sequence: expected_sequence,
        recoverable_tail: None,
    })
}

fn hash_file_prefix(file: &mut File, prefix_bytes: u64) -> Result<Hasher> {
    file.seek(SeekFrom::Start(0))?;
    let mut remaining = prefix_bytes;
    let mut buffer = [0_u8; 8192];
    let mut hasher = Hasher::new();
    while remaining != 0 {
        let chunk_u64 = remaining.min(buffer.len() as u64);
        let chunk = usize::try_from(chunk_u64)
            .map_err(|_| corruption(0, "WAL prefix hash chunk length does not fit usize"))?;
        file.read_exact(&mut buffer[..chunk])?;
        hasher.update(&buffer[..chunk]);
        remaining -= chunk_u64;
    }
    Ok(hasher)
}

fn encode_wal_header(wal_id: u64, first_sequence: u64) -> [u8; WAL_HEADER_LEN] {
    let mut header = [0_u8; WAL_HEADER_LEN];
    header[..8].copy_from_slice(&WAL_MAGIC);
    header[8..10].copy_from_slice(&WAL_FORMAT_VERSION.to_le_bytes());
    header[10..12].copy_from_slice(&(WAL_HEADER_LEN as u16).to_le_bytes());
    header[12..16].copy_from_slice(&0_u32.to_le_bytes());
    header[16..24].copy_from_slice(&wal_id.to_le_bytes());
    header[24..32].copy_from_slice(&first_sequence.to_le_bytes());
    header[32..36].copy_from_slice(&0_u32.to_le_bytes());
    let checksum = crc32fast::hash(&header[..36]);
    header[36..40].copy_from_slice(&checksum.to_le_bytes());
    header
}

pub(super) fn encode_record(
    kind: MutationKind,
    sequence: u64,
    key: &[u8],
    value: &[u8],
) -> Result<Vec<u8>> {
    validate_key_value(key, value)?;
    if kind == MutationKind::Delete && !value.is_empty() {
        return Err(DbError::InvalidInput(
            "LSM WAL delete record cannot contain a value".to_owned(),
        ));
    }
    let key_len = u32::try_from(key.len())
        .map_err(|_| DbError::InvalidInput("LSM WAL key length does not fit u32".to_owned()))?;
    let value_len = u32::try_from(value.len())
        .map_err(|_| DbError::InvalidInput("LSM WAL value length does not fit u32".to_owned()))?;
    let payload_len = key
        .len()
        .checked_add(value.len())
        .ok_or_else(|| DbError::InvalidInput("LSM WAL payload overflowed usize".to_owned()))?;
    let capacity = RECORD_HEADER_LEN
        .checked_add(payload_len)
        .ok_or_else(|| DbError::InvalidInput("LSM WAL record overflowed usize".to_owned()))?;

    let mut header = [0_u8; RECORD_HEADER_LEN];
    header[..4].copy_from_slice(&RECORD_MAGIC);
    header[4] = RECORD_VERSION;
    header[5] = kind.encoded();
    header[6..8].copy_from_slice(&0_u16.to_le_bytes());
    header[8..16].copy_from_slice(&sequence.to_le_bytes());
    header[16..20].copy_from_slice(&key_len.to_le_bytes());
    header[20..24].copy_from_slice(&value_len.to_le_bytes());
    let header_crc = crc32fast::hash(&header[..24]);
    header[24..28].copy_from_slice(&header_crc.to_le_bytes());
    let mut record_hasher = Hasher::new();
    record_hasher.update(&header[..28]);
    record_hasher.update(key);
    record_hasher.update(value);
    header[28..32].copy_from_slice(&record_hasher.finalize().to_le_bytes());

    let mut encoded = Vec::with_capacity(capacity);
    encoded.extend_from_slice(&header);
    encoded.extend_from_slice(key);
    encoded.extend_from_slice(value);
    Ok(encoded)
}

fn validate_wal_header(
    header: &[u8; WAL_HEADER_LEN],
    expected_wal_id: u64,
    expected_first_sequence: u64,
) -> Result<()> {
    if header[..8] != WAL_MAGIC {
        return Err(corruption(0, "LSM WAL magic mismatch"));
    }
    let expected_crc = read_u32(&header[36..40]);
    let actual_crc = crc32fast::hash(&header[..36]);
    if expected_crc != actual_crc {
        return Err(corruption(
            36,
            format!(
                "LSM WAL header checksum mismatch: expected {expected_crc:08x}, computed {actual_crc:08x}"
            ),
        ));
    }
    let version = read_u16(&header[8..10]);
    if version != WAL_FORMAT_VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "LSM WAL",
            found: u64::from(version),
            supported: u64::from(WAL_FORMAT_VERSION),
        });
    }
    let header_len = read_u16(&header[10..12]);
    if usize::from(header_len) != WAL_HEADER_LEN {
        return Err(corruption(
            10,
            format!("LSM WAL declares header length {header_len}"),
        ));
    }
    if read_u32(&header[12..16]) != 0 {
        return Err(corruption(12, "LSM WAL has unsupported flags"));
    }
    let wal_id = read_u64(&header[16..24]);
    if wal_id != expected_wal_id {
        return Err(corruption(
            16,
            format!("LSM WAL id {wal_id} does not match authoritative id {expected_wal_id}"),
        ));
    }
    let first_sequence = read_u64(&header[24..32]);
    if first_sequence != expected_first_sequence {
        return Err(corruption(
            24,
            format!(
                "LSM WAL first sequence {first_sequence} does not match authoritative sequence {expected_first_sequence}"
            ),
        ));
    }
    if wal_id == 0 || first_sequence == 0 {
        return Err(corruption(
            16,
            "LSM WAL id and first sequence must both be nonzero",
        ));
    }
    if read_u32(&header[32..36]) != 0 {
        return Err(corruption(32, "LSM WAL reserved header bytes are nonzero"));
    }
    Ok(())
}

fn parse_record_header(
    header: &[u8; RECORD_HEADER_LEN],
    offset: u64,
    expected_sequence: u64,
) -> Result<RecordMetadata> {
    if header[..4] != RECORD_MAGIC {
        return Err(corruption(offset, "LSM WAL record magic mismatch"));
    }
    let expected_header_crc = read_u32(&header[24..28]);
    let actual_header_crc = crc32fast::hash(&header[..24]);
    if expected_header_crc != actual_header_crc {
        return Err(corruption(
            offset + 24,
            format!(
                "LSM WAL record header checksum mismatch: expected {expected_header_crc:08x}, computed {actual_header_crc:08x}"
            ),
        ));
    }
    if header[4] != RECORD_VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "LSM WAL record",
            found: u64::from(header[4]),
            supported: u64::from(RECORD_VERSION),
        });
    }
    let kind = MutationKind::decode(header[5], offset)?;
    if read_u16(&header[6..8]) != 0 {
        return Err(corruption(
            offset + 6,
            "LSM WAL record has unsupported flags",
        ));
    }
    let sequence = read_u64(&header[8..16]);
    if sequence != expected_sequence {
        return Err(corruption(
            offset + 8,
            format!("LSM WAL record sequence {sequence}, expected {expected_sequence}"),
        ));
    }
    let key_len_u32 = read_u32(&header[16..20]);
    let value_len_u32 = read_u32(&header[20..24]);
    let key_len = usize::try_from(key_len_u32)
        .map_err(|_| corruption(offset + 16, "WAL key length does not fit usize"))?;
    let value_len = usize::try_from(value_len_u32)
        .map_err(|_| corruption(offset + 20, "WAL value length does not fit usize"))?;
    validate_record_lengths(kind, key_len, value_len, offset)?;
    let payload_len = u64::from(key_len_u32)
        .checked_add(u64::from(value_len_u32))
        .ok_or_else(|| corruption(offset, "WAL payload length overflowed u64"))?;
    let total_len = RECORD_HEADER_LEN_U64
        .checked_add(payload_len)
        .ok_or_else(|| corruption(offset, "WAL record length overflowed u64"))?;
    Ok(RecordMetadata {
        kind,
        sequence,
        key_len,
        value_len,
        total_len,
        expected_record_crc: read_u32(&header[28..32]),
    })
}

fn validate_partial_record_header(bytes: &[u8], offset: u64, expected_sequence: u64) -> Result<()> {
    let magic_bytes = bytes.len().min(RECORD_MAGIC.len());
    if bytes[..magic_bytes] != RECORD_MAGIC[..magic_bytes] {
        return Err(corruption(
            offset,
            "unrecognized bytes after the final complete WAL record",
        ));
    }
    if bytes.len() >= 5 && bytes[4] != RECORD_VERSION {
        return Err(corruption(offset + 4, "invalid partial WAL record version"));
    }
    if bytes.len() >= 6 {
        MutationKind::decode(bytes[5], offset)?;
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
            .map_err(|_| corruption(offset + 16, "partial WAL key length does not fit usize"))?;
        if key_len > MAX_KEY_BYTES {
            return Err(corruption(
                offset + 16,
                format!("declared WAL key length {key_len} exceeds {MAX_KEY_BYTES}"),
            ));
        }
    }
    if bytes.len() >= 24 {
        let kind = MutationKind::decode(bytes[5], offset)?;
        let key_len = usize::try_from(read_u32(&bytes[16..20]))
            .map_err(|_| corruption(offset + 16, "partial WAL key length does not fit usize"))?;
        let value_len = usize::try_from(read_u32(&bytes[20..24]))
            .map_err(|_| corruption(offset + 20, "partial WAL value length does not fit usize"))?;
        validate_record_lengths(kind, key_len, value_len, offset)?;
    }
    if bytes.len() >= 28 {
        let expected_crc = read_u32(&bytes[24..28]);
        let actual_crc = crc32fast::hash(&bytes[..24]);
        if expected_crc != actual_crc {
            return Err(corruption(
                offset + 24,
                "partial WAL record header checksum mismatch",
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
        let delta = u64::try_from(field_offset)
            .map_err(|_| corruption(record_offset, "WAL field offset does not fit u64"))?;
        return Err(corruption(
            record_offset + delta,
            format!("invalid {field_name} prefix in partial WAL tail"),
        ));
    }
    Ok(())
}

fn validate_record_lengths(
    kind: MutationKind,
    key_len: usize,
    value_len: usize,
    offset: u64,
) -> Result<()> {
    if key_len > MAX_KEY_BYTES {
        return Err(corruption(
            offset + 16,
            format!("declared WAL key length {key_len} exceeds {MAX_KEY_BYTES}"),
        ));
    }
    if value_len > MAX_VALUE_BYTES {
        return Err(corruption(
            offset + 20,
            format!("declared WAL value length {value_len} exceeds {MAX_VALUE_BYTES}"),
        ));
    }
    if kind == MutationKind::Delete && value_len != 0 {
        return Err(corruption(
            offset + 20,
            format!("WAL tombstone declares nonzero value length {value_len}"),
        ));
    }
    key_len
        .checked_add(value_len)
        .ok_or_else(|| corruption(offset, "LSM WAL payload length overflowed usize"))?;
    Ok(())
}

pub(super) fn checked_record_end(offset: u64, record_len: u64) -> Result<u64> {
    offset
        .checked_add(record_len)
        .ok_or_else(|| corruption(offset, "LSM WAL record end overflowed u64"))
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

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use tempfile::tempdir;

    #[test]
    fn append_rejects_authoritative_path_replacement_after_sync_before_acknowledgement() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join(file_name(INITIAL_WAL_ID));
        let detached = directory.path().join("detached-wal.log");
        let mut wal =
            Wal::create_new(&path, INITIAL_WAL_ID, INITIAL_FIRST_SEQUENCE).expect("create WAL");
        let authoritative_before = std::fs::read(&path).expect("read initial WAL");

        let result = wal.append_with_post_sync_hook(MutationKind::Put, b"key", b"value", || {
            std::fs::rename(&path, &detached)?;
            std::fs::write(&path, &authoritative_before)?;
            Ok(())
        });

        assert!(
            matches!(result, Err(DbError::Corruption { .. })),
            "mutation must fail closed when authoritative WAL ownership changes after sync: {result:?}"
        );
        assert_eq!(
            std::fs::read(&path).expect("read replacement WAL"),
            authoritative_before,
            "replacement authoritative WAL must not contain the stale-handle mutation"
        );
    }
}
