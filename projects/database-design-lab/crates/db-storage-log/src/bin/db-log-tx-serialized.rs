//! Serialized single-process Phase 5 transaction concurrency experiment.
//!
//! This executable uses the same version-2 incremental transaction records as
//! `db-log-tx-incremental`, but places the complete transaction commit critical section behind a
//! process-local mutex. Concurrent callers therefore receive unique contiguous transaction ids that
//! define one strict serialization order. The mutex covers validation, encoding, the underlying
//! `LogEngine::put` durable append/sync boundary, and in-memory publication. This does not claim
//! multi-process writer safety, deadlock detection, MVCC, or parallel commit execution.

use std::collections::BTreeMap;
use std::env;
use std::process::ExitCode;
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread;

use db_core::{validate_key, validate_key_value, DbError, KvEngine, Result, MAX_VALUE_BYTES};
use db_storage_log::LogEngine;

const LEGACY_SNAPSHOT_KEY: &[u8] = b"\0db-lab-atomic-snapshot-v1";
const TX_KEY_PREFIX: &[u8] = b"\0db-lab-tx-v2/";
const TX_MAGIC: [u8; 8] = *b"DBTXMUT2";
const TX_VERSION: u16 = 2;
const TX_HEADER_LEN: usize = 24;
const MUTATION_HEADER_LEN: usize = 12;
const TX_TRAILER_LEN: usize = 4;
const MAX_BATCH_MUTATIONS: usize = 1024;
const KIND_PUT: u8 = 1;
const KIND_DELETE: u8 = 2;

#[derive(Debug, Clone, PartialEq, Eq)]
enum Mutation {
    Put { key: Vec<u8>, value: Vec<u8> },
    Delete { key: Vec<u8> },
}

struct IncrementalTransactionEngine {
    backing: LogEngine,
    values: BTreeMap<Vec<u8>, Vec<u8>>,
    next_tx_id: u64,
}

impl IncrementalTransactionEngine {
    fn open(path: &str) -> Result<Self> {
        let backing = LogEngine::open(path)?;
        let inspection = LogEngine::inspect(path, true)?;
        let mut values = BTreeMap::new();
        let mut expected_tx_id = 1_u64;

        for entry in inspection.entries {
            let key = entry.key.into_vec();
            if key.as_slice() == LEGACY_SNAPSHOT_KEY {
                return Err(DbError::InvalidInput(
                    "legacy snapshot transaction database is not compatible with incremental v2; use db-log-tx for the v1 database"
                        .to_owned(),
                ));
            }
            let Some(tx_id) = parse_tx_key(&key)? else {
                return Err(corruption(format!(
                    "serialized transaction database contains unexpected live key {}",
                    encode_hex(&key)
                )));
            };
            if tx_id != expected_tx_id {
                return Err(corruption(format!(
                    "incremental transaction id discontinuity: expected {expected_tx_id}, found {tx_id}"
                )));
            }
            let bytes = entry
                .value
                .ok_or_else(|| corruption("inspection omitted transaction value"))?
                .into_vec();
            let mutations = decode_transaction(&bytes, tx_id)?;
            apply_mutations(&mut values, &mutations);
            expected_tx_id = expected_tx_id
                .checked_add(1)
                .ok_or_else(|| corruption("transaction id overflow during replay"))?;
        }

        Ok(Self {
            backing,
            values,
            next_tx_id: expected_tx_id,
        })
    }

    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        Ok(self.values.get(key).cloned())
    }

    fn commit_batch(&mut self, mutations: &[Mutation]) -> Result<u64> {
        validate_batch(mutations)?;
        let tx_id = self.next_tx_id;
        let encoded = encode_transaction(tx_id, mutations)?;
        let key = tx_key(tx_id);

        self.backing.put(&key, &encoded)?;

        apply_mutations(&mut self.values, mutations);
        self.next_tx_id = tx_id.checked_add(1).ok_or_else(|| {
            DbError::InvalidInput("transaction id space exhausted after commit".to_owned())
        })?;
        Ok(tx_id)
    }
}

struct SerializedTransactionEngine {
    inner: Mutex<IncrementalTransactionEngine>,
}

impl SerializedTransactionEngine {
    fn open(path: &str) -> Result<Self> {
        Ok(Self {
            inner: Mutex::new(IncrementalTransactionEngine::open(path)?),
        })
    }

    fn lock(&self) -> Result<MutexGuard<'_, IncrementalTransactionEngine>> {
        self.inner.lock().map_err(|_| DbError::Poisoned)
    }

    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        self.lock()?.get(key)
    }

    fn commit_batch(&self, mutations: &[Mutation]) -> Result<u64> {
        self.lock()?.commit_batch(mutations)
    }
}

fn validate_batch(mutations: &[Mutation]) -> Result<()> {
    if mutations.is_empty() {
        return Err(DbError::InvalidInput(
            "incremental transaction batch must not be empty".to_owned(),
        ));
    }
    if mutations.len() > MAX_BATCH_MUTATIONS {
        return Err(DbError::InvalidInput(format!(
            "incremental transaction has {} mutations; maximum is {MAX_BATCH_MUTATIONS}",
            mutations.len()
        )));
    }
    for mutation in mutations {
        match mutation {
            Mutation::Put { key, value } => validate_key_value(key, value)?,
            Mutation::Delete { key } => validate_key(key)?,
        }
    }
    Ok(())
}

fn apply_mutations(values: &mut BTreeMap<Vec<u8>, Vec<u8>>, mutations: &[Mutation]) {
    for mutation in mutations {
        match mutation {
            Mutation::Put { key, value } => {
                values.insert(key.clone(), value.clone());
            }
            Mutation::Delete { key } => {
                values.remove(key.as_slice());
            }
        }
    }
}

fn tx_key(tx_id: u64) -> Vec<u8> {
    let mut key = Vec::with_capacity(TX_KEY_PREFIX.len() + 8);
    key.extend_from_slice(TX_KEY_PREFIX);
    key.extend_from_slice(&tx_id.to_be_bytes());
    key
}

fn parse_tx_key(key: &[u8]) -> Result<Option<u64>> {
    let Some(suffix) = key.strip_prefix(TX_KEY_PREFIX) else {
        return Ok(None);
    };
    if suffix.len() != 8 {
        return Err(corruption(
            "incremental transaction key has invalid id width",
        ));
    }
    let mut bytes = [0_u8; 8];
    bytes.copy_from_slice(suffix);
    let tx_id = u64::from_be_bytes(bytes);
    if tx_id == 0 {
        return Err(corruption("incremental transaction id zero is reserved"));
    }
    Ok(Some(tx_id))
}

fn encode_transaction(tx_id: u64, mutations: &[Mutation]) -> Result<Vec<u8>> {
    if tx_id == 0 {
        return Err(DbError::InvalidInput(
            "transaction id zero is reserved".to_owned(),
        ));
    }
    let count = u32::try_from(mutations.len()).map_err(|_| {
        DbError::InvalidInput("transaction mutation count does not fit u32".to_owned())
    })?;
    let mut encoded = Vec::with_capacity(TX_HEADER_LEN + TX_TRAILER_LEN);
    encoded.extend_from_slice(&TX_MAGIC);
    encoded.extend_from_slice(&TX_VERSION.to_le_bytes());
    encoded.extend_from_slice(&0_u16.to_le_bytes());
    encoded.extend_from_slice(&tx_id.to_le_bytes());
    encoded.extend_from_slice(&count.to_le_bytes());

    for mutation in mutations {
        let (kind, key, value): (u8, &[u8], &[u8]) = match mutation {
            Mutation::Put { key, value } => (KIND_PUT, key, value),
            Mutation::Delete { key } => (KIND_DELETE, key, &[]),
        };
        let key_len = u32::try_from(key.len()).map_err(|_| {
            DbError::InvalidInput("transaction key length does not fit u32".to_owned())
        })?;
        let value_len = u32::try_from(value.len()).map_err(|_| {
            DbError::InvalidInput("transaction value length does not fit u32".to_owned())
        })?;
        encoded.push(kind);
        encoded.extend_from_slice(&[0_u8; 3]);
        encoded.extend_from_slice(&key_len.to_le_bytes());
        encoded.extend_from_slice(&value_len.to_le_bytes());
        encoded.extend_from_slice(key);
        encoded.extend_from_slice(value);
        if encoded.len() + TX_TRAILER_LEN > MAX_VALUE_BYTES {
            return Err(DbError::InvalidInput(format!(
                "encoded incremental transaction exceeds the {MAX_VALUE_BYTES}-byte backing value limit"
            )));
        }
    }

    let checksum = crc32fast::hash(&encoded);
    encoded.extend_from_slice(&checksum.to_le_bytes());
    Ok(encoded)
}

fn decode_transaction(bytes: &[u8], expected_tx_id: u64) -> Result<Vec<Mutation>> {
    if bytes.len() < TX_HEADER_LEN + TX_TRAILER_LEN {
        return Err(corruption("incremental transaction is truncated"));
    }
    if bytes[..8] != TX_MAGIC {
        return Err(corruption("incremental transaction magic mismatch"));
    }
    let version = read_u16(&bytes[8..10]);
    if version != TX_VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "incremental transaction",
            found: u64::from(version),
            supported: u64::from(TX_VERSION),
        });
    }
    if read_u16(&bytes[10..12]) != 0 {
        return Err(corruption(
            "incremental transaction reserved header bits are nonzero",
        ));
    }
    let tx_id = read_u64(&bytes[12..20]);
    if tx_id != expected_tx_id {
        return Err(corruption(format!(
            "incremental transaction key/value id mismatch: key={expected_tx_id}, value={tx_id}"
        )));
    }
    let count = usize::try_from(read_u32(&bytes[20..24]))
        .map_err(|_| corruption("transaction mutation count does not fit usize"))?;
    if count == 0 || count > MAX_BATCH_MUTATIONS {
        return Err(corruption(format!(
            "incremental transaction mutation count {count} is outside 1..={MAX_BATCH_MUTATIONS}"
        )));
    }

    let trailer_start = bytes
        .len()
        .checked_sub(TX_TRAILER_LEN)
        .ok_or_else(|| corruption("transaction checksum offset underflow"))?;
    let expected_crc = read_u32(&bytes[trailer_start..]);
    let actual_crc = crc32fast::hash(&bytes[..trailer_start]);
    if expected_crc != actual_crc {
        return Err(corruption(format!(
            "incremental transaction checksum mismatch: expected {expected_crc:08x}, computed {actual_crc:08x}"
        )));
    }

    let minimum = count
        .checked_mul(MUTATION_HEADER_LEN)
        .and_then(|payload| payload.checked_add(TX_HEADER_LEN + TX_TRAILER_LEN))
        .ok_or_else(|| corruption("transaction mutation count overflow"))?;
    if minimum > bytes.len() {
        return Err(corruption(
            "transaction mutation count exceeds available payload",
        ));
    }

    let mut cursor = TX_HEADER_LEN;
    let mut mutations = Vec::with_capacity(count);
    for _ in 0..count {
        let header_end = cursor
            .checked_add(MUTATION_HEADER_LEN)
            .ok_or_else(|| corruption("transaction mutation header overflow"))?;
        if header_end > trailer_start {
            return Err(corruption("transaction mutation header is truncated"));
        }
        let kind = bytes[cursor];
        if bytes[cursor + 1..cursor + 4] != [0_u8; 3] {
            return Err(corruption(
                "transaction mutation reserved header bits are nonzero",
            ));
        }
        let key_len = usize::try_from(read_u32(&bytes[cursor + 4..cursor + 8]))
            .map_err(|_| corruption("transaction key length does not fit usize"))?;
        let value_len = usize::try_from(read_u32(&bytes[cursor + 8..header_end]))
            .map_err(|_| corruption("transaction value length does not fit usize"))?;
        cursor = header_end;
        let key_end = cursor
            .checked_add(key_len)
            .ok_or_else(|| corruption("transaction key extent overflow"))?;
        let value_end = key_end
            .checked_add(value_len)
            .ok_or_else(|| corruption("transaction value extent overflow"))?;
        if value_end > trailer_start {
            return Err(corruption("transaction mutation payload is truncated"));
        }
        let key = bytes[cursor..key_end].to_vec();
        let value = bytes[key_end..value_end].to_vec();
        let mutation = match kind {
            KIND_PUT => {
                validate_key_value(&key, &value).map_err(|error| {
                    corruption(format!("transaction PUT violates KV bounds: {error}"))
                })?;
                Mutation::Put { key, value }
            }
            KIND_DELETE => {
                if value_len != 0 {
                    return Err(corruption(
                        "transaction DELETE unexpectedly contains a value",
                    ));
                }
                validate_key(&key).map_err(|error| {
                    corruption(format!("transaction DELETE violates KV bounds: {error}"))
                })?;
                Mutation::Delete { key }
            }
            other => {
                return Err(corruption(format!(
                    "unknown transaction mutation kind {other}"
                )))
            }
        };
        mutations.push(mutation);
        cursor = value_end;
    }
    if cursor != trailer_start {
        return Err(corruption(
            "incremental transaction has unexplained trailing payload",
        ));
    }
    Ok(mutations)
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

fn read_u64(bytes: &[u8]) -> u64 {
    let mut array = [0_u8; 8];
    array.copy_from_slice(bytes);
    u64::from_le_bytes(array)
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

fn parse_batch(argument: &str) -> std::result::Result<Vec<Mutation>, String> {
    if argument.is_empty() {
        return Err("concurrent batch must not be empty".to_owned());
    }
    let mutations = argument
        .split(',')
        .map(parse_mutation)
        .collect::<std::result::Result<Vec<_>, _>>()?;
    validate_batch(&mutations).map_err(|error| error.to_string())?;
    Ok(mutations)
}

fn run() -> std::result::Result<(), String> {
    let mut args = env::args().skip(1);
    let path = args.next().ok_or_else(|| {
        "usage: db-log-tx-serialized <path> get <hex-key> | batch <mutation>... | concurrent <batch>..."
            .to_owned()
    })?;
    let command = args
        .next()
        .ok_or_else(|| "missing command: expected get, batch, or concurrent".to_owned())?;

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
                SerializedTransactionEngine::open(&path).map_err(|error| error.to_string())?;
            match engine.get(&key).map_err(|error| error.to_string())? {
                Some(value) => println!("{}", encode_hex(&value)),
                None => println!("null"),
            }
        }
        "batch" => {
            let mutations = args
                .map(|argument| parse_mutation(&argument))
                .collect::<std::result::Result<Vec<_>, _>>()?;
            validate_batch(&mutations).map_err(|error| error.to_string())?;
            let engine =
                SerializedTransactionEngine::open(&path).map_err(|error| error.to_string())?;
            let tx_id = engine
                .commit_batch(&mutations)
                .map_err(|error| error.to_string())?;
            println!("committed {} tx={tx_id}", mutations.len());
        }
        "concurrent" => {
            let batches = args
                .map(|argument| parse_batch(&argument))
                .collect::<std::result::Result<Vec<_>, _>>()?;
            if batches.is_empty() {
                return Err("concurrent requires at least one comma-separated batch".to_owned());
            }

            let engine = Arc::new(
                SerializedTransactionEngine::open(&path).map_err(|error| error.to_string())?,
            );
            let handles = batches
                .into_iter()
                .enumerate()
                .map(|(worker, batch)| {
                    let engine = Arc::clone(&engine);
                    thread::spawn(move || {
                        let mutation_count = batch.len();
                        let tx_id = engine
                            .commit_batch(&batch)
                            .map_err(|error| error.to_string())?;
                        Ok::<_, String>((worker, mutation_count, tx_id))
                    })
                })
                .collect::<Vec<_>>();

            let mut results = Vec::with_capacity(handles.len());
            for handle in handles {
                results.push(
                    handle
                        .join()
                        .map_err(|_| "concurrent transaction worker panicked".to_owned())??,
                );
            }
            results.sort_by_key(|(worker, _, _)| *worker);
            for (worker, mutation_count, tx_id) in results {
                println!("worker={worker} committed {mutation_count} tx={tx_id}");
            }
        }
        _ => {
            return Err(format!(
                "unknown command {command:?}; expected get, batch, or concurrent"
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
    use std::sync::{Arc, Barrier};
    use std::thread;

    use db_core::KvEngine;
    use db_storage_memory::MemoryEngine;
    use tempfile::tempdir;

    use super::{IncrementalTransactionEngine, Mutation, SerializedTransactionEngine};

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

    #[test]
    fn concurrent_commits_match_tx_id_ordered_memory_oracle_after_reopen() {
        let dir = tempdir().expect("tempdir");
        let path = dir.path().join("serialized.log");
        let path = path.to_str().expect("utf8 path").to_owned();
        let engine = Arc::new(SerializedTransactionEngine::open(&path).expect("open"));
        let batches = vec![
            vec![
                Mutation::Put {
                    key: b"x".to_vec(),
                    value: b"one".to_vec(),
                },
                Mutation::Put {
                    key: b"a".to_vec(),
                    value: b"1".to_vec(),
                },
            ],
            vec![
                Mutation::Put {
                    key: b"x".to_vec(),
                    value: b"two".to_vec(),
                },
                Mutation::Delete { key: b"a".to_vec() },
            ],
            vec![
                Mutation::Put {
                    key: b"b".to_vec(),
                    value: b"3".to_vec(),
                },
                Mutation::Put {
                    key: b"x".to_vec(),
                    value: b"three".to_vec(),
                },
            ],
        ];
        let barrier = Arc::new(Barrier::new(batches.len() + 1));
        let handles = batches
            .into_iter()
            .map(|batch| {
                let engine = Arc::clone(&engine);
                let barrier = Arc::clone(&barrier);
                thread::spawn(move || {
                    barrier.wait();
                    let tx_id = engine.commit_batch(&batch).expect("concurrent commit");
                    (tx_id, batch)
                })
            })
            .collect::<Vec<_>>();

        barrier.wait();
        let mut committed = handles
            .into_iter()
            .map(|handle| handle.join().expect("worker"))
            .collect::<Vec<_>>();
        committed.sort_by_key(|(tx_id, _)| *tx_id);
        assert_eq!(
            committed
                .iter()
                .map(|(tx_id, _)| *tx_id)
                .collect::<Vec<_>>(),
            vec![1, 2, 3]
        );

        let mut oracle = MemoryEngine::new();
        for (_, batch) in &committed {
            apply_oracle(&mut oracle, batch);
        }
        for key in [b"a".as_slice(), b"b".as_slice(), b"x".as_slice()] {
            assert_eq!(
                engine.get(key).expect("serialized get"),
                oracle.get(key).expect("oracle get"),
                "live mismatch for {key:?}"
            );
        }

        drop(engine);
        let reopened = IncrementalTransactionEngine::open(&path).expect("reopen");
        for key in [b"a".as_slice(), b"b".as_slice(), b"x".as_slice()] {
            assert_eq!(
                reopened.get(key).expect("reopened get"),
                oracle.get(key).expect("oracle get"),
                "reopen mismatch for {key:?}"
            );
        }
    }
}
