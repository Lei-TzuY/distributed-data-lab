use std::fs;
use std::io::{Seek, SeekFrom, Write};

use crc32fast::Hasher;
use db_core::{DbError, KvEngine};
use db_storage_lsm::LsmEngine;
use tempfile::tempdir;

const WAL_HEADER_LEN: u64 = 40;
const RECORD_HEADER_LEN: usize = 32;

fn encode_put(sequence: u64, key: &[u8], value: &[u8]) -> Vec<u8> {
    let key_len = u32::try_from(key.len()).expect("test key length fits u32");
    let value_len = u32::try_from(value.len()).expect("test value length fits u32");
    let mut header = [0_u8; RECORD_HEADER_LEN];
    header[..4].copy_from_slice(b"LSMR");
    header[4] = 1;
    header[5] = 1;
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

    let mut encoded = Vec::with_capacity(RECORD_HEADER_LEN + key.len() + value.len());
    encoded.extend_from_slice(&header);
    encoded.extend_from_slice(key);
    encoded.extend_from_slice(value);
    encoded
}

#[test]
fn live_lsm_rejects_same_length_valid_substitution_before_next_mutation() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine
        .put(b"base", b"one")
        .expect("persist baseline mutation");

    let wal_path = path.join("wal-0000000000000001.log");
    let original_len = fs::metadata(&wal_path).expect("stat active WAL").len();
    let replacement = encode_put(1, b"evil", b"two");
    let mut backing = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(&wal_path)
        .expect("open active WAL externally");
    backing
        .seek(SeekFrom::Start(WAL_HEADER_LEN))
        .expect("seek to first WAL record");
    backing
        .write_all(&replacement)
        .expect("replace first WAL record with CRC-valid same-length record");
    backing.sync_data().expect("sync substituted WAL bytes");
    drop(backing);
    assert_eq!(
        fs::metadata(&wal_path).expect("stat substituted WAL").len(),
        original_len,
        "regression requires unchanged physical EOF"
    );
    let drifted = fs::read(&wal_path).expect("capture substituted WAL");

    let error = engine
        .put(b"ours", b"three")
        .expect_err("live engine must reject same-length acknowledged-prefix substitution");
    assert!(matches!(error, DbError::Corruption { .. }));
    assert_eq!(
        fs::read(&wal_path).expect("read WAL after rejected mutation"),
        drifted,
        "rejection must not mutate substituted WAL"
    );
    assert!(matches!(engine.get(b"base"), Err(DbError::Poisoned)));
}

#[test]
fn live_lsm_rejects_external_truncation_below_acknowledged_boundary() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine
        .put(b"base", b"one")
        .expect("persist baseline mutation");

    let wal_path = path.join("wal-0000000000000001.log");
    let acknowledged_len = fs::metadata(&wal_path).expect("stat active WAL").len();
    assert!(
        acknowledged_len > WAL_HEADER_LEN,
        "baseline mutation must extend the WAL beyond its header"
    );

    let backing = fs::OpenOptions::new()
        .write(true)
        .open(&wal_path)
        .expect("open active WAL externally");
    backing
        .set_len(WAL_HEADER_LEN)
        .expect("truncate active WAL below acknowledged boundary");
    backing.sync_data().expect("sync truncated WAL length");
    drop(backing);
    let drifted = fs::read(&wal_path).expect("capture truncated WAL");

    let error = engine
        .put(b"ours", b"two")
        .expect_err("live engine must reject truncation below acknowledged boundary");
    assert!(matches!(error, DbError::Corruption { .. }));
    assert_eq!(
        fs::read(&wal_path).expect("read WAL after rejected mutation"),
        drifted,
        "rejection must not repair or append to externally truncated WAL"
    );
    assert!(matches!(engine.get(b"base"), Err(DbError::Poisoned)));
}

#[cfg(unix)]
#[test]
fn live_lsm_rejects_path_replacement_before_next_mutation() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine
        .put(b"base", b"one")
        .expect("persist baseline mutation");

    let wal_path = path.join("wal-0000000000000001.log");
    let replacement_path = path.join("replacement.log");
    fs::copy(&wal_path, &replacement_path).expect("copy WAL into replacement inode");
    fs::rename(&replacement_path, &wal_path).expect("atomically replace active WAL path");
    let replacement = fs::read(&wal_path).expect("capture replacement WAL");

    let error = engine
        .put(b"ours", b"two")
        .expect_err("live engine must reject replacement of its active WAL path");
    assert!(matches!(error, DbError::Corruption { .. }));
    assert_eq!(
        fs::read(&wal_path).expect("read replacement WAL after rejected mutation"),
        replacement,
        "rejection must not append through an unlinked stale WAL handle"
    );
    assert!(matches!(engine.get(b"base"), Err(DbError::Poisoned)));
}

#[cfg(unix)]
#[test]
fn live_lsm_rejects_authoritative_wal_path_changed_to_symlink() {
    use std::os::unix::fs::symlink;

    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine
        .put(b"base", b"one")
        .expect("persist baseline mutation");

    let wal_path = path.join("wal-0000000000000001.log");
    let backing_path = path.join("wal-backing.log");
    fs::rename(&wal_path, &backing_path).expect("move authoritative WAL to backing pathname");
    symlink(&backing_path, &wal_path).expect("replace authoritative WAL pathname with symlink");
    let before = fs::read(&backing_path).expect("capture durable WAL before rejected mutation");

    let error = engine
        .put(b"ours", b"two")
        .expect_err("live engine must reject authoritative WAL pathname becoming a symlink");
    assert!(matches!(error, DbError::Corruption { .. }));
    assert_eq!(
        fs::read(&backing_path).expect("read backing WAL after rejected mutation"),
        before,
        "rejection must not append through a symlinked authoritative WAL pathname"
    );
    assert!(matches!(engine.get(b"base"), Err(DbError::Poisoned)));
}

#[cfg(unix)]
#[test]
fn lsm_open_rejects_symlinked_engine_root() {
    use std::os::unix::fs::symlink;

    let directory = tempdir().expect("temporary directory");
    let real_path = directory.path().join("engine-real");
    {
        let mut engine = LsmEngine::create_new(&real_path).expect("create LSM engine");
        engine
            .put(b"base", b"one")
            .expect("persist baseline mutation");
    }

    let alias_path = directory.path().join("engine-alias");
    symlink(&real_path, &alias_path).expect("create symlinked engine root");

    let error = LsmEngine::open(&alias_path)
        .expect_err("opening through a symlinked authoritative engine root must fail closed");
    assert!(matches!(error, DbError::Corruption { .. }));
}

#[cfg(unix)]
#[test]
fn live_lsm_rejects_engine_root_changed_to_symlink_before_next_mutation() {
    use std::os::unix::fs::symlink;

    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let moved_path = directory.path().join("engine-moved");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine
        .put(b"base", b"one")
        .expect("persist baseline mutation");

    fs::rename(&path, &moved_path).expect("move authoritative engine directory");
    symlink(&moved_path, &path).expect("replace engine root pathname with symlink");
    let wal_path = moved_path.join("wal-0000000000000001.log");
    let before = fs::read(&wal_path).expect("capture durable WAL before rejected mutation");

    let error = engine
        .put(b"ours", b"two")
        .expect_err("live engine must reject authoritative root pathname becoming a symlink");
    assert!(matches!(error, DbError::Corruption { .. }));
    assert_eq!(
        fs::read(&wal_path).expect("read WAL after rejected mutation"),
        before,
        "rejection must not append through a symlinked authoritative engine root"
    );
    assert!(matches!(engine.get(b"base"), Err(DbError::Poisoned)));
}
