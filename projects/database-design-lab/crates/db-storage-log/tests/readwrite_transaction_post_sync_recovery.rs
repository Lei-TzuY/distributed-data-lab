use std::process::{Command, Output};

use db_core::KvEngine;
use db_storage_log::LogEngine;
use tempfile::tempdir;

const TX_KEY_PREFIX: &[u8] = b"\0db-lab-tx-v2/";
const TX_MAGIC: [u8; 8] = *b"DBTXMUT2";
const TX_VERSION: u16 = 2;
const KIND_PUT: u8 = 1;

fn run(path: &str, workers: &str) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-log-tx-readwrite"))
        .args([path, workers])
        .output()
        .expect("run db-log-tx-readwrite")
}

fn assert_success(output: &Output) {
    assert!(
        output.status.success(),
        "command failed: status={:?}, stdout={}, stderr={}",
        output.status.code(),
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn stdout(output: &Output) -> String {
    String::from_utf8(output.stdout.clone())
        .expect("utf8 stdout")
        .trim()
        .to_owned()
}

fn tx_key(tx_id: u64) -> Vec<u8> {
    let mut key = Vec::with_capacity(TX_KEY_PREFIX.len() + 8);
    key.extend_from_slice(TX_KEY_PREFIX);
    key.extend_from_slice(&tx_id.to_be_bytes());
    key
}

fn encode_single_put(tx_id: u64, key: &[u8], value: &[u8]) -> Vec<u8> {
    let mut encoded = Vec::new();
    encoded.extend_from_slice(&TX_MAGIC);
    encoded.extend_from_slice(&TX_VERSION.to_le_bytes());
    encoded.extend_from_slice(&0_u16.to_le_bytes());
    encoded.extend_from_slice(&tx_id.to_le_bytes());
    encoded.extend_from_slice(&1_u32.to_le_bytes());
    encoded.push(KIND_PUT);
    encoded.extend_from_slice(&[0_u8; 3]);
    encoded.extend_from_slice(&(key.len() as u32).to_le_bytes());
    encoded.extend_from_slice(&(value.len() as u32).to_le_bytes());
    encoded.extend_from_slice(key);
    encoded.extend_from_slice(value);
    let checksum = crc32fast::hash(&encoded);
    encoded.extend_from_slice(&checksum.to_le_bytes());
    encoded
}

#[test]
fn durable_transaction_replays_after_post_sync_pre_publication_crash_model() {
    let dir = tempdir().expect("tempdir");
    let path_buf = dir.path().join("readwrite-post-sync.log");
    let path = path_buf.to_str().expect("utf8 path");

    // Establish tx=1 through the executable read-write path.
    let initialized = run(path, "0");
    assert_success(&initialized);
    assert_eq!(stdout(&initialized), "counter=0");

    // Model termination after the durable append+sync boundary but before the read-write engine
    // publishes tx=2 into its process-local BTreeMap: write the exact v2 transaction record through
    // LogEngine, whose successful put has already sync_data'd the record, then drop that process
    // state without applying the mutation anywhere else.
    {
        let mut backing = LogEngine::open(path).expect("open backing log");
        let encoded = encode_single_put(2, b"counter", &1_u64.to_le_bytes());
        backing
            .put(&tx_key(2), &encoded)
            .expect("durably append tx=2");
    }

    // Reopen must treat the synced record as committed even though no prior read-write in-memory
    // publication survived. A zero-worker run is read-only after replay.
    let reopened = run(path, "0");
    assert_success(&reopened);
    assert_eq!(stdout(&reopened), "counter=1");

    // Recovery must also advance the transaction sequence exactly once: the next normal commit is
    // tx=3 and produces counter=2 rather than rejecting a discontinuity or replaying tx=2 twice.
    let continued = run(path, "1");
    assert_success(&continued);
    assert_eq!(stdout(&continued), "counter=2");

    let final_reopen = run(path, "0");
    assert_success(&final_reopen);
    assert_eq!(stdout(&final_reopen), "counter=2");
}
