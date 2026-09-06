//! Executable Phase 5 transaction slice built on the durable append-log engine.
//!
//! Each committed batch rewrites the complete logical snapshot into exactly one append-log value.
//! This is intentionally a bounded educational design, not a scalable transaction manager: the
//! encoded live state must fit the common 1 MiB value limit. In return, atomicity inherits the
//! append-log's existing checksummed-record and `sync_data` boundary: reopen observes the complete
//! new snapshot or discards an incomplete final record as a unit.

use std::collections::BTreeMap;
use std::env;
use std::process::ExitCode;

use db_core::{validate_key, validate_key_value, DbError, KvEngine, Result, MAX_VALUE_BYTES};
use db_storage_log::LogEngine;

const SNAPSHOT_KEY: &[u8] = b"\0db-lab-atomic-snapshot-v1";
const SNAPSHOT_MAGIC: [u8; 8] = *b"DBTXSNAP";
const SNAPSHOT_VERSION: u16 = 1;
const SNAPSHOT_HEADER_LEN: usize = 16;
const SNAPSHOT_TRAILER_LEN: usize = 4;
const MAX_BATCH_MUTATIONS: usize = 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
enum Mutation {
    Put { key: Vec<u8>, value: Vec<u8> },
    Delete { key: Vec<u8> },
}

struct SnapshotTransactionEngine {
    backing: LogEngine,
    values: BTreeMap<Vec<u8>, Vec<u8>>,
}

impl SnapshotTransactionEngine {
    fn open(path: &str) -> Result<Self> {
        let mut backing = LogEngine::open(path)?;
        let values = match backing.get(SNAPSHOT_KEY)? {
            Some(bytes) => decode_snapshot(&bytes)?,
            None => BTreeMap::new(),
        };
        Ok(Self { backing, values })
    }

    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        Ok(self.values.get(key).cloned())
    }

    fn commit_batch(&mut self, mutations: &[Mutation]) -> Result<Vec<Option<Vec<u8>>>> {
        if mutations.len() > MAX_BATCH_MUTATIONS {
            return Err(DbError::InvalidInput(format!(
                "atomic batch has {} mutations; maximum is {MAX_BATCH_MUTATIONS}",
                mutations.len()
            )));
        }
        validate_mutations(mutations)?;
        if mutations.is_empty() {
            return Ok(Vec::new());
        }

        let mut candidate = self.values.clone();
        let mut previous = Vec::with_capacity(mutations.len());
        for mutation in mutations {
            match mutation {
                Mutation::Put { key, value } => {
                    previous.push(candidate.insert(key.clone(), value.clone()));
                }
                Mutation::Delete { key } => {
                    previous.push(candidate.remove(key.as_slice()));
                }
            }
        }

        let encoded = encode_snapshot(&candidate)?;
        self.backing.put(SNAPSHOT_KEY, &encoded)?;
        self.values = candidate;
        Ok(previous)
    }

    #[cfg(test)]
    fn reopen(&mut self) -> Result<()> {
        self.backing.reopen()?;
        self.values = match self.backing.get(SNAPSHOT_KEY)? {
            Some(bytes) => decode_snapshot(&bytes)?,
            None => BTreeMap::new(),
        };
        Ok(())
    }
}

fn validate_mutations(mutations: &[Mutation]) -> Result<()> {
    for mutation in mutations {
        match mutation {
            Mutation::Put { key, value } => validate_key_value(key, value)?,
            Mutation::Delete { key } => validate_key(key)?,
        }
    }
    Ok(())
}

fn encode_snapshot(values: &BTreeMap<Vec<u8>, Vec<u8>>) -> Result<Vec<u8>> {
    let count = u32::try_from(values.len())
        .map_err(|_| DbError::InvalidInput("snapshot entry count does not fit u32".to_owned()))?;
    let mut encoded = Vec::with_capacity(SNAPSHOT_HEADER_LEN + SNAPSHOT_TRAILER_LEN);
    encoded.extend_from_slice(&SNAPSHOT_MAGIC);
    encoded.extend_from_slice(&SNAPSHOT_VERSION.to_le_bytes());
    encoded.extend_from_slice(&0_u16.to_le_bytes());
    encoded.extend_from_slice(&count.to_le_bytes());

    for (key, value) in values {
        validate_key_value(key, value)?;
        let key_len = u32::try_from(key.len()).map_err(|_| {
            DbError::InvalidInput("snapshot key length does not fit u32".to_owned())
        })?;
        let value_len = u32::try_from(value.len()).map_err(|_| {
            DbError::InvalidInput("snapshot value length does not fit u32".to_owned())
        })?;
        encoded.extend_from_slice(&key_len.to_le_bytes());
        encoded.extend_from_slice(&value_len.to_le_bytes());
        encoded.extend_from_slice(key);
        encoded.extend_from_slice(value);
        if encoded.len() + SNAPSHOT_TRAILER_LEN > MAX_VALUE_BYTES {
            return Err(DbError::InvalidInput(format!(
                "encoded atomic snapshot exceeds the {MAX_VALUE_BYTES}-byte backing value limit"
            )));
        }
    }

    let checksum = crc32fast::hash(&encoded);
    encoded.extend_from_slice(&checksum.to_le_bytes());
    if encoded.len() > MAX_VALUE_BYTES {
        return Err(DbError::InvalidInput(format!(
            "encoded atomic snapshot exceeds the {MAX_VALUE_BYTES}-byte backing value limit"
        )));
    }
    Ok(encoded)
}

fn decode_snapshot(bytes: &[u8]) -> Result<BTreeMap<Vec<u8>, Vec<u8>>> {
    if bytes.len() < SNAPSHOT_HEADER_LEN + SNAPSHOT_TRAILER_LEN {
        return Err(corruption("atomic snapshot is truncated"));
    }
    if bytes[..8] != SNAPSHOT_MAGIC {
        return Err(corruption("atomic snapshot magic mismatch"));
    }
    let version = read_u16(&bytes[8..10]);
    if version != SNAPSHOT_VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "atomic snapshot",
            found: u64::from(version),
            supported: u64::from(SNAPSHOT_VERSION),
        });
    }
    if read_u16(&bytes[10..12]) != 0 {
        return Err(corruption(
            "atomic snapshot reserved header bits are nonzero",
        ));
    }
    let trailer_start = bytes
        .len()
        .checked_sub(SNAPSHOT_TRAILER_LEN)
        .ok_or_else(|| corruption("atomic snapshot checksum offset underflow"))?;
    let expected_crc = read_u32(&bytes[trailer_start..]);
    let actual_crc = crc32fast::hash(&bytes[..trailer_start]);
    if expected_crc != actual_crc {
        return Err(corruption(format!(
            "atomic snapshot checksum mismatch: expected {expected_crc:08x}, computed {actual_crc:08x}"
        )));
    }

    let count = usize::try_from(read_u32(&bytes[12..16]))
        .map_err(|_| corruption("atomic snapshot entry count does not fit usize"))?;
    let minimum_entry_bytes = count
        .checked_mul(8)
        .and_then(|bytes| bytes.checked_add(SNAPSHOT_HEADER_LEN + SNAPSHOT_TRAILER_LEN))
        .ok_or_else(|| corruption("atomic snapshot entry count overflow"))?;
    if minimum_entry_bytes > bytes.len() {
        return Err(corruption("atomic snapshot entry count exceeds payload"));
    }

    let mut cursor = SNAPSHOT_HEADER_LEN;
    let mut values = BTreeMap::new();
    let mut previous_key: Option<Vec<u8>> = None;
    for _ in 0..count {
        let lengths_end = cursor
            .checked_add(8)
            .ok_or_else(|| corruption("atomic snapshot length header overflow"))?;
        if lengths_end > trailer_start {
            return Err(corruption("atomic snapshot entry header is truncated"));
        }
        let key_len = usize::try_from(read_u32(&bytes[cursor..cursor + 4]))
            .map_err(|_| corruption("atomic snapshot key length does not fit usize"))?;
        let value_len = usize::try_from(read_u32(&bytes[cursor + 4..lengths_end]))
            .map_err(|_| corruption("atomic snapshot value length does not fit usize"))?;
        cursor = lengths_end;
        let key_end = cursor
            .checked_add(key_len)
            .ok_or_else(|| corruption("atomic snapshot key extent overflow"))?;
        let value_end = key_end
            .checked_add(value_len)
            .ok_or_else(|| corruption("atomic snapshot value extent overflow"))?;
        if value_end > trailer_start {
            return Err(corruption("atomic snapshot entry payload is truncated"));
        }
        let key = bytes[cursor..key_end].to_vec();
        let value = bytes[key_end..value_end].to_vec();
        validate_key_value(&key, &value).map_err(|error| {
            corruption(format!("atomic snapshot entry violates KV bounds: {error}"))
        })?;
        if previous_key
            .as_ref()
            .is_some_and(|previous| previous >= &key)
        {
            return Err(corruption(
                "atomic snapshot keys are not strictly increasing",
            ));
        }
        previous_key = Some(key.clone());
        values.insert(key, value);
        cursor = value_end;
    }
    if cursor != trailer_start {
        return Err(corruption(
            "atomic snapshot has unexplained trailing payload",
        ));
    }
    Ok(values)
}

fn corruption(reason: impl Into<String>) -> DbError {
    DbError::Corruption {
        offset: 0,
        reason: reason.into(),
    }
}

fn read_u16(bytes: &[u8]) -> u16 {
    let mut array = [0_u8; 2];
    array.copy_from_slice(bytes);
    u16::from_le_bytes(array)
}

fn read_u32(bytes: &[u8]) -> u32 {
    let mut array = [0_u8; 4];
    array.copy_from_slice(bytes);
    u32::from_le_bytes(array)
}

fn decode_hex(input: &str) -> std::result::Result<Vec<u8>, String> {
    if input.len() % 2 != 0 {
        return Err(format!("hex input has odd length: {input}"));
    }
    let bytes = input.as_bytes();
    let mut decoded = Vec::with_capacity(bytes.len() / 2);
    let mut index = 0;
    while index < bytes.len() {
        let high = hex_nibble(bytes[index])?;
        let low = hex_nibble(bytes[index + 1])?;
        decoded.push((high << 4) | low);
        index += 2;
    }
    Ok(decoded)
}

fn hex_nibble(byte: u8) -> std::result::Result<u8, String> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        b'A'..=b'F' => Ok(byte - b'A' + 10),
        _ => Err(format!("invalid hex byte {:?}", char::from(byte))),
    }
}

fn encode_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for &byte in bytes {
        encoded.push(char::from(HEX[usize::from(byte >> 4)]));
        encoded.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    encoded
}

fn parse_mutation(argument: &str) -> std::result::Result<Mutation, String> {
    if let Some(rest) = argument.strip_prefix("put:") {
        let (key, value) = rest
            .split_once(':')
            .ok_or_else(|| "put mutation must be put:<hex-key>:<hex-value>".to_owned())?;
        return Ok(Mutation::Put {
            key: decode_hex(key)?,
            value: decode_hex(value)?,
        });
    }
    if let Some(key) = argument.strip_prefix("delete:") {
        return Ok(Mutation::Delete {
            key: decode_hex(key)?,
        });
    }
    Err(format!(
        "unknown mutation {argument:?}; expected put:<hex-key>:<hex-value> or delete:<hex-key>"
    ))
}

fn run() -> std::result::Result<(), String> {
    let mut args = env::args().skip(1);
    let path = args
        .next()
        .ok_or_else(|| "usage: db-log-tx <path> get <hex-key> | batch <mutation>...".to_owned())?;
    let command = args
        .next()
        .ok_or_else(|| "missing command: expected get or batch".to_owned())?;

    match command.as_str() {
        "get" => {
            let key = args
                .next()
                .ok_or_else(|| "get requires one hex key".to_owned())?;
            if args.next().is_some() {
                return Err("get accepts exactly one hex key".to_owned());
            }
            let key = decode_hex(&key)?;
            let engine =
                SnapshotTransactionEngine::open(&path).map_err(|error| error.to_string())?;
            match engine.get(&key).map_err(|error| error.to_string())? {
                Some(value) => println!("{}", encode_hex(&value)),
                None => println!("null"),
            }
        }
        "batch" => {
            let mutations = args
                .map(|argument| parse_mutation(&argument))
                .collect::<std::result::Result<Vec<_>, _>>()?;
            if mutations.is_empty() {
                return Err("batch requires at least one mutation".to_owned());
            }
            validate_mutations(&mutations).map_err(|error| error.to_string())?;
            let mut engine =
                SnapshotTransactionEngine::open(&path).map_err(|error| error.to_string())?;
            engine
                .commit_batch(&mutations)
                .map_err(|error| error.to_string())?;
            println!("committed {}", mutations.len());
        }
        _ => {
            return Err(format!(
                "unknown command {command:?}; expected get or batch"
            ))
        }
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use db_core::KvEngine;
    use db_storage_memory::MemoryEngine;
    use tempfile::tempdir;

    use super::{Mutation, SnapshotTransactionEngine};

    fn apply_oracle(engine: &mut MemoryEngine, mutations: &[Mutation]) {
        for mutation in mutations {
            match mutation {
                Mutation::Put { key, value } => {
                    engine.put(key, value).expect("oracle put");
                }
                Mutation::Delete { key } => {
                    engine.delete(key).expect("oracle delete");
                }
            }
        }
    }

    fn assert_matches_oracle(
        engine: &SnapshotTransactionEngine,
        oracle: &mut MemoryEngine,
        keys: &[&[u8]],
    ) {
        for key in keys {
            assert_eq!(
                engine.get(key).expect("transaction get"),
                oracle.get(key).expect("oracle get"),
                "mismatch for key {key:?}"
            );
        }
    }

    #[test]
    fn deterministic_batches_match_memory_oracle_across_reopen() {
        let dir = tempdir().expect("tempdir");
        let path = dir.path().join("tx.log");
        let mut engine = SnapshotTransactionEngine::open(path.to_str().expect("utf8 path"))
            .expect("open transaction engine");
        let mut oracle = MemoryEngine::new();

        let batches = vec![
            vec![
                Mutation::Put {
                    key: b"a".to_vec(),
                    value: b"one".to_vec(),
                },
                Mutation::Put {
                    key: b"b".to_vec(),
                    value: b"two".to_vec(),
                },
            ],
            vec![
                Mutation::Put {
                    key: b"a".to_vec(),
                    value: b"new".to_vec(),
                },
                Mutation::Delete { key: b"b".to_vec() },
                Mutation::Put {
                    key: Vec::new(),
                    value: Vec::new(),
                },
            ],
            vec![
                Mutation::Delete { key: b"a".to_vec() },
                Mutation::Put {
                    key: b"a".to_vec(),
                    value: b"again".to_vec(),
                },
            ],
        ];

        for batch in batches {
            engine.commit_batch(&batch).expect("commit batch");
            apply_oracle(&mut oracle, &batch);
            assert_matches_oracle(&engine, &mut oracle, &[b"", b"a", b"b"]);
            engine.reopen().expect("reopen transaction engine");
            assert_matches_oracle(&engine, &mut oracle, &[b"", b"a", b"b"]);
        }
    }
}
