//! Serializable read-write Phase 5 transaction experiment.
//!
//! A transaction closure holds one process-local mutex across reads, decisions, staged writes,
//! validation, the existing `LogEngine::put` append+sync durability boundary, and in-memory
//! publication. Reads observe prior committed state plus the transaction's own staged writes.
//! If the closure returns an error, nothing is appended or published. This intentionally does not
//! claim multi-process isolation, MVCC, deadlock detection, or parallel commit execution.

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

struct DurableState {
    backing: LogEngine,
    values: BTreeMap<Vec<u8>, Vec<u8>>,
    next_tx_id: u64,
}

impl DurableState {
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
                    "read-write transaction database contains unexpected live key {}",
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

    fn commit(&mut self, mutations: &[Mutation]) -> Result<u64> {
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

struct ReadWriteTransaction<'a> {
    base: &'a BTreeMap<Vec<u8>, Vec<u8>>,
    overlay: BTreeMap<Vec<u8>, Option<Vec<u8>>>,
    mutations: Vec<Mutation>,
}

impl<'a> ReadWriteTransaction<'a> {
    fn new(base: &'a BTreeMap<Vec<u8>, Vec<u8>>) -> Self {
        Self {
            base,
            overlay: BTreeMap::new(),
            mutations: Vec::new(),
        }
    }

    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        if let Some(value) = self.overlay.get(key) {
            return Ok(value.clone());
        }
        Ok(self.base.get(key).cloned())
    }

    fn put(&mut self, key: &[u8], value: &[u8]) -> Result<()> {
        validate_key_value(key, value)?;
        self.overlay.insert(key.to_vec(), Some(value.to_vec()));
        self.mutations.push(Mutation::Put {
            key: key.to_vec(),
            value: value.to_vec(),
        });
        Ok(())
    }

    #[cfg_attr(
        not(test),
        expect(
            dead_code,
            reason = "DELETE is part of the transaction surface and is exercised by regression transactions, while the CLI counter demo only needs PUT"
        )
    )]
    fn delete(&mut self, key: &[u8]) -> Result<()> {
        validate_key(key)?;
        self.overlay.insert(key.to_vec(), None);
        self.mutations.push(Mutation::Delete { key: key.to_vec() });
        Ok(())
    }

    fn into_mutations(self) -> Vec<Mutation> {
        self.mutations
    }
}

struct SerializableReadWriteEngine {
    inner: Mutex<DurableState>,
}

impl SerializableReadWriteEngine {
    fn open(path: &str) -> Result<Self> {
        Ok(Self {
            inner: Mutex::new(DurableState::open(path)?),
        })
    }

    fn lock(&self) -> Result<MutexGuard<'_, DurableState>> {
        self.inner.lock().map_err(|_| DbError::Poisoned)
    }

    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        self.lock()?.get(key)
    }

    fn transaction<T, F>(&self, body: F) -> Result<(Option<u64>, T)>
    where
        F: FnOnce(&mut ReadWriteTransaction<'_>) -> Result<T>,
    {
        let mut state = self.lock()?;
        let mut tx = ReadWriteTransaction::new(&state.values);
        let output = body(&mut tx)?;
        let mutations = tx.into_mutations();
        if mutations.is_empty() {
            return Ok((None, output));
        }
        let tx_id = state.commit(&mutations)?;
        Ok((Some(tx_id), output))
    }
}

fn validate_batch(mutations: &[Mutation]) -> Result<()> {
    if mutations.is_empty() {
        return Err(DbError::InvalidInput(
            "read-write transaction batch must not be empty".to_owned(),
        ));
    }
    if mutations.len() > MAX_BATCH_MUTATIONS {
        return Err(DbError::InvalidInput(format!(
            "read-write transaction has {} mutations; maximum is {MAX_BATCH_MUTATIONS}",
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

fn encode_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for &byte in bytes {
        encoded.push(char::from(HEX[usize::from(byte >> 4)]));
        encoded.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    encoded
}

fn counter_value(value: Option<Vec<u8>>) -> Result<u64> {
    let Some(value) = value else {
        return Ok(0);
    };
    if value.len() != 8 {
        return Err(corruption("counter value is not an encoded u64"));
    }
    let mut bytes = [0_u8; 8];
    bytes.copy_from_slice(&value);
    Ok(u64::from_le_bytes(bytes))
}

fn run_demo(path: &str, workers: usize) -> Result<u64> {
    let engine = Arc::new(SerializableReadWriteEngine::open(path)?);
    if engine.get(b"counter")?.is_none() {
        engine.transaction(|tx| tx.put(b"counter", &0_u64.to_le_bytes()))?;
    }

    let mut handles = Vec::with_capacity(workers);
    for _ in 0..workers {
        let engine = Arc::clone(&engine);
        handles.push(thread::spawn(move || -> Result<()> {
            engine.transaction(|tx| {
                let current = counter_value(tx.get(b"counter")?)?;
                tx.put(b"counter", &current.saturating_add(1).to_le_bytes())
            })?;
            Ok(())
        }));
    }
    for handle in handles {
        handle.join().map_err(|_| DbError::Poisoned)??;
    }
    counter_value(engine.get(b"counter")?)
}

fn main() -> ExitCode {
    let mut args = env::args().skip(1);
    let Some(path) = args.next() else {
        eprintln!("usage: db-log-tx-readwrite <path> [workers]");
        return ExitCode::from(2);
    };
    let workers = match args.next() {
        Some(raw) => match raw.parse::<usize>() {
            Ok(value) => value,
            Err(error) => {
                eprintln!("invalid worker count: {error}");
                return ExitCode::from(2);
            }
        },
        None => 8,
    };
    if args.next().is_some() {
        eprintln!("usage: db-log-tx-readwrite <path> [workers]");
        return ExitCode::from(2);
    }

    match run_demo(&path, workers) {
        Ok(value) => {
            println!("counter={value}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use db_storage_memory::MemoryEngine;
    use tempfile::tempdir;

    #[test]
    fn read_your_writes_commit_and_reopen() {
        let dir = tempdir().expect("tempdir");
        let path = dir.path().join("readwrite.log");
        let path = path.to_str().expect("utf8 path");
        let engine = SerializableReadWriteEngine::open(path).expect("open engine");

        let (tx_id, observed) = engine
            .transaction(|tx| {
                assert_eq!(tx.get(b"a")?, None);
                tx.put(b"a", b"one")?;
                assert_eq!(tx.get(b"a")?, Some(b"one".to_vec()));
                tx.delete(b"a")?;
                assert_eq!(tx.get(b"a")?, None);
                tx.put(b"a", b"two")?;
                tx.get(b"a")
            })
            .expect("commit read-write transaction");
        assert_eq!(tx_id, Some(1));
        assert_eq!(observed, Some(b"two".to_vec()));
        drop(engine);

        let reopened = SerializableReadWriteEngine::open(path).expect("reopen engine");
        assert_eq!(reopened.get(b"a").expect("get a"), Some(b"two".to_vec()));
    }

    #[test]
    fn closure_error_rolls_back_without_consuming_transaction_id() {
        let dir = tempdir().expect("tempdir");
        let path = dir.path().join("rollback.log");
        let path = path.to_str().expect("utf8 path");
        let engine = SerializableReadWriteEngine::open(path).expect("open engine");

        let result: Result<(Option<u64>, ())> = engine.transaction(|tx| {
            tx.put(b"key", b"uncommitted")?;
            Err(DbError::InvalidInput("abort".to_owned()))
        });
        assert!(result.is_err());
        assert_eq!(engine.get(b"key").expect("get key"), None);

        let (tx_id, ()) = engine
            .transaction(|tx| tx.put(b"key", b"committed"))
            .expect("commit after abort");
        assert_eq!(tx_id, Some(1));
        drop(engine);

        let reopened = SerializableReadWriteEngine::open(path).expect("reopen engine");
        assert_eq!(
            reopened.get(b"key").expect("get key"),
            Some(b"committed".to_vec())
        );
    }

    #[test]
    fn serialized_read_modify_write_prevents_lost_updates_and_reopens() {
        let dir = tempdir().expect("tempdir");
        let path = dir.path().join("concurrent.log");
        let path = path.to_str().expect("utf8 path").to_owned();
        let engine = Arc::new(SerializableReadWriteEngine::open(&path).expect("open engine"));
        engine
            .transaction(|tx| tx.put(b"counter", &0_u64.to_le_bytes()))
            .expect("initialize counter");

        let workers = 12_u64;
        let mut handles = Vec::new();
        for _ in 0..workers {
            let engine = Arc::clone(&engine);
            handles.push(thread::spawn(move || {
                engine
                    .transaction(|tx| {
                        let current = counter_value(tx.get(b"counter")?)?;
                        tx.put(b"counter", &(current + 1).to_le_bytes())
                    })
                    .map(|(tx_id, ())| tx_id.expect("write transaction id"))
            }));
        }

        let mut ids = handles
            .into_iter()
            .map(|handle| {
                handle
                    .join()
                    .expect("worker join")
                    .expect("worker transaction")
            })
            .collect::<Vec<_>>();
        ids.sort_unstable();
        assert_eq!(ids, (2..=workers + 1).collect::<Vec<_>>());
        assert_eq!(
            counter_value(engine.get(b"counter").expect("get counter")).expect("decode counter"),
            workers
        );
        drop(engine);

        let reopened = SerializableReadWriteEngine::open(&path).expect("reopen engine");
        assert_eq!(
            counter_value(reopened.get(b"counter").expect("get counter")).expect("decode counter"),
            workers
        );
    }

    #[test]
    fn ordered_program_matches_memory_engine_oracle_after_reopen() {
        let dir = tempdir().expect("tempdir");
        let path = dir.path().join("oracle.log");
        let path = path.to_str().expect("utf8 path");
        let engine = SerializableReadWriteEngine::open(path).expect("open engine");
        let mut oracle = MemoryEngine::new();

        engine
            .transaction(|tx| {
                tx.put(b"a", b"10")?;
                tx.put(b"b", b"7")
            })
            .expect("first transaction");
        oracle.put(b"a", b"10").expect("oracle put a");
        oracle.put(b"b", b"7").expect("oracle put b");

        engine
            .transaction(|tx| {
                let a = tx.get(b"a")?.expect("a exists");
                let b = tx.get(b"b")?.expect("b exists");
                let sum = format!(
                    "{}",
                    std::str::from_utf8(&a)
                        .expect("utf8 a")
                        .parse::<u64>()
                        .expect("parse a")
                        + std::str::from_utf8(&b)
                            .expect("utf8 b")
                            .parse::<u64>()
                            .expect("parse b")
                );
                tx.put(b"sum", sum.as_bytes())?;
                tx.delete(b"a")?;
                assert_eq!(tx.get(b"a")?, None);
                Ok(())
            })
            .expect("second transaction");
        oracle.put(b"sum", b"17").expect("oracle put sum");
        oracle.delete(b"a").expect("oracle delete a");
        drop(engine);

        let reopened = SerializableReadWriteEngine::open(path).expect("reopen engine");
        for key in [b"a".as_slice(), b"b".as_slice(), b"sum".as_slice()] {
            assert_eq!(
                reopened.get(key).expect("candidate get"),
                oracle.get(key).expect("oracle get"),
                "differential mismatch for {}",
                String::from_utf8_lossy(key)
            );
        }
    }
}
