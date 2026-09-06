use std::fs::{self, File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

use db_core::{DbError, Result, MAX_KEY_BYTES};

use crate::sstable::SstableDescriptor;

pub(super) const CURRENT_FILE_NAME: &str = "CURRENT";
pub(super) const CURRENT_SLOT_BYTES: usize = 4096;
const CURRENT_FILE_BYTES: usize = CURRENT_SLOT_BYTES * 2;
const CURRENT_MAGIC: [u8; 8] = *b"DBLSMCUR";
const CURRENT_FORMAT_VERSION: u16 = 1;
const MANIFEST_MAGIC: [u8; 8] = *b"DBLSMMAN";
const MANIFEST_FORMAT_VERSION_V1: u16 = 1;
const MANIFEST_FORMAT_VERSION_V2: u16 = 2;
const MANIFEST_FORMAT_VERSION_V3: u16 = 3;
const MANIFEST_FORMAT_VERSION_V4: u16 = 4;
const MANIFEST_FORMAT_VERSION: u16 = 5;
const MANIFEST_HEADER_LEN_V1: usize = 64;
const MANIFEST_HEADER_LEN_V4: usize = 80;
const MANIFEST_HEADER_LEN: usize = 88;
const LEGACY_DESCRIPTOR_PREFIX_LEN: usize = 40;
const DESCRIPTOR_PREFIX_LEN: usize = 48;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct VersionSet {
    pub(super) current_generation: u64,
    pub(super) manifest_id: u64,
    pub(super) durable_sequence: u64,
    pub(super) tombstone_gc_sequence: u64,
    pub(super) table_id_high_watermark: u64,
    pub(super) wal_id: u64,
    pub(super) wal_first_sequence: u64,
    pub(super) tables: Vec<SstableDescriptor>,
}

pub(super) struct ManifestState {
    pub(super) durable_sequence: u64,
    pub(super) tombstone_gc_sequence: u64,
    pub(super) wal_id: u64,
    pub(super) wal_first_sequence: u64,
    pub(super) tables: Vec<SstableDescriptor>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct CurrentSlot {
    generation: u64,
    manifest_id: u64,
}

pub(super) fn manifest_file_name(manifest_id: u64) -> String {
    format!("MANIFEST-{manifest_id:016}")
}

pub(super) fn create_initial(
    directory: &Path,
    wal_id: u64,
    wal_first_sequence: u64,
) -> Result<VersionSet> {
    let version = VersionSet {
        current_generation: 0,
        manifest_id: 1,
        durable_sequence: 0,
        tombstone_gc_sequence: 0,
        table_id_high_watermark: 0,
        wal_id,
        wal_first_sequence,
        tables: Vec::new(),
    };
    write_manifest_new(directory, &version)?;

    let current_path = directory.join(CURRENT_FILE_NAME);
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&current_path)?;
    let mut bytes = vec![0_u8; CURRENT_FILE_BYTES];
    let slot0 = encode_current_slot(0, version.current_generation, version.manifest_id);
    let slot1 = encode_current_slot(1, version.current_generation, version.manifest_id);
    bytes[..CURRENT_SLOT_BYTES].copy_from_slice(&slot0);
    bytes[CURRENT_SLOT_BYTES..].copy_from_slice(&slot1);
    file.write_all(&bytes)?;
    file.sync_all()?;
    Ok(version)
}

pub(super) fn load(directory: &Path) -> Result<VersionSet> {
    let current = read_current(&directory.join(CURRENT_FILE_NAME))?;
    let mut version = read_manifest(directory, current.manifest_id)?;
    version.current_generation = current.generation;
    Ok(version)
}

pub(super) fn prepare_install(
    directory: &Path,
    current: &VersionSet,
    new_manifest_id: u64,
    state: ManifestState,
) -> Result<VersionSet> {
    let ManifestState {
        durable_sequence,
        tombstone_gc_sequence,
        wal_id,
        wal_first_sequence,
        tables,
    } = state;
    if durable_sequence < current.durable_sequence {
        return Err(corruption(
            24,
            "manifest install would move the durable sequence backward",
        ));
    }
    if tombstone_gc_sequence < current.tombstone_gc_sequence {
        return Err(corruption(
            64,
            "manifest install would move the tombstone GC sequence backward",
        ));
    }
    let generation = current
        .current_generation
        .checked_add(1)
        .ok_or_else(|| corruption(0, "CURRENT generation exhausted"))?;
    let table_id_high_watermark = tables
        .iter()
        .fold(current.table_id_high_watermark, |high, descriptor| {
            high.max(descriptor.table_id)
        });
    validate_version_set(
        durable_sequence,
        tombstone_gc_sequence,
        table_id_high_watermark,
        wal_id,
        wal_first_sequence,
        &tables,
    )?;
    let next = VersionSet {
        current_generation: generation,
        manifest_id: new_manifest_id,
        durable_sequence,
        tombstone_gc_sequence,
        table_id_high_watermark,
        wal_id,
        wal_first_sequence,
        tables,
    };
    write_manifest_new(directory, &next)?;
    Ok(next)
}

pub(super) fn publish_prepared(directory: &Path, prepared: &VersionSet) -> Result<()> {
    write_current_slot(directory, prepared.current_generation, prepared.manifest_id)
}

pub(super) fn install(
    directory: &Path,
    current: &VersionSet,
    new_manifest_id: u64,
    state: ManifestState,
) -> Result<VersionSet> {
    let next = prepare_install(directory, current, new_manifest_id, state)?;
    publish_prepared(directory, &next)?;
    Ok(next)
}

/// Publishes the same immutable manifest into the other CURRENT mirror at generation + 1.
///
/// WAL reclamation uses this after a WAL-reference-changing manifest publication so that both valid
/// CURRENT slots depend on the new WAL before the old segment is removed.
pub(super) fn mirror_current(directory: &Path, current: &VersionSet) -> Result<VersionSet> {
    let generation = current
        .current_generation
        .checked_add(1)
        .ok_or_else(|| corruption(0, "CURRENT generation exhausted while mirroring"))?;
    write_current_slot(directory, generation, current.manifest_id)?;
    let mut mirrored = current.clone();
    mirrored.current_generation = generation;
    Ok(mirrored)
}

fn write_current_slot(directory: &Path, generation: u64, manifest_id: u64) -> Result<()> {
    let slot_id = usize::try_from(generation % 2).expect("modulo two fits usize");
    let slot = encode_current_slot(slot_id, generation, manifest_id);
    let mut current_file = OpenOptions::new()
        .read(true)
        .write(true)
        .open(directory.join(CURRENT_FILE_NAME))?;
    current_file.seek(SeekFrom::Start(
        u64::try_from(slot_id * CURRENT_SLOT_BYTES).expect("CURRENT offset fits u64"),
    ))?;
    current_file.write_all(&slot)?;
    current_file.sync_data()?;
    Ok(())
}

fn read_current(path: &Path) -> Result<CurrentSlot> {
    let mut bytes = Vec::new();
    File::open(path)?.read_to_end(&mut bytes)?;
    if bytes.len() != CURRENT_FILE_BYTES {
        return Err(corruption(
            0,
            format!(
                "CURRENT has {} bytes; expected {CURRENT_FILE_BYTES}",
                bytes.len()
            ),
        ));
    }
    let first = parse_current_slot(&bytes[..CURRENT_SLOT_BYTES], 0).ok();
    let second = parse_current_slot(&bytes[CURRENT_SLOT_BYTES..], 1).ok();
    match (first, second) {
        (None, None) => Err(corruption(0, "both CURRENT slots are invalid")),
        (Some(slot), None) | (None, Some(slot)) => Ok(slot),
        (Some(left), Some(right)) => {
            if left.generation == right.generation {
                if left.manifest_id != right.manifest_id {
                    return Err(corruption(
                        0,
                        "equal-generation CURRENT slots disagree on manifest id",
                    ));
                }
                Ok(left)
            } else {
                let (older, newer) = if left.generation < right.generation {
                    (left, right)
                } else {
                    (right, left)
                };
                if newer.generation - older.generation != 1 {
                    return Err(corruption(
                        0,
                        "CURRENT slot generations differ by more than one",
                    ));
                }
                Ok(newer)
            }
        }
    }
}

fn encode_current_slot(
    slot_id: usize,
    generation: u64,
    manifest_id: u64,
) -> [u8; CURRENT_SLOT_BYTES] {
    let mut bytes = [0_u8; CURRENT_SLOT_BYTES];
    bytes[0..8].copy_from_slice(&CURRENT_MAGIC);
    bytes[8..10].copy_from_slice(&CURRENT_FORMAT_VERSION.to_le_bytes());
    bytes[10..12].copy_from_slice(&(slot_id as u16).to_le_bytes());
    bytes[16..24].copy_from_slice(&generation.to_le_bytes());
    bytes[24..32].copy_from_slice(&manifest_id.to_le_bytes());
    let crc = crc32fast::hash(&bytes[..CURRENT_SLOT_BYTES - 4]);
    bytes[CURRENT_SLOT_BYTES - 4..].copy_from_slice(&crc.to_le_bytes());
    bytes
}

fn parse_current_slot(bytes: &[u8], physical_slot: usize) -> Result<CurrentSlot> {
    let base = u64::try_from(physical_slot * CURRENT_SLOT_BYTES).expect("CURRENT offset fits u64");
    if bytes.len() != CURRENT_SLOT_BYTES || bytes[0..8] != CURRENT_MAGIC {
        return Err(corruption(base, "invalid CURRENT slot magic/length"));
    }
    let version = u16::from_le_bytes(bytes[8..10].try_into().expect("fixed slice"));
    if version != CURRENT_FORMAT_VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "LSM CURRENT",
            found: u64::from(version),
            supported: u64::from(CURRENT_FORMAT_VERSION),
        });
    }
    let slot_id = u16::from_le_bytes(bytes[10..12].try_into().expect("fixed slice"));
    if usize::from(slot_id) != physical_slot {
        return Err(corruption(
            base + 10,
            "CURRENT slot id does not match physical slot",
        ));
    }
    if bytes[12..16] != [0; 4]
        || bytes[32..CURRENT_SLOT_BYTES - 4]
            .iter()
            .any(|byte| *byte != 0)
    {
        return Err(corruption(base + 12, "CURRENT reserved bytes are nonzero"));
    }
    let expected = u32::from_le_bytes(
        bytes[CURRENT_SLOT_BYTES - 4..]
            .try_into()
            .expect("fixed slice"),
    );
    if crc32fast::hash(&bytes[..CURRENT_SLOT_BYTES - 4]) != expected {
        return Err(corruption(
            base + CURRENT_SLOT_BYTES as u64 - 4,
            "CURRENT checksum mismatch",
        ));
    }
    let manifest_id = u64::from_le_bytes(bytes[24..32].try_into().expect("fixed slice"));
    if manifest_id == 0 {
        return Err(corruption(base + 24, "CURRENT manifest id must be nonzero"));
    }
    Ok(CurrentSlot {
        generation: u64::from_le_bytes(bytes[16..24].try_into().expect("fixed slice")),
        manifest_id,
    })
}

fn write_manifest_new(directory: &Path, version: &VersionSet) -> Result<()> {
    validate_version_set(
        version.durable_sequence,
        version.tombstone_gc_sequence,
        version.table_id_high_watermark,
        version.wal_id,
        version.wal_first_sequence,
        &version.tables,
    )?;
    let mut body = Vec::new();
    for descriptor in &version.tables {
        body.extend_from_slice(&encode_descriptor(descriptor)?);
    }
    let body_len = u64::try_from(body.len())
        .map_err(|_| corruption(0, "manifest body length does not fit u64"))?;
    let mut bytes = vec![0_u8; MANIFEST_HEADER_LEN];
    bytes.extend_from_slice(&body);

    bytes[0..8].copy_from_slice(&MANIFEST_MAGIC);
    bytes[8..10].copy_from_slice(&MANIFEST_FORMAT_VERSION.to_le_bytes());
    bytes[10..12].copy_from_slice(&(MANIFEST_HEADER_LEN as u16).to_le_bytes());
    bytes[16..24].copy_from_slice(&version.manifest_id.to_le_bytes());
    bytes[24..32].copy_from_slice(&version.durable_sequence.to_le_bytes());
    bytes[32..40].copy_from_slice(
        &u64::try_from(version.tables.len())
            .map_err(|_| corruption(0, "manifest table count does not fit u64"))?
            .to_le_bytes(),
    );
    bytes[40..48].copy_from_slice(&body_len.to_le_bytes());
    bytes[48..56].copy_from_slice(&version.wal_id.to_le_bytes());
    bytes[56..64].copy_from_slice(&version.wal_first_sequence.to_le_bytes());
    bytes[64..72].copy_from_slice(&version.tombstone_gc_sequence.to_le_bytes());
    bytes[72..80].copy_from_slice(&version.table_id_high_watermark.to_le_bytes());
    let header_crc = crc32fast::hash(&bytes[..84]);
    bytes[84..88].copy_from_slice(&header_crc.to_le_bytes());
    let file_crc = crc32fast::hash(&bytes);
    bytes.extend_from_slice(&file_crc.to_le_bytes());

    let path = directory.join(manifest_file_name(version.manifest_id));
    let mut file = OpenOptions::new().write(true).create_new(true).open(path)?;
    file.write_all(&bytes)?;
    file.sync_all()?;
    Ok(())
}

fn read_manifest(directory: &Path, manifest_id: u64) -> Result<VersionSet> {
    let path = directory.join(manifest_file_name(manifest_id));
    let bytes = fs::read(&path)?;
    if bytes.len() < MANIFEST_HEADER_LEN_V1 + 4 {
        return Err(corruption(0, "truncated manifest"));
    }
    if bytes[0..8] != MANIFEST_MAGIC {
        return Err(corruption(0, "invalid manifest magic"));
    }
    let format_version = u16::from_le_bytes(bytes[8..10].try_into().expect("fixed slice"));
    let (
        header_len,
        wal_id,
        wal_first_sequence,
        tombstone_gc_sequence,
        encoded_table_id_high_watermark,
    ) = match format_version {
        MANIFEST_FORMAT_VERSION_V1 => {
            if u16::from_le_bytes(bytes[10..12].try_into().expect("fixed slice")) as usize
                != MANIFEST_HEADER_LEN_V1
                || bytes[12..16] != [0; 4]
                || bytes[48..60].iter().any(|byte| *byte != 0)
            {
                return Err(corruption(10, "invalid v1 manifest header fields"));
            }
            let expected_header_crc =
                u32::from_le_bytes(bytes[60..64].try_into().expect("fixed slice"));
            if crc32fast::hash(&bytes[..60]) != expected_header_crc {
                return Err(corruption(60, "manifest header checksum mismatch"));
            }
            (MANIFEST_HEADER_LEN_V1, 1, 1, 0, None)
        }
        MANIFEST_FORMAT_VERSION_V2 | MANIFEST_FORMAT_VERSION_V3 => {
            if bytes.len() < MANIFEST_HEADER_LEN_V4 + 4
                || u16::from_le_bytes(bytes[10..12].try_into().expect("fixed slice")) as usize
                    != MANIFEST_HEADER_LEN_V4
                || bytes[12..16] != [0; 4]
                || bytes[64..76].iter().any(|byte| *byte != 0)
            {
                return Err(corruption(10, "invalid v2/v3 manifest header fields"));
            }
            let expected_header_crc =
                u32::from_le_bytes(bytes[76..80].try_into().expect("fixed slice"));
            if crc32fast::hash(&bytes[..76]) != expected_header_crc {
                return Err(corruption(76, "manifest header checksum mismatch"));
            }
            (
                MANIFEST_HEADER_LEN_V4,
                u64::from_le_bytes(bytes[48..56].try_into().expect("fixed slice")),
                u64::from_le_bytes(bytes[56..64].try_into().expect("fixed slice")),
                0,
                None,
            )
        }
        MANIFEST_FORMAT_VERSION_V4 => {
            if bytes.len() < MANIFEST_HEADER_LEN_V4 + 4
                || u16::from_le_bytes(bytes[10..12].try_into().expect("fixed slice")) as usize
                    != MANIFEST_HEADER_LEN_V4
                || bytes[12..16] != [0; 4]
                || bytes[72..76].iter().any(|byte| *byte != 0)
            {
                return Err(corruption(10, "invalid v4 manifest header fields"));
            }
            let expected_header_crc =
                u32::from_le_bytes(bytes[76..80].try_into().expect("fixed slice"));
            if crc32fast::hash(&bytes[..76]) != expected_header_crc {
                return Err(corruption(76, "manifest header checksum mismatch"));
            }
            (
                MANIFEST_HEADER_LEN_V4,
                u64::from_le_bytes(bytes[48..56].try_into().expect("fixed slice")),
                u64::from_le_bytes(bytes[56..64].try_into().expect("fixed slice")),
                u64::from_le_bytes(bytes[64..72].try_into().expect("fixed slice")),
                None,
            )
        }
        MANIFEST_FORMAT_VERSION => {
            if bytes.len() < MANIFEST_HEADER_LEN + 4
                || u16::from_le_bytes(bytes[10..12].try_into().expect("fixed slice")) as usize
                    != MANIFEST_HEADER_LEN
                || bytes[12..16] != [0; 4]
                || bytes[80..84].iter().any(|byte| *byte != 0)
            {
                return Err(corruption(10, "invalid v5 manifest header fields"));
            }
            let expected_header_crc =
                u32::from_le_bytes(bytes[84..88].try_into().expect("fixed slice"));
            if crc32fast::hash(&bytes[..84]) != expected_header_crc {
                return Err(corruption(84, "manifest header checksum mismatch"));
            }
            (
                MANIFEST_HEADER_LEN,
                u64::from_le_bytes(bytes[48..56].try_into().expect("fixed slice")),
                u64::from_le_bytes(bytes[56..64].try_into().expect("fixed slice")),
                u64::from_le_bytes(bytes[64..72].try_into().expect("fixed slice")),
                Some(u64::from_le_bytes(
                    bytes[72..80].try_into().expect("fixed slice"),
                )),
            )
        }
        _ => {
            return Err(DbError::UnsupportedVersion {
                format: "LSM manifest",
                found: u64::from(format_version),
                supported: u64::from(MANIFEST_FORMAT_VERSION),
            });
        }
    };

    let encoded_manifest_id = u64::from_le_bytes(bytes[16..24].try_into().expect("fixed slice"));
    if encoded_manifest_id != manifest_id {
        return Err(corruption(
            16,
            "manifest id does not match filename/CURRENT",
        ));
    }
    let durable_sequence = u64::from_le_bytes(bytes[24..32].try_into().expect("fixed slice"));
    let table_count = usize::try_from(u64::from_le_bytes(
        bytes[32..40].try_into().expect("fixed slice"),
    ))
    .map_err(|_| corruption(32, "manifest table count does not fit usize"))?;
    let body_len = usize::try_from(u64::from_le_bytes(
        bytes[40..48].try_into().expect("fixed slice"),
    ))
    .map_err(|_| corruption(40, "manifest body length does not fit usize"))?;
    let expected_len = header_len
        .checked_add(body_len)
        .and_then(|len| len.checked_add(4))
        .ok_or_else(|| corruption(40, "manifest extent arithmetic overflowed usize"))?;
    if expected_len != bytes.len() {
        return Err(corruption(
            40,
            "manifest body length does not match physical file",
        ));
    }
    let crc_offset = bytes.len() - 4;
    let expected_file_crc =
        u32::from_le_bytes(bytes[crc_offset..].try_into().expect("fixed slice"));
    if crc32fast::hash(&bytes[..crc_offset]) != expected_file_crc {
        return Err(corruption(
            crc_offset as u64,
            "manifest file checksum mismatch",
        ));
    }

    let mut offset = header_len;
    let body_end = crc_offset;
    let mut tables = Vec::with_capacity(table_count);
    for _ in 0..table_count {
        let (descriptor, next) = decode_descriptor(&bytes, offset, body_end, format_version)?;
        tables.push(descriptor);
        offset = next;
    }
    if offset != body_end {
        return Err(corruption(
            offset as u64,
            "manifest contains unexplained descriptor bytes",
        ));
    }
    let active_table_id_high_watermark = tables
        .iter()
        .map(|descriptor| descriptor.table_id)
        .max()
        .unwrap_or(0);
    let table_id_high_watermark = encoded_table_id_high_watermark.unwrap_or_else(|| {
        if tables.is_empty() && durable_sequence > 0 {
            durable_sequence.max(active_table_id_high_watermark)
        } else {
            active_table_id_high_watermark
        }
    });
    validate_version_set(
        durable_sequence,
        tombstone_gc_sequence,
        table_id_high_watermark,
        wal_id,
        wal_first_sequence,
        &tables,
    )?;
    Ok(VersionSet {
        current_generation: 0,
        manifest_id,
        durable_sequence,
        tombstone_gc_sequence,
        table_id_high_watermark,
        wal_id,
        wal_first_sequence,
        tables,
    })
}

fn encode_descriptor(descriptor: &SstableDescriptor) -> Result<Vec<u8>> {
    if descriptor.level > 1 {
        return Err(corruption(
            0,
            "manifest SSTable level exceeds implemented L1 policy",
        ));
    }
    if descriptor.smallest_key.len() > MAX_KEY_BYTES || descriptor.largest_key.len() > MAX_KEY_BYTES
    {
        return Err(corruption(
            0,
            "manifest SSTable key bound exceeds common key limit",
        ));
    }
    let smallest_len = u32::try_from(descriptor.smallest_key.len())
        .map_err(|_| corruption(0, "manifest smallest-key length does not fit u32"))?;
    let largest_len = u32::try_from(descriptor.largest_key.len())
        .map_err(|_| corruption(0, "manifest largest-key length does not fit u32"))?;
    let capacity = DESCRIPTOR_PREFIX_LEN
        .checked_add(descriptor.smallest_key.len())
        .and_then(|len| len.checked_add(descriptor.largest_key.len()))
        .and_then(|len| len.checked_add(4))
        .ok_or_else(|| corruption(0, "manifest descriptor size overflowed usize"))?;
    let mut bytes = Vec::with_capacity(capacity);
    bytes.extend_from_slice(&descriptor.table_id.to_le_bytes());
    bytes.extend_from_slice(&descriptor.file_bytes.to_le_bytes());
    bytes.extend_from_slice(&descriptor.entry_count.to_le_bytes());
    bytes.extend_from_slice(&descriptor.durable_sequence.to_le_bytes());
    bytes.extend_from_slice(&descriptor.level.to_le_bytes());
    bytes.extend_from_slice(&0_u32.to_le_bytes());
    bytes.extend_from_slice(&smallest_len.to_le_bytes());
    bytes.extend_from_slice(&largest_len.to_le_bytes());
    bytes.extend_from_slice(&descriptor.smallest_key);
    bytes.extend_from_slice(&descriptor.largest_key);
    let crc = crc32fast::hash(&bytes);
    bytes.extend_from_slice(&crc.to_le_bytes());
    Ok(bytes)
}

fn decode_descriptor(
    bytes: &[u8],
    offset: usize,
    limit: usize,
    manifest_format_version: u16,
) -> Result<(SstableDescriptor, usize)> {
    let legacy = manifest_format_version <= MANIFEST_FORMAT_VERSION_V2;
    let prefix_len = if legacy {
        LEGACY_DESCRIPTOR_PREFIX_LEN
    } else {
        DESCRIPTOR_PREFIX_LEN
    };
    let prefix_end = offset
        .checked_add(prefix_len)
        .ok_or_else(|| corruption(offset as u64, "manifest descriptor extent overflowed"))?;
    if prefix_end > limit {
        return Err(corruption(
            offset as u64,
            "truncated manifest descriptor prefix",
        ));
    }
    let prefix = &bytes[offset..prefix_end];
    let (level, smallest_len_offset, largest_len_offset) = if legacy {
        (0_u32, 32_usize, 36_usize)
    } else {
        if u32::from_le_bytes(prefix[36..40].try_into().expect("fixed slice")) != 0 {
            return Err(corruption(
                offset as u64 + 36,
                "manifest descriptor reserved field is nonzero",
            ));
        }
        (
            u32::from_le_bytes(prefix[32..36].try_into().expect("fixed slice")),
            40_usize,
            44_usize,
        )
    };
    let smallest_len = usize::try_from(u32::from_le_bytes(
        prefix[smallest_len_offset..smallest_len_offset + 4]
            .try_into()
            .expect("fixed slice"),
    ))
    .map_err(|_| {
        corruption(
            offset as u64 + smallest_len_offset as u64,
            "smallest-key length does not fit usize",
        )
    })?;
    let largest_len = usize::try_from(u32::from_le_bytes(
        prefix[largest_len_offset..largest_len_offset + 4]
            .try_into()
            .expect("fixed slice"),
    ))
    .map_err(|_| {
        corruption(
            offset as u64 + largest_len_offset as u64,
            "largest-key length does not fit usize",
        )
    })?;
    if level > 1 {
        return Err(corruption(
            offset as u64 + 32,
            "manifest SSTable level exceeds implemented L1 policy",
        ));
    }
    if smallest_len > MAX_KEY_BYTES || largest_len > MAX_KEY_BYTES {
        return Err(corruption(
            offset as u64 + smallest_len_offset as u64,
            "manifest key bound exceeds common key limit",
        ));
    }
    let smallest_end = prefix_end
        .checked_add(smallest_len)
        .ok_or_else(|| corruption(offset as u64, "manifest smallest-key extent overflowed"))?;
    let largest_end = smallest_end
        .checked_add(largest_len)
        .ok_or_else(|| corruption(offset as u64, "manifest largest-key extent overflowed"))?;
    let descriptor_end = largest_end.checked_add(4).ok_or_else(|| {
        corruption(
            offset as u64,
            "manifest descriptor checksum extent overflowed",
        )
    })?;
    if descriptor_end > limit {
        return Err(corruption(offset as u64, "truncated manifest descriptor"));
    }
    let expected_crc = u32::from_le_bytes(
        bytes[largest_end..descriptor_end]
            .try_into()
            .expect("fixed slice"),
    );
    if crc32fast::hash(&bytes[offset..largest_end]) != expected_crc {
        return Err(corruption(
            largest_end as u64,
            "manifest descriptor checksum mismatch",
        ));
    }
    let descriptor = SstableDescriptor {
        table_id: u64::from_le_bytes(prefix[0..8].try_into().expect("fixed slice")),
        level,
        file_bytes: u64::from_le_bytes(prefix[8..16].try_into().expect("fixed slice")),
        entry_count: u64::from_le_bytes(prefix[16..24].try_into().expect("fixed slice")),
        durable_sequence: u64::from_le_bytes(prefix[24..32].try_into().expect("fixed slice")),
        smallest_key: bytes[prefix_end..smallest_end].to_vec(),
        largest_key: bytes[smallest_end..largest_end].to_vec(),
    };
    if descriptor.table_id == 0
        || descriptor.file_bytes == 0
        || descriptor.entry_count == 0
        || descriptor.durable_sequence == 0
        || descriptor.smallest_key > descriptor.largest_key
    {
        return Err(corruption(
            offset as u64,
            "invalid manifest SSTable descriptor values",
        ));
    }
    Ok((descriptor, descriptor_end))
}

fn validate_version_set(
    durable_sequence: u64,
    tombstone_gc_sequence: u64,
    table_id_high_watermark: u64,
    wal_id: u64,
    wal_first_sequence: u64,
    tables: &[SstableDescriptor],
) -> Result<()> {
    if tombstone_gc_sequence > durable_sequence {
        return Err(corruption(
            64,
            format!(
                "manifest tombstone GC sequence {tombstone_gc_sequence} exceeds durable sequence {durable_sequence}"
            ),
        ));
    }
    if durable_sequence > 0 && table_id_high_watermark == 0 {
        return Err(corruption(
            72,
            "nonempty durable history requires a nonzero SSTable id high watermark",
        ));
    }
    if wal_id == 0 || wal_first_sequence == 0 {
        return Err(corruption(
            0,
            "manifest WAL id and first sequence must both be nonzero",
        ));
    }
    if wal_first_sequence > durable_sequence.saturating_add(1) {
        return Err(corruption(
            0,
            format!(
                "manifest WAL first sequence {wal_first_sequence} is beyond durable sequence {durable_sequence} + 1"
            ),
        ));
    }

    let mut previous_table_id = 0_u64;
    let mut previous_durable = 0_u64;
    let mut level1_tables = 0_usize;
    for descriptor in tables {
        if descriptor.level > 1 {
            return Err(corruption(
                0,
                "manifest SSTable level exceeds implemented L1 policy",
            ));
        }
        if descriptor.level == 1 {
            level1_tables = level1_tables
                .checked_add(1)
                .ok_or_else(|| corruption(0, "manifest L1 table count overflowed usize"))?;
            if level1_tables > 1 {
                return Err(corruption(
                    0,
                    "current L1 policy permits exactly one non-overlapping run",
                ));
            }
        }
        if descriptor.table_id <= previous_table_id {
            return Err(corruption(
                0,
                "manifest table ids are not strictly increasing",
            ));
        }
        if descriptor.durable_sequence <= previous_durable {
            return Err(corruption(
                0,
                "manifest SSTable durable sequences are not strictly increasing",
            ));
        }
        previous_table_id = descriptor.table_id;
        previous_durable = descriptor.durable_sequence;
    }
    if previous_table_id > table_id_high_watermark {
        return Err(corruption(
            72,
            format!(
                "manifest SSTable id high watermark {table_id_high_watermark} is below active table id {previous_table_id}"
            ),
        ));
    }
    let expected = tables.last().map_or(0, |table| table.durable_sequence);
    if tables.is_empty() {
        if durable_sequence != tombstone_gc_sequence {
            return Err(corruption(
                24,
                format!(
                    "table-less manifest durable sequence {durable_sequence} is not fully covered by tombstone GC sequence {tombstone_gc_sequence}"
                ),
            ));
        }
    } else if durable_sequence != expected {
        return Err(corruption(
            0,
            format!(
                "manifest durable sequence {durable_sequence} does not equal latest table watermark {expected}"
            ),
        ));
    }
    Ok(())
}

fn corruption(offset: u64, reason: impl Into<String>) -> DbError {
    DbError::Corruption {
        offset,
        reason: reason.into(),
    }
}
