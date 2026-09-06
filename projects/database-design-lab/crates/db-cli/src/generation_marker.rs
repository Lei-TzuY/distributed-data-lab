use serde::Serialize;
use thiserror::Error;

pub const COMMIT_MARKER_MAGIC: [u8; 8] = *b"DBLGCMT\0";
pub const COMMIT_MARKER_VERSION: u16 = 2;
pub const COMMIT_MARKER_LEN: usize = 64;
pub const APPEND_LOG_FORMAT_VERSION: u16 = 1;
pub const MIN_COMMITTED_PREFIX_BYTES: u64 = 16;

const CRC32_POLYNOMIAL: u32 = 0xedb8_8320;
const CRC32_TABLE: [u32; 256] = build_crc32_table();

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct CommittedPrefix {
    pub bytes: u64,
    pub crc32: u32,
    pub record_count: u64,
    pub next_sequence: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CommitMarker {
    pub generation_id: u64,
    pub log_format_version: u16,
    pub committed_prefix: CommittedPrefix,
}

#[derive(Debug, Error, Clone, PartialEq, Eq)]
pub enum CommitMarkerError {
    #[error("marker generation id must be greater than zero")]
    ZeroGeneration,
    #[error("marker has {found} bytes, expected {expected}")]
    Length { found: usize, expected: usize },
    #[error("magic mismatch")]
    Magic,
    #[error("checksum mismatch: expected {expected:08x}, computed {computed:08x}")]
    Checksum { expected: u32, computed: u32 },
    #[error("unsupported marker version {found}; expected {expected}")]
    Version { found: u16, expected: u16 },
    #[error("invalid marker header length {0}")]
    HeaderLength(u16),
    #[error("marker generation {found} disagrees with filename generation {expected}")]
    GenerationMismatch { found: u64, expected: u64 },
    #[error("unsupported append-log format version {found}; expected {expected}")]
    LogFormat { found: u16, expected: u16 },
    #[error("marker reserved field is nonzero: {0:#06x}")]
    Reserved(u16),
    #[error("marker committed prefix has {found} bytes, need at least {minimum}")]
    PrefixTooShort { found: u64, minimum: u64 },
    #[error(
        "marker committed prefix record count {record_count} cannot advance to a next sequence"
    )]
    PrefixSequenceOverflow { record_count: u64 },
    #[error(
        "marker committed prefix record_count={record_count} requires next_sequence={expected}, found {next_sequence}"
    )]
    PrefixSequence {
        record_count: u64,
        next_sequence: u64,
        expected: u64,
    },
    #[error("unsupported marker flags {0:#010x}")]
    Flags(u32),
    #[error("marker secondary reserved field is nonzero: {0:#010x}")]
    Reserved2(u32),
}

pub fn encode_commit_marker(
    generation_id: u64,
    committed_prefix: CommittedPrefix,
) -> Result<[u8; COMMIT_MARKER_LEN], CommitMarkerError> {
    validate_generation(generation_id)?;
    validate_committed_prefix(committed_prefix)?;

    let mut bytes = [0_u8; COMMIT_MARKER_LEN];
    bytes[..8].copy_from_slice(&COMMIT_MARKER_MAGIC);
    bytes[8..10].copy_from_slice(&COMMIT_MARKER_VERSION.to_le_bytes());
    bytes[10..12].copy_from_slice(&(COMMIT_MARKER_LEN as u16).to_le_bytes());
    bytes[12..20].copy_from_slice(&generation_id.to_le_bytes());
    bytes[20..22].copy_from_slice(&APPEND_LOG_FORMAT_VERSION.to_le_bytes());
    bytes[22..24].copy_from_slice(&0_u16.to_le_bytes());
    bytes[24..32].copy_from_slice(&committed_prefix.bytes.to_le_bytes());
    bytes[32..36].copy_from_slice(&committed_prefix.crc32.to_le_bytes());
    bytes[36..40].copy_from_slice(&0_u32.to_le_bytes());
    bytes[40..48].copy_from_slice(&committed_prefix.record_count.to_le_bytes());
    bytes[48..56].copy_from_slice(&committed_prefix.next_sequence.to_le_bytes());
    bytes[56..60].copy_from_slice(&0_u32.to_le_bytes());
    let checksum = crc32_ieee(&bytes[..60]);
    bytes[60..64].copy_from_slice(&checksum.to_le_bytes());
    Ok(bytes)
}

pub fn decode_commit_marker(
    bytes: &[u8],
    filename_generation: u64,
) -> Result<CommitMarker, CommitMarkerError> {
    validate_generation(filename_generation)?;
    if bytes.len() != COMMIT_MARKER_LEN {
        return Err(CommitMarkerError::Length {
            found: bytes.len(),
            expected: COMMIT_MARKER_LEN,
        });
    }
    if bytes[..8] != COMMIT_MARKER_MAGIC {
        return Err(CommitMarkerError::Magic);
    }

    let expected_crc = read_u32(&bytes[60..64]);
    let computed_crc = crc32_ieee(&bytes[..60]);
    if expected_crc != computed_crc {
        return Err(CommitMarkerError::Checksum {
            expected: expected_crc,
            computed: computed_crc,
        });
    }

    let version = read_u16(&bytes[8..10]);
    if version != COMMIT_MARKER_VERSION {
        return Err(CommitMarkerError::Version {
            found: version,
            expected: COMMIT_MARKER_VERSION,
        });
    }
    let header_len = read_u16(&bytes[10..12]);
    if usize::from(header_len) != COMMIT_MARKER_LEN {
        return Err(CommitMarkerError::HeaderLength(header_len));
    }
    let generation_id = read_u64(&bytes[12..20]);
    if generation_id == 0 || generation_id != filename_generation {
        return Err(CommitMarkerError::GenerationMismatch {
            found: generation_id,
            expected: filename_generation,
        });
    }
    let log_format_version = read_u16(&bytes[20..22]);
    if log_format_version != APPEND_LOG_FORMAT_VERSION {
        return Err(CommitMarkerError::LogFormat {
            found: log_format_version,
            expected: APPEND_LOG_FORMAT_VERSION,
        });
    }
    let reserved = read_u16(&bytes[22..24]);
    if reserved != 0 {
        return Err(CommitMarkerError::Reserved(reserved));
    }

    let committed_prefix = CommittedPrefix {
        bytes: read_u64(&bytes[24..32]),
        crc32: read_u32(&bytes[32..36]),
        record_count: read_u64(&bytes[40..48]),
        next_sequence: read_u64(&bytes[48..56]),
    };
    validate_committed_prefix(committed_prefix)?;

    let flags = read_u32(&bytes[36..40]);
    if flags != 0 {
        return Err(CommitMarkerError::Flags(flags));
    }
    let reserved2 = read_u32(&bytes[56..60]);
    if reserved2 != 0 {
        return Err(CommitMarkerError::Reserved2(reserved2));
    }

    Ok(CommitMarker {
        generation_id,
        log_format_version,
        committed_prefix,
    })
}

#[derive(Debug, Clone)]
pub struct Crc32Ieee {
    state: u32,
}

impl Default for Crc32Ieee {
    fn default() -> Self {
        Self::new()
    }
}

impl Crc32Ieee {
    #[must_use]
    pub const fn new() -> Self {
        Self { state: u32::MAX }
    }

    pub fn update(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            let index = ((self.state ^ u32::from(byte)) & 0xff) as usize;
            self.state = (self.state >> 8) ^ CRC32_TABLE[index];
        }
    }

    #[must_use]
    pub const fn finalize(self) -> u32 {
        !self.state
    }
}

#[must_use]
pub fn crc32_ieee(bytes: &[u8]) -> u32 {
    let mut hasher = Crc32Ieee::new();
    hasher.update(bytes);
    hasher.finalize()
}

fn validate_generation(generation_id: u64) -> Result<(), CommitMarkerError> {
    if generation_id == 0 {
        Err(CommitMarkerError::ZeroGeneration)
    } else {
        Ok(())
    }
}

fn validate_committed_prefix(prefix: CommittedPrefix) -> Result<(), CommitMarkerError> {
    if prefix.bytes < MIN_COMMITTED_PREFIX_BYTES {
        return Err(CommitMarkerError::PrefixTooShort {
            found: prefix.bytes,
            minimum: MIN_COMMITTED_PREFIX_BYTES,
        });
    }
    let expected =
        prefix
            .record_count
            .checked_add(1)
            .ok_or(CommitMarkerError::PrefixSequenceOverflow {
                record_count: prefix.record_count,
            })?;
    if prefix.next_sequence != expected {
        return Err(CommitMarkerError::PrefixSequence {
            record_count: prefix.record_count,
            next_sequence: prefix.next_sequence,
            expected,
        });
    }
    Ok(())
}

const fn build_crc32_table() -> [u32; 256] {
    let mut table = [0_u32; 256];
    let mut index = 0_usize;
    while index < table.len() {
        let mut crc = index as u32;
        let mut bit = 0_u8;
        while bit < 8 {
            crc = if crc & 1 == 1 {
                (crc >> 1) ^ CRC32_POLYNOMIAL
            } else {
                crc >> 1
            };
            bit += 1;
        }
        table[index] = crc;
        index += 1;
    }
    table
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

#[cfg(test)]
mod tests {
    use super::*;

    fn proof() -> CommittedPrefix {
        CommittedPrefix {
            bytes: 123,
            crc32: 0x1020_3040,
            record_count: 7,
            next_sequence: 8,
        }
    }

    #[test]
    fn crc32_matches_standard_check_vector() {
        assert_eq!(crc32_ieee(b"123456789"), 0xcbf4_3926);
    }

    #[test]
    fn streaming_crc_matches_one_shot_crc() {
        let mut hasher = Crc32Ieee::new();
        hasher.update(b"1234");
        hasher.update(b"56789");
        assert_eq!(hasher.finalize(), crc32_ieee(b"123456789"));
    }

    #[test]
    fn marker_round_trip_binds_generation_and_prefix() {
        let encoded = encode_commit_marker(42, proof()).expect("encode marker");
        let decoded = decode_commit_marker(&encoded, 42).expect("decode marker");
        assert_eq!(decoded.generation_id, 42);
        assert_eq!(decoded.log_format_version, APPEND_LOG_FORMAT_VERSION);
        assert_eq!(decoded.committed_prefix, proof());
        assert!(decode_commit_marker(&encoded, 41).is_err());
    }

    #[test]
    fn marker_corruption_is_rejected() {
        let mut encoded = encode_commit_marker(7, proof()).expect("encode marker");
        encoded[24] ^= 0x80;
        assert!(matches!(
            decode_commit_marker(&encoded, 7),
            Err(CommitMarkerError::Checksum { .. })
        ));
    }

    #[test]
    fn structurally_inconsistent_prefix_is_rejected() {
        let mut invalid = proof();
        invalid.next_sequence = 99;
        assert!(matches!(
            encode_commit_marker(7, invalid),
            Err(CommitMarkerError::PrefixSequence { .. })
        ));
    }

    #[test]
    fn exhausted_record_count_is_rejected() {
        let mut invalid = proof();
        invalid.record_count = u64::MAX;
        invalid.next_sequence = u64::MAX;
        assert!(matches!(
            encode_commit_marker(7, invalid),
            Err(CommitMarkerError::PrefixSequenceOverflow { .. })
        ));
    }

    #[test]
    fn legacy_v1_marker_length_is_not_accepted_as_v2() {
        let legacy = [0_u8; 32];
        assert!(matches!(
            decode_commit_marker(&legacy, 1),
            Err(CommitMarkerError::Length {
                found: 32,
                expected: COMMIT_MARKER_LEN
            })
        ));
    }
}
