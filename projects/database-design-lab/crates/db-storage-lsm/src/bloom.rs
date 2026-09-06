use db_core::{DbError, Result};

pub(super) const BITS_PER_KEY: u64 = 10;
pub(super) const HASH_PROBES: u8 = 7;
pub(super) const FILTER_HEADER_LEN: usize = 40;
const FILTER_TRAILER_LEN: usize = 4;
const FILTER_MAGIC: [u8; 8] = *b"DBLSMBLM";
const FILTER_VERSION: u16 = 1;
const HASH_ALGORITHM_FNV64_DOUBLE: u8 = 1;
const MIN_FILTER_BITS: u64 = 64;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;
const HASH_SEED_ONE: u64 = 0x9e37_79b9_7f4a_7c15;
const HASH_SEED_TWO: u64 = 0xd1b5_4a32_d192_ed03;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct BloomFilter {
    bit_count: u64,
    key_count: u64,
    bits: Vec<u8>,
}

impl BloomFilter {
    pub(super) fn build<'a>(
        keys: impl IntoIterator<Item = &'a [u8]>,
        key_count: usize,
    ) -> Result<Self> {
        let key_count = u64::try_from(key_count)
            .map_err(|_| corruption(0, "Bloom key count does not fit u64"))?;
        if key_count == 0 {
            return Err(corruption(
                0,
                "Bloom filter cannot describe an empty SSTable",
            ));
        }
        let bit_count = canonical_bit_count(key_count)?;
        let byte_count = usize::try_from(bit_count / 8)
            .map_err(|_| corruption(0, "Bloom bit extent does not fit usize"))?;
        let mut filter = Self {
            bit_count,
            key_count,
            bits: vec![0; byte_count],
        };
        let mut observed = 0_u64;
        for key in keys {
            filter.insert(key);
            observed = observed
                .checked_add(1)
                .ok_or_else(|| corruption(0, "Bloom observed key count overflowed u64"))?;
        }
        if observed != key_count {
            return Err(corruption(
                0,
                format!("Bloom received {observed} keys but expected {key_count}"),
            ));
        }
        Ok(filter)
    }

    pub(super) fn decode(bytes: &[u8], offset: u64, expected_key_count: u64) -> Result<Self> {
        let minimum = FILTER_HEADER_LEN
            .checked_add(FILTER_TRAILER_LEN)
            .ok_or_else(|| corruption(offset, "Bloom minimum extent overflowed usize"))?;
        if bytes.len() < minimum {
            return Err(corruption(offset, "truncated Bloom filter section"));
        }
        if bytes[0..8] != FILTER_MAGIC {
            return Err(corruption(offset, "Bloom filter magic mismatch"));
        }
        let version = read_u16(&bytes[8..10]);
        if version != FILTER_VERSION {
            return Err(DbError::UnsupportedVersion {
                format: "LSM Bloom filter",
                found: u64::from(version),
                supported: u64::from(FILTER_VERSION),
            });
        }
        if usize::from(read_u16(&bytes[10..12])) != FILTER_HEADER_LEN {
            return Err(corruption(
                offset + 10,
                "Bloom header length is not canonical",
            ));
        }
        if bytes[12] != HASH_ALGORITHM_FNV64_DOUBLE {
            return Err(corruption(offset + 12, "unknown Bloom hash algorithm"));
        }
        if bytes[13] != HASH_PROBES {
            return Err(corruption(
                offset + 13,
                "Bloom probe count is not canonical",
            ));
        }
        if read_u16(&bytes[14..16]) != 0 {
            return Err(corruption(offset + 14, "Bloom flags are nonzero"));
        }
        let bit_count = read_u64(&bytes[16..24]);
        let key_count = read_u64(&bytes[24..32]);
        let payload_len = usize::try_from(read_u32(&bytes[32..36]))
            .map_err(|_| corruption(offset + 32, "Bloom payload length does not fit usize"))?;
        let expected_header_crc = read_u32(&bytes[36..40]);
        if crc32fast::hash(&bytes[..36]) != expected_header_crc {
            return Err(corruption(offset + 36, "Bloom header checksum mismatch"));
        }
        if key_count != expected_key_count {
            return Err(corruption(
                offset + 24,
                format!(
                    "Bloom key count {key_count} does not match SSTable entry count {expected_key_count}"
                ),
            ));
        }
        let canonical_bits = canonical_bit_count(key_count)?;
        if bit_count != canonical_bits || bit_count % 8 != 0 {
            return Err(corruption(offset + 16, "Bloom bit count is not canonical"));
        }
        let expected_payload_len = usize::try_from(bit_count / 8)
            .map_err(|_| corruption(offset + 16, "Bloom bit extent does not fit usize"))?;
        if payload_len != expected_payload_len {
            return Err(corruption(
                offset + 32,
                "Bloom payload length disagrees with bit count",
            ));
        }
        let payload_end = FILTER_HEADER_LEN
            .checked_add(payload_len)
            .ok_or_else(|| corruption(offset, "Bloom payload extent overflowed usize"))?;
        let expected_len = payload_end
            .checked_add(FILTER_TRAILER_LEN)
            .ok_or_else(|| corruption(offset, "Bloom section extent overflowed usize"))?;
        if bytes.len() != expected_len {
            return Err(corruption(
                offset,
                "Bloom section is not an exact canonical extent",
            ));
        }
        let expected_crc = read_u32(&bytes[payload_end..expected_len]);
        if crc32fast::hash(&bytes[..payload_end]) != expected_crc {
            let checksum_offset = u64::try_from(payload_end)
                .ok()
                .and_then(|delta| offset.checked_add(delta))
                .ok_or_else(|| corruption(offset, "Bloom checksum offset overflowed u64"))?;
            return Err(corruption(
                checksum_offset,
                "Bloom section checksum mismatch",
            ));
        }
        Ok(Self {
            bit_count,
            key_count,
            bits: bytes[FILTER_HEADER_LEN..payload_end].to_vec(),
        })
    }

    pub(super) fn encode(&self) -> Result<Vec<u8>> {
        let payload_len = u32::try_from(self.bits.len())
            .map_err(|_| corruption(0, "Bloom payload length does not fit u32"))?;
        let capacity = FILTER_HEADER_LEN
            .checked_add(self.bits.len())
            .and_then(|len| len.checked_add(FILTER_TRAILER_LEN))
            .ok_or_else(|| corruption(0, "Bloom encoded extent overflowed usize"))?;
        let mut encoded = vec![0_u8; FILTER_HEADER_LEN];
        encoded[0..8].copy_from_slice(&FILTER_MAGIC);
        encoded[8..10].copy_from_slice(&FILTER_VERSION.to_le_bytes());
        encoded[10..12].copy_from_slice(&(FILTER_HEADER_LEN as u16).to_le_bytes());
        encoded[12] = HASH_ALGORITHM_FNV64_DOUBLE;
        encoded[13] = HASH_PROBES;
        encoded[16..24].copy_from_slice(&self.bit_count.to_le_bytes());
        encoded[24..32].copy_from_slice(&self.key_count.to_le_bytes());
        encoded[32..36].copy_from_slice(&payload_len.to_le_bytes());
        let header_crc = crc32fast::hash(&encoded[..36]);
        encoded[36..40].copy_from_slice(&header_crc.to_le_bytes());
        encoded.extend_from_slice(&self.bits);
        let crc = crc32fast::hash(&encoded);
        encoded.extend_from_slice(&crc.to_le_bytes());
        debug_assert_eq!(encoded.len(), capacity);
        Ok(encoded)
    }

    pub(super) fn may_contain(&self, key: &[u8]) -> bool {
        probe_positions(key, self.bit_count).all(|bit| {
            let byte = usize::try_from(bit / 8).expect("Bloom bit index fits allocated usize");
            let mask = 1_u8 << (bit % 8);
            self.bits[byte] & mask != 0
        })
    }

    #[cfg(test)]
    pub(super) const fn bit_count(&self) -> u64 {
        self.bit_count
    }

    #[cfg(test)]
    pub(super) const fn key_count(&self) -> u64 {
        self.key_count
    }

    fn insert(&mut self, key: &[u8]) {
        for bit in probe_positions(key, self.bit_count) {
            let byte = usize::try_from(bit / 8).expect("Bloom bit index fits allocated usize");
            let mask = 1_u8 << (bit % 8);
            self.bits[byte] |= mask;
        }
    }
}

fn canonical_bit_count(key_count: u64) -> Result<u64> {
    if key_count == 0 {
        return Err(corruption(0, "Bloom key count must be nonzero"));
    }
    let requested = key_count
        .checked_mul(BITS_PER_KEY)
        .ok_or_else(|| corruption(0, "Bloom bit count overflowed u64"))?
        .max(MIN_FILTER_BITS);
    requested
        .checked_add(7)
        .map(|bits| bits / 8 * 8)
        .ok_or_else(|| corruption(0, "Bloom byte rounding overflowed u64"))
}

fn probe_positions(key: &[u8], bit_count: u64) -> impl Iterator<Item = u64> + '_ {
    let first = hash64(key, HASH_SEED_ONE);
    let second = hash64(key, HASH_SEED_TWO) | 1;
    (0..HASH_PROBES)
        .map(move |probe| first.wrapping_add(u64::from(probe).wrapping_mul(second)) % bit_count)
}

fn hash64(bytes: &[u8], seed: u64) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325_u64 ^ seed;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(FNV_PRIME);
    }
    hash ^= hash >> 33;
    hash = hash.wrapping_mul(0xff51_afd7_ed55_8ccd);
    hash ^= hash >> 33;
    hash = hash.wrapping_mul(0xc4ce_b9fe_1a85_ec53);
    hash ^ (hash >> 33)
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
mod tests {
    use super::*;

    fn key(namespace: u8, value: u64) -> Vec<u8> {
        let mut key = vec![namespace];
        key.extend_from_slice(&value.to_le_bytes());
        key
    }

    #[test]
    fn round_trip_has_no_false_negatives_and_measured_false_positive_rate_is_bounded() {
        const KEYS: usize = 10_000;
        const ABSENT: usize = 50_000;
        let keys = (0..KEYS as u64)
            .map(|value| key(0x11, value))
            .collect::<Vec<_>>();
        let filter = BloomFilter::build(keys.iter().map(Vec::as_slice), keys.len())
            .expect("build deterministic Bloom filter");
        assert_eq!(filter.key_count(), KEYS as u64);
        assert_eq!(filter.bit_count(), 100_000);
        for present in &keys {
            assert!(
                filter.may_contain(present),
                "Bloom filter produced a false negative"
            );
        }

        let encoded = filter.encode().expect("encode Bloom filter");
        let decoded = BloomFilter::decode(&encoded, 64, KEYS as u64).expect("decode Bloom filter");
        assert_eq!(decoded, filter);

        let false_positives = (0..ABSENT as u64)
            .filter(|value| decoded.may_contain(&key(0x22, *value)))
            .count();
        assert_eq!(
            false_positives, 422,
            "stable hash/filter semantics changed; review the on-disk Bloom format before updating this fixture"
        );
        assert!(
            false_positives * 100 < ABSENT * 2,
            "deterministic Bloom false-positive rate exceeded 2%: {false_positives}/{ABSENT}"
        );
    }

    #[test]
    fn checksum_parameter_and_extent_corruption_fail_closed() {
        let keys = [b"alpha".as_slice(), b"beta".as_slice(), b"gamma".as_slice()];
        let filter = BloomFilter::build(keys, keys.len()).expect("build Bloom filter");
        let encoded = filter.encode().expect("encode Bloom filter");

        let mut bad_payload = encoded.clone();
        bad_payload[FILTER_HEADER_LEN] ^= 0x80;
        assert!(BloomFilter::decode(&bad_payload, 64, 3).is_err());

        let mut bad_probe_count = encoded.clone();
        bad_probe_count[13] = HASH_PROBES - 1;
        let header_crc = crc32fast::hash(&bad_probe_count[..36]);
        bad_probe_count[36..40].copy_from_slice(&header_crc.to_le_bytes());
        let payload_end = bad_probe_count.len() - FILTER_TRAILER_LEN;
        let crc = crc32fast::hash(&bad_probe_count[..payload_end]);
        bad_probe_count[payload_end..].copy_from_slice(&crc.to_le_bytes());
        assert!(BloomFilter::decode(&bad_probe_count, 64, 3).is_err());

        assert!(BloomFilter::decode(&encoded[..encoded.len() - 1], 64, 3).is_err());
        assert!(BloomFilter::decode(&encoded, 64, 4).is_err());
    }
}
