use std::collections::BTreeMap;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};

use db_core::{DbError, Result, MAX_KEY_BYTES, MAX_VALUE_BYTES};

use crate::bloom::BloomFilter;
use crate::memtable::VersionedEntry;

pub(super) const SSTABLE_HEADER_LEN: usize = 64;
const SSTABLE_FOOTER_LEN: usize = 64;
const RECORD_HEADER_LEN: usize = 28;
const INDEX_PREFIX_LEN: usize = 24;
const SSTABLE_MAGIC: [u8; 8] = *b"DBLSMSST";
const SSTABLE_FOOTER_MAGIC: [u8; 8] = *b"DBLSMEND";
const RECORD_MAGIC: [u8; 4] = *b"SSTR";
const LEGACY_FORMAT_VERSION: u16 = 1;
const FORMAT_VERSION: u16 = 2;
const RECORD_VERSION: u8 = 1;
const INDEX_VERSION: u8 = 1;
const KIND_PUT: u8 = 1;
const KIND_DELETE: u8 = 2;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct SstableDescriptor {
    pub(super) table_id: u64,
    pub(super) level: u32,
    pub(super) file_bytes: u64,
    pub(super) entry_count: u64,
    pub(super) durable_sequence: u64,
    pub(super) smallest_key: Vec<u8>,
    pub(super) largest_key: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum EntryKind {
    Put,
    Delete,
}

impl EntryKind {
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
                offset,
                format!("unknown SSTable entry kind {encoded}"),
            )),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct IndexEntry {
    key: Vec<u8>,
    sequence: u64,
    kind: EntryKind,
    record_offset: u64,
}

#[derive(Debug)]
pub(super) struct SsTable {
    path: PathBuf,
    descriptor: SstableDescriptor,
    bytes: Vec<u8>,
    index: Vec<IndexEntry>,
    bloom: Option<BloomFilter>,
}

impl SsTable {
    pub(super) fn create_new(
        directory: &Path,
        table_id: u64,
        durable_sequence: u64,
        entries: &BTreeMap<Vec<u8>, VersionedEntry>,
    ) -> Result<Self> {
        Self::create_new_with_format(
            directory,
            table_id,
            0,
            durable_sequence,
            entries,
            FORMAT_VERSION,
        )
    }

    pub(super) fn create_new_at_level(
        directory: &Path,
        table_id: u64,
        level: u32,
        durable_sequence: u64,
        entries: &BTreeMap<Vec<u8>, VersionedEntry>,
    ) -> Result<Self> {
        Self::create_new_with_format(
            directory,
            table_id,
            level,
            durable_sequence,
            entries,
            FORMAT_VERSION,
        )
    }

    #[cfg(test)]
    pub(super) fn create_legacy_v1_for_test(
        directory: &Path,
        table_id: u64,
        durable_sequence: u64,
        entries: &BTreeMap<Vec<u8>, VersionedEntry>,
    ) -> Result<Self> {
        Self::create_new_with_format(
            directory,
            table_id,
            0,
            durable_sequence,
            entries,
            LEGACY_FORMAT_VERSION,
        )
    }

    fn create_new_with_format(
        directory: &Path,
        table_id: u64,
        level: u32,
        durable_sequence: u64,
        entries: &BTreeMap<Vec<u8>, VersionedEntry>,
        format_version: u16,
    ) -> Result<Self> {
        if entries.is_empty() {
            return Err(corruption(0, "cannot create an empty SSTable"));
        }
        if format_version != LEGACY_FORMAT_VERSION && format_version != FORMAT_VERSION {
            return Err(corruption(0, "cannot create unsupported SSTable format"));
        }
        let entry_count = u64::try_from(entries.len())
            .map_err(|_| corruption(0, "SSTable entry count does not fit u64"))?;
        let mut bytes = vec![0_u8; SSTABLE_HEADER_LEN];
        if format_version == FORMAT_VERSION {
            let bloom = BloomFilter::build(entries.keys().map(Vec::as_slice), entries.len())?;
            bytes.extend_from_slice(&bloom.encode()?);
        }
        let data_offset = u64::try_from(bytes.len())
            .map_err(|_| corruption(0, "SSTable data offset does not fit u64"))?;
        let mut record_offsets = Vec::with_capacity(entries.len());

        for (key, entry) in entries {
            validate_entry_bounds(key, entry.value.as_deref())?;
            let record_offset = u64::try_from(bytes.len())
                .map_err(|_| corruption(0, "SSTable record offset does not fit u64"))?;
            record_offsets.push(record_offset);
            bytes.extend_from_slice(&encode_record(key, entry)?);
        }

        let index_offset = u64::try_from(bytes.len())
            .map_err(|_| corruption(0, "SSTable index offset does not fit u64"))?;
        for ((key, entry), record_offset) in entries.iter().zip(record_offsets) {
            bytes.extend_from_slice(&encode_index_entry(key, entry, record_offset)?);
        }
        let footer_offset = u64::try_from(bytes.len())
            .map_err(|_| corruption(0, "SSTable footer offset does not fit u64"))?;
        bytes.resize(
            bytes
                .len()
                .checked_add(SSTABLE_FOOTER_LEN)
                .ok_or_else(|| corruption(0, "SSTable size overflowed usize"))?,
            0,
        );

        let header = encode_header(
            format_version,
            table_id,
            entry_count,
            data_offset,
            index_offset,
            footer_offset,
        );
        bytes[..SSTABLE_HEADER_LEN].copy_from_slice(&header);
        let whole_crc = crc32fast::hash(&bytes[..usize_from_u64(footer_offset, 0)?]);
        let footer = encode_footer(
            format_version,
            table_id,
            entry_count,
            index_offset,
            footer_offset,
            durable_sequence,
            whole_crc,
        );
        let footer_start = usize_from_u64(footer_offset, 0)?;
        bytes[footer_start..footer_start + SSTABLE_FOOTER_LEN].copy_from_slice(&footer);

        let path = directory.join(file_name(table_id));
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)?;
        file.write_all(&bytes)?;
        file.sync_all()?;

        let descriptor = SstableDescriptor {
            table_id,
            level,
            file_bytes: u64::try_from(bytes.len())
                .map_err(|_| corruption(0, "SSTable file size does not fit u64"))?,
            entry_count,
            durable_sequence,
            smallest_key: entries
                .first_key_value()
                .expect("nonempty checked above")
                .0
                .clone(),
            largest_key: entries
                .last_key_value()
                .expect("nonempty checked above")
                .0
                .clone(),
        };
        Self::open(&path, descriptor)
    }

    pub(super) fn open(path: &Path, descriptor: SstableDescriptor) -> Result<Self> {
        let bytes = fs::read(path)?;
        let file_bytes = u64::try_from(bytes.len())
            .map_err(|_| corruption(0, "SSTable file length does not fit u64"))?;
        if file_bytes != descriptor.file_bytes {
            return Err(corruption(
                0,
                format!(
                    "SSTable {} length mismatch: manifest {}, file {file_bytes}",
                    descriptor.table_id, descriptor.file_bytes
                ),
            ));
        }
        if bytes.len() < SSTABLE_HEADER_LEN + SSTABLE_FOOTER_LEN {
            return Err(corruption(0, "truncated SSTable header/footer"));
        }

        let header = parse_header(&bytes[..SSTABLE_HEADER_LEN])?;
        if header.table_id != descriptor.table_id || header.entry_count != descriptor.entry_count {
            return Err(corruption(
                0,
                "SSTable header does not match manifest descriptor",
            ));
        }
        let footer_start = usize_from_u64(header.footer_offset, 0)?;
        let footer_end = footer_start
            .checked_add(SSTABLE_FOOTER_LEN)
            .ok_or_else(|| corruption(header.footer_offset, "SSTable footer extent overflowed"))?;
        if footer_end != bytes.len() {
            return Err(corruption(
                header.footer_offset,
                "SSTable footer is not the exact physical tail",
            ));
        }
        let footer = parse_footer(&bytes[footer_start..footer_end], header.footer_offset)?;
        if footer.format_version != header.format_version
            || footer.table_id != header.table_id
            || footer.entry_count != header.entry_count
            || footer.index_offset != header.index_offset
            || footer.footer_offset != header.footer_offset
            || footer.durable_sequence != descriptor.durable_sequence
        {
            return Err(corruption(
                header.footer_offset,
                "SSTable footer metadata disagrees with header/manifest",
            ));
        }
        let actual_whole_crc = crc32fast::hash(&bytes[..footer_start]);
        if actual_whole_crc != footer.whole_crc {
            return Err(corruption(
                header.footer_offset + 56,
                "SSTable whole-file checksum mismatch",
            ));
        }

        let data_start = usize_from_u64(header.data_offset, 0)?;
        let index_start = usize_from_u64(header.index_offset, 0)?;
        if index_start > footer_start || index_start < data_start {
            return Err(corruption(0, "invalid SSTable data/index extent ordering"));
        }
        let bloom = match header.format_version {
            LEGACY_FORMAT_VERSION => {
                if data_start != SSTABLE_HEADER_LEN {
                    return Err(corruption(
                        32,
                        "legacy SSTable v1 data must begin immediately after the header",
                    ));
                }
                None
            }
            FORMAT_VERSION => {
                if data_start <= SSTABLE_HEADER_LEN {
                    return Err(corruption(32, "SSTable v2 is missing its Bloom section"));
                }
                Some(BloomFilter::decode(
                    &bytes[SSTABLE_HEADER_LEN..data_start],
                    SSTABLE_HEADER_LEN as u64,
                    header.entry_count,
                )?)
            }
            _ => unreachable!("header version validated before extent parsing"),
        };

        let expected_count = usize_from_u64(header.entry_count, 0)?;
        let mut records = Vec::with_capacity(expected_count);
        let mut offset = data_start;
        while offset < index_start {
            let record_offset = u64::try_from(offset)
                .map_err(|_| corruption(0, "SSTable record offset does not fit u64"))?;
            let (key, entry, next) = decode_record(&bytes, offset, index_start)?;
            if records
                .last()
                .is_some_and(|previous: &(Vec<u8>, VersionedEntry, u64)| previous.0 >= key)
            {
                return Err(corruption(
                    record_offset,
                    "SSTable data keys are not strictly sorted",
                ));
            }
            records.push((key, entry, record_offset));
            offset = next;
        }
        if offset != index_start || records.len() != expected_count {
            return Err(corruption(
                header.index_offset,
                "SSTable data record count/extent mismatch",
            ));
        }

        let mut index = Vec::with_capacity(expected_count);
        offset = index_start;
        while offset < footer_start {
            let (entry, next) = decode_index_entry(&bytes, offset, footer_start)?;
            if index
                .last()
                .is_some_and(|previous: &IndexEntry| previous.key >= entry.key)
            {
                return Err(corruption(
                    u64::try_from(offset).unwrap_or(u64::MAX),
                    "SSTable index keys are not strictly sorted",
                ));
            }
            index.push(entry);
            offset = next;
        }
        if offset != footer_start || index.len() != expected_count {
            return Err(corruption(
                header.footer_offset,
                "SSTable index record count/extent mismatch",
            ));
        }

        for ((record_key, record_entry, record_offset), index_entry) in records.iter().zip(&index) {
            let expected_kind = if record_entry.value.is_some() {
                EntryKind::Put
            } else {
                EntryKind::Delete
            };
            if record_key != &index_entry.key
                || record_entry.sequence != index_entry.sequence
                || *record_offset != index_entry.record_offset
                || expected_kind != index_entry.kind
            {
                return Err(corruption(
                    index_entry.record_offset,
                    "SSTable index does not exactly describe data record",
                ));
            }
        }

        let first = index
            .first()
            .ok_or_else(|| corruption(0, "manifest references empty SSTable"))?;
        let last = index.last().expect("nonempty checked above");
        if first.key != descriptor.smallest_key || last.key != descriptor.largest_key {
            return Err(corruption(
                0,
                "SSTable key bounds disagree with manifest descriptor",
            ));
        }
        if index.iter().map(|entry| entry.sequence).max().unwrap_or(0) > descriptor.durable_sequence
        {
            return Err(corruption(
                0,
                "SSTable entry sequence exceeds durable watermark",
            ));
        }
        if bloom
            .as_ref()
            .is_some_and(|filter| index.iter().any(|entry| !filter.may_contain(&entry.key)))
        {
            return Err(corruption(
                SSTABLE_HEADER_LEN as u64,
                "SSTable Bloom filter has a false negative for an indexed key",
            ));
        }

        Ok(Self {
            path: path.to_path_buf(),
            descriptor,
            bytes,
            index,
            bloom,
        })
    }

    pub(super) fn descriptor(&self) -> &SstableDescriptor {
        &self.descriptor
    }

    pub(super) fn get(&self, key: &[u8]) -> Result<Option<VersionedEntry>> {
        if key < self.descriptor.smallest_key.as_slice()
            || key > self.descriptor.largest_key.as_slice()
            || self
                .bloom
                .as_ref()
                .is_some_and(|filter| !filter.may_contain(key))
        {
            return Ok(None);
        }
        let index = match self
            .index
            .binary_search_by(|entry| entry.key.as_slice().cmp(key))
        {
            Ok(index) => index,
            Err(_) => return Ok(None),
        };
        let entry = &self.index[index];
        let offset = usize_from_u64(entry.record_offset, entry.record_offset)?;
        let (_, decoded, _) = decode_record(&self.bytes, offset, self.bytes.len())?;
        Ok(Some(decoded))
    }

    pub(super) fn overlay_range(
        &self,
        start: &[u8],
        end: Option<&[u8]>,
        visible: &mut BTreeMap<Vec<u8>, VersionedEntry>,
    ) -> Result<u64> {
        let mut decoded_records = 0_u64;
        for entry in &self.index {
            if entry.key.as_slice() < start {
                continue;
            }
            if end.is_some_and(|end| entry.key.as_slice() >= end) {
                break;
            }
            let offset = usize_from_u64(entry.record_offset, entry.record_offset)?;
            let (key, decoded, _) = decode_record(&self.bytes, offset, self.bytes.len())?;
            decoded_records = decoded_records.saturating_add(1);
            let replace = visible
                .get(key.as_slice())
                .is_none_or(|current| decoded.sequence > current.sequence);
            if replace {
                visible.insert(key, decoded);
            }
        }
        Ok(decoded_records)
    }

    #[cfg(test)]
    pub(super) fn bloom_may_contain(&self, key: &[u8]) -> Option<bool> {
        self.bloom.as_ref().map(|filter| filter.may_contain(key))
    }

    #[cfg(test)]
    pub(super) fn format_version(&self) -> u16 {
        if self.bloom.is_some() {
            FORMAT_VERSION
        } else {
            LEGACY_FORMAT_VERSION
        }
    }

    #[allow(dead_code)]
    pub(super) fn path(&self) -> &Path {
        &self.path
    }
}

#[derive(Debug, Clone, Copy)]
struct Header {
    format_version: u16,
    table_id: u64,
    entry_count: u64,
    data_offset: u64,
    index_offset: u64,
    footer_offset: u64,
}

#[derive(Debug, Clone, Copy)]
struct Footer {
    format_version: u16,
    table_id: u64,
    entry_count: u64,
    index_offset: u64,
    footer_offset: u64,
    durable_sequence: u64,
    whole_crc: u32,
}

pub(super) fn file_name(table_id: u64) -> String {
    format!("sst-{table_id:016}.sst")
}

fn encode_header(
    format_version: u16,
    table_id: u64,
    entry_count: u64,
    data_offset: u64,
    index_offset: u64,
    footer_offset: u64,
) -> [u8; 64] {
    let mut header = [0_u8; 64];
    header[0..8].copy_from_slice(&SSTABLE_MAGIC);
    header[8..10].copy_from_slice(&format_version.to_le_bytes());
    header[10..12].copy_from_slice(&(SSTABLE_HEADER_LEN as u16).to_le_bytes());
    header[16..24].copy_from_slice(&table_id.to_le_bytes());
    header[24..32].copy_from_slice(&entry_count.to_le_bytes());
    header[32..40].copy_from_slice(&data_offset.to_le_bytes());
    header[40..48].copy_from_slice(&index_offset.to_le_bytes());
    header[48..56].copy_from_slice(&footer_offset.to_le_bytes());
    let checksum = crc32fast::hash(&header[..60]);
    header[60..64].copy_from_slice(&checksum.to_le_bytes());
    header
}

fn parse_header(bytes: &[u8]) -> Result<Header> {
    if bytes.len() != SSTABLE_HEADER_LEN {
        return Err(corruption(0, "invalid SSTable header length"));
    }
    if bytes[0..8] != SSTABLE_MAGIC {
        return Err(corruption(0, "invalid SSTable magic"));
    }
    let version = u16::from_le_bytes(bytes[8..10].try_into().expect("fixed slice"));
    if version != LEGACY_FORMAT_VERSION && version != FORMAT_VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "LSM SSTable",
            found: u64::from(version),
            supported: u64::from(FORMAT_VERSION),
        });
    }
    if u16::from_le_bytes(bytes[10..12].try_into().expect("fixed slice")) as usize
        != SSTABLE_HEADER_LEN
    {
        return Err(corruption(10, "invalid SSTable header-size field"));
    }
    if bytes[12..16] != [0; 4] || bytes[56..60] != [0; 4] {
        return Err(corruption(12, "nonzero reserved SSTable header bytes"));
    }
    let expected = u32::from_le_bytes(bytes[60..64].try_into().expect("fixed slice"));
    if crc32fast::hash(&bytes[..60]) != expected {
        return Err(corruption(60, "SSTable header checksum mismatch"));
    }
    Ok(Header {
        format_version: version,
        table_id: u64::from_le_bytes(bytes[16..24].try_into().expect("fixed slice")),
        entry_count: u64::from_le_bytes(bytes[24..32].try_into().expect("fixed slice")),
        data_offset: u64::from_le_bytes(bytes[32..40].try_into().expect("fixed slice")),
        index_offset: u64::from_le_bytes(bytes[40..48].try_into().expect("fixed slice")),
        footer_offset: u64::from_le_bytes(bytes[48..56].try_into().expect("fixed slice")),
    })
}

fn encode_footer(
    format_version: u16,
    table_id: u64,
    entry_count: u64,
    index_offset: u64,
    footer_offset: u64,
    durable_sequence: u64,
    whole_crc: u32,
) -> [u8; 64] {
    let mut footer = [0_u8; 64];
    footer[0..8].copy_from_slice(&SSTABLE_FOOTER_MAGIC);
    footer[8..10].copy_from_slice(&format_version.to_le_bytes());
    footer[10..12].copy_from_slice(&(SSTABLE_FOOTER_LEN as u16).to_le_bytes());
    footer[16..24].copy_from_slice(&table_id.to_le_bytes());
    footer[24..32].copy_from_slice(&entry_count.to_le_bytes());
    footer[32..40].copy_from_slice(&index_offset.to_le_bytes());
    footer[40..48].copy_from_slice(&footer_offset.to_le_bytes());
    footer[48..56].copy_from_slice(&durable_sequence.to_le_bytes());
    footer[56..60].copy_from_slice(&whole_crc.to_le_bytes());
    let checksum = crc32fast::hash(&footer[..60]);
    footer[60..64].copy_from_slice(&checksum.to_le_bytes());
    footer
}

fn parse_footer(bytes: &[u8], offset: u64) -> Result<Footer> {
    if bytes.len() != SSTABLE_FOOTER_LEN || bytes[0..8] != SSTABLE_FOOTER_MAGIC {
        return Err(corruption(offset, "invalid SSTable footer"));
    }
    let version = u16::from_le_bytes(bytes[8..10].try_into().expect("fixed slice"));
    if version != LEGACY_FORMAT_VERSION && version != FORMAT_VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "LSM SSTable",
            found: u64::from(version),
            supported: u64::from(FORMAT_VERSION),
        });
    }
    if u16::from_le_bytes(bytes[10..12].try_into().expect("fixed slice")) as usize
        != SSTABLE_FOOTER_LEN
        || bytes[12..16] != [0; 4]
    {
        return Err(corruption(offset + 10, "invalid SSTable footer fields"));
    }
    let expected = u32::from_le_bytes(bytes[60..64].try_into().expect("fixed slice"));
    if crc32fast::hash(&bytes[..60]) != expected {
        return Err(corruption(offset + 60, "SSTable footer checksum mismatch"));
    }
    Ok(Footer {
        format_version: version,
        table_id: u64::from_le_bytes(bytes[16..24].try_into().expect("fixed slice")),
        entry_count: u64::from_le_bytes(bytes[24..32].try_into().expect("fixed slice")),
        index_offset: u64::from_le_bytes(bytes[32..40].try_into().expect("fixed slice")),
        footer_offset: u64::from_le_bytes(bytes[40..48].try_into().expect("fixed slice")),
        durable_sequence: u64::from_le_bytes(bytes[48..56].try_into().expect("fixed slice")),
        whole_crc: u32::from_le_bytes(bytes[56..60].try_into().expect("fixed slice")),
    })
}

fn encode_record(key: &[u8], entry: &VersionedEntry) -> Result<Vec<u8>> {
    let kind = if entry.value.is_some() {
        EntryKind::Put
    } else {
        EntryKind::Delete
    };
    let value = entry.value.as_deref().unwrap_or_default();
    let key_len =
        u32::try_from(key.len()).map_err(|_| corruption(0, "key length does not fit u32"))?;
    let value_len =
        u32::try_from(value.len()).map_err(|_| corruption(0, "value length does not fit u32"))?;
    let capacity = RECORD_HEADER_LEN
        .checked_add(key.len())
        .and_then(|size| size.checked_add(value.len()))
        .and_then(|size| size.checked_add(4))
        .ok_or_else(|| corruption(0, "SSTable record size overflowed usize"))?;
    let mut encoded = Vec::with_capacity(capacity);
    encoded.extend_from_slice(&RECORD_MAGIC);
    encoded.push(RECORD_VERSION);
    encoded.push(kind.encoded());
    encoded.extend_from_slice(&0_u16.to_le_bytes());
    encoded.extend_from_slice(&entry.sequence.to_le_bytes());
    encoded.extend_from_slice(&key_len.to_le_bytes());
    encoded.extend_from_slice(&value_len.to_le_bytes());
    let header_crc = crc32fast::hash(&encoded);
    encoded.extend_from_slice(&header_crc.to_le_bytes());
    encoded.extend_from_slice(key);
    encoded.extend_from_slice(value);
    let record_crc = crc32fast::hash(&encoded);
    encoded.extend_from_slice(&record_crc.to_le_bytes());
    Ok(encoded)
}

fn decode_record(
    bytes: &[u8],
    offset: usize,
    limit: usize,
) -> Result<(Vec<u8>, VersionedEntry, usize)> {
    let header_end = checked_add(offset, RECORD_HEADER_LEN, offset)?;
    if header_end > limit || header_end > bytes.len() {
        return Err(corruption(offset as u64, "truncated SSTable record header"));
    }
    let header = &bytes[offset..header_end];
    if header[0..4] != RECORD_MAGIC || header[4] != RECORD_VERSION || header[6..8] != [0; 2] {
        return Err(corruption(offset as u64, "invalid SSTable record header"));
    }
    let expected_header_crc = u32::from_le_bytes(header[24..28].try_into().expect("fixed slice"));
    if crc32fast::hash(&header[..24]) != expected_header_crc {
        return Err(corruption(
            offset as u64 + 24,
            "SSTable record header checksum mismatch",
        ));
    }
    let kind = EntryKind::decode(header[5], offset as u64 + 5)?;
    let sequence = u64::from_le_bytes(header[8..16].try_into().expect("fixed slice"));
    let key_len = usize::try_from(u32::from_le_bytes(
        header[16..20].try_into().expect("fixed slice"),
    ))
    .map_err(|_| corruption(offset as u64 + 16, "SSTable key length does not fit usize"))?;
    let value_len = usize::try_from(u32::from_le_bytes(
        header[20..24].try_into().expect("fixed slice"),
    ))
    .map_err(|_| {
        corruption(
            offset as u64 + 20,
            "SSTable value length does not fit usize",
        )
    })?;
    if key_len > MAX_KEY_BYTES || value_len > MAX_VALUE_BYTES {
        return Err(corruption(
            offset as u64,
            "SSTable record exceeds common key/value bounds",
        ));
    }
    if kind == EntryKind::Delete && value_len != 0 {
        return Err(corruption(
            offset as u64 + 20,
            "SSTable tombstone carries a value",
        ));
    }
    let payload_end = checked_add(header_end, key_len, offset)?;
    let payload_end = checked_add(payload_end, value_len, offset)?;
    let record_end = checked_add(payload_end, 4, offset)?;
    if record_end > limit || record_end > bytes.len() {
        return Err(corruption(
            offset as u64,
            "truncated SSTable record payload",
        ));
    }
    let expected_record_crc = u32::from_le_bytes(
        bytes[payload_end..record_end]
            .try_into()
            .expect("four-byte record crc"),
    );
    if crc32fast::hash(&bytes[offset..payload_end]) != expected_record_crc {
        return Err(corruption(
            payload_end as u64,
            "SSTable record checksum mismatch",
        ));
    }
    let key_start = header_end;
    let key_end = key_start + key_len;
    let key = bytes[key_start..key_end].to_vec();
    let value = match kind {
        EntryKind::Put => Some(bytes[key_end..payload_end].to_vec()),
        EntryKind::Delete => None,
    };
    Ok((key, VersionedEntry { sequence, value }, record_end))
}

fn encode_index_entry(key: &[u8], entry: &VersionedEntry, record_offset: u64) -> Result<Vec<u8>> {
    let kind = if entry.value.is_some() {
        EntryKind::Put
    } else {
        EntryKind::Delete
    };
    let key_len =
        u32::try_from(key.len()).map_err(|_| corruption(0, "index key length does not fit u32"))?;
    let capacity = INDEX_PREFIX_LEN
        .checked_add(key.len())
        .and_then(|size| size.checked_add(4))
        .ok_or_else(|| corruption(0, "SSTable index entry size overflowed usize"))?;
    let mut encoded = Vec::with_capacity(capacity);
    encoded.extend_from_slice(&key_len.to_le_bytes());
    encoded.push(kind.encoded());
    encoded.push(INDEX_VERSION);
    encoded.extend_from_slice(&0_u16.to_le_bytes());
    encoded.extend_from_slice(&entry.sequence.to_le_bytes());
    encoded.extend_from_slice(&record_offset.to_le_bytes());
    encoded.extend_from_slice(key);
    let checksum = crc32fast::hash(&encoded);
    encoded.extend_from_slice(&checksum.to_le_bytes());
    Ok(encoded)
}

fn decode_index_entry(bytes: &[u8], offset: usize, limit: usize) -> Result<(IndexEntry, usize)> {
    let prefix_end = checked_add(offset, INDEX_PREFIX_LEN, offset)?;
    if prefix_end > limit || prefix_end > bytes.len() {
        return Err(corruption(offset as u64, "truncated SSTable index prefix"));
    }
    let prefix = &bytes[offset..prefix_end];
    let key_len = usize::try_from(u32::from_le_bytes(
        prefix[0..4].try_into().expect("fixed slice"),
    ))
    .map_err(|_| corruption(offset as u64, "SSTable index key length does not fit usize"))?;
    if key_len > MAX_KEY_BYTES || prefix[5] != INDEX_VERSION || prefix[6..8] != [0; 2] {
        return Err(corruption(offset as u64, "invalid SSTable index entry"));
    }
    let kind = EntryKind::decode(prefix[4], offset as u64 + 4)?;
    let sequence = u64::from_le_bytes(prefix[8..16].try_into().expect("fixed slice"));
    let record_offset = u64::from_le_bytes(prefix[16..24].try_into().expect("fixed slice"));
    let key_end = checked_add(prefix_end, key_len, offset)?;
    let entry_end = checked_add(key_end, 4, offset)?;
    if entry_end > limit || entry_end > bytes.len() {
        return Err(corruption(
            offset as u64,
            "truncated SSTable index key/checksum",
        ));
    }
    let expected = u32::from_le_bytes(bytes[key_end..entry_end].try_into().expect("fixed slice"));
    if crc32fast::hash(&bytes[offset..key_end]) != expected {
        return Err(corruption(
            key_end as u64,
            "SSTable index checksum mismatch",
        ));
    }
    Ok((
        IndexEntry {
            key: bytes[prefix_end..key_end].to_vec(),
            sequence,
            kind,
            record_offset,
        },
        entry_end,
    ))
}

fn validate_entry_bounds(key: &[u8], value: Option<&[u8]>) -> Result<()> {
    if key.len() > MAX_KEY_BYTES {
        return Err(DbError::InvalidInput(format!(
            "key length {} exceeds maximum {MAX_KEY_BYTES}",
            key.len()
        )));
    }
    if value.is_some_and(|value| value.len() > MAX_VALUE_BYTES) {
        return Err(DbError::InvalidInput(format!(
            "value length {} exceeds maximum {MAX_VALUE_BYTES}",
            value.map_or(0, <[u8]>::len)
        )));
    }
    Ok(())
}

fn checked_add(base: usize, add: usize, offset: usize) -> Result<usize> {
    base.checked_add(add)
        .ok_or_else(|| corruption(offset as u64, "SSTable extent arithmetic overflowed usize"))
}

fn usize_from_u64(value: u64, offset: u64) -> Result<usize> {
    usize::try_from(value)
        .map_err(|_| corruption(offset, format!("SSTable extent {value} does not fit usize")))
}

fn corruption(offset: u64, reason: impl Into<String>) -> DbError {
    DbError::Corruption {
        offset,
        reason: reason.into(),
    }
}
