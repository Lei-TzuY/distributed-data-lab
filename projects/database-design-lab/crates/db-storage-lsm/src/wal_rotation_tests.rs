use std::fs;
use std::io::Write;

use db_core::{DbError, KvEngine};
use tempfile::tempdir;

use super::manifest::{CURRENT_FILE_NAME, CURRENT_SLOT_BYTES};
use super::wal::{
    encode_record, file_name as wal_file_name, MutationKind, Wal, INITIAL_FIRST_SEQUENCE,
    INITIAL_WAL_ID,
};
use super::{LsmEngine, MUTABLE_MEMTABLE_BYTES_LIMIT};

fn large_value(byte: u8) -> Vec<u8> {
    vec![byte; MUTABLE_MEMTABLE_BYTES_LIMIT / 2 + 1_024]
}

fn read_u64(bytes: &[u8]) -> u64 {
    u64::from_le_bytes(bytes.try_into().expect("fixed u64 slice"))
}

fn write_v1_empty_manifest(path: &std::path::Path) {
    let mut bytes = vec![0_u8; 64];
    bytes[0..8].copy_from_slice(b"DBLSMMAN");
    bytes[8..10].copy_from_slice(&1_u16.to_le_bytes());
    bytes[10..12].copy_from_slice(&64_u16.to_le_bytes());
    bytes[16..24].copy_from_slice(&1_u64.to_le_bytes());
    let header_crc = crc32fast::hash(&bytes[..60]);
    bytes[60..64].copy_from_slice(&header_crc.to_le_bytes());
    let file_crc = crc32fast::hash(&bytes);
    bytes.extend_from_slice(&file_crc.to_le_bytes());
    fs::write(path.join("MANIFEST-0000000000000001"), bytes).expect("write v1 manifest");
}

#[test]
fn manifest_v1_reopens_with_implicit_initial_wal_and_upgrades_on_first_flush() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let engine = LsmEngine::create_new(&path).expect("create LSM engine");
    drop(engine);
    write_v1_empty_manifest(&path);

    let mut reopened = LsmEngine::open(&path).expect("open v1 manifest");
    let legacy = reopened.stats().expect("v1 manifest stats");
    assert_eq!(legacy.active_wal_id, 1);
    assert_eq!(legacy.active_wal_first_sequence, 1);
    assert_eq!(legacy.durable_sequence, 0);
    assert_eq!(legacy.wal_records, 0);

    reopened.put(b"a", &large_value(0x01)).expect("put a");
    reopened
        .put(b"b", &large_value(0x02))
        .expect("flush and upgrade manifest");
    let upgraded = reopened.stats().expect("upgraded stats");
    assert_eq!(upgraded.active_wal_id, 2);
    assert_eq!(upgraded.active_wal_first_sequence, 3);
    assert_eq!(upgraded.durable_sequence, 2);
    assert!(!path.join(wal_file_name(1)).exists());
    assert!(path.join("MANIFEST-0000000000000003").exists());
    reopened.reopen().expect("reopen upgraded state");
    assert_eq!(reopened.get(b"a").expect("get a"), Some(large_value(0x01)));
    assert_eq!(reopened.get(b"b").expect("get b"), Some(large_value(0x02)));
}

#[test]
fn fully_flushed_segment_rotates_and_reclaims_only_after_both_current_mirrors_move() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    let first = large_value(0x11);
    let second = large_value(0x22);

    engine.put(b"a", &first).expect("put a");
    engine.put(b"b", &second).expect("put b and flush");

    let stats = engine.stats().expect("stats after rotation");
    assert_eq!(stats.durable_sequence, 2);
    assert_eq!(stats.active_wal_id, 2);
    assert_eq!(stats.active_wal_first_sequence, 3);
    assert_eq!(stats.wal_records, 0);
    assert!(!path.join(wal_file_name(1)).exists());
    assert!(path.join(wal_file_name(2)).is_file());

    let current = fs::read(path.join(CURRENT_FILE_NAME)).expect("read CURRENT");
    assert_eq!(current.len(), CURRENT_SLOT_BYTES * 2);
    let slot0 = &current[..CURRENT_SLOT_BYTES];
    let slot1 = &current[CURRENT_SLOT_BYTES..];
    assert_eq!(read_u64(&slot0[16..24]), 2);
    assert_eq!(read_u64(&slot1[16..24]), 3);
    assert_eq!(read_u64(&slot0[24..32]), 3);
    assert_eq!(read_u64(&slot1[24..32]), 3);

    let manifest = fs::read(path.join("MANIFEST-0000000000000003")).expect("read rotated manifest");
    assert_eq!(
        u16::from_le_bytes(manifest[8..10].try_into().expect("version")),
        5
    );
    assert_eq!(read_u64(&manifest[48..56]), 2);
    assert_eq!(read_u64(&manifest[56..64]), 3);
    assert_eq!(read_u64(&manifest[64..72]), 0);

    let verify = LsmEngine::verify(&path).expect("verify rotated engine");
    assert_eq!(verify.wal.wal_id, 2);
    assert_eq!(verify.wal.first_sequence, 3);
    assert_eq!(verify.wal.record_count, 0);

    engine.put(b"tail", b"v").expect("append into new WAL");
    engine.reopen().expect("reopen rotated engine");
    assert_eq!(engine.get(b"a").expect("get a"), Some(first));
    assert_eq!(engine.get(b"b").expect("get b"), Some(second));
    assert_eq!(engine.get(b"tail").expect("get tail"), Some(b"v".to_vec()));
}

#[test]
fn replayed_frozen_prefix_does_not_reclaim_wal_while_newer_mutable_tail_depends_on_it() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    fs::create_dir(&path).expect("create legacy engine directory");
    let mut wal = Wal::create_new(
        &path.join(wal_file_name(INITIAL_WAL_ID)),
        INITIAL_WAL_ID,
        INITIAL_FIRST_SEQUENCE,
    )
    .expect("create legacy WAL");
    let first = large_value(0x31);
    let second = large_value(0x32);
    wal.append(MutationKind::Put, b"a", &first)
        .expect("append a");
    wal.append(MutationKind::Put, b"b", &second)
        .expect("append b and cross freeze threshold on replay");
    wal.append(MutationKind::Put, b"tail", b"three")
        .expect("append mutable tail");
    drop(wal);

    let mut engine = LsmEngine::open(&path).expect("open exact legacy WAL-only layout");
    assert_eq!(
        engine
            .stats()
            .expect("legacy replay stats")
            .immutable_memtables,
        1
    );
    engine
        .put(b"tail-2", b"four")
        .expect("flush replayed frozen prefix");

    let after_prefix_flush = engine.stats().expect("stats after prefix flush");
    assert_eq!(after_prefix_flush.durable_sequence, 2);
    assert_eq!(after_prefix_flush.active_wal_id, 1);
    assert_eq!(after_prefix_flush.active_wal_first_sequence, 1);
    assert_eq!(after_prefix_flush.wal_records, 4);
    assert!(path.join(wal_file_name(1)).exists());
    assert_eq!(
        engine.get(b"tail").expect("tail survives"),
        Some(b"three".to_vec())
    );
    assert_eq!(
        engine.get(b"tail-2").expect("second tail survives"),
        Some(b"four".to_vec())
    );
    engine.reopen().expect("reopen with retained WAL tail");
    assert_eq!(engine.get(b"a").expect("get a"), Some(first));
    assert_eq!(engine.get(b"b").expect("get b"), Some(second));
    assert_eq!(
        engine.get(b"tail").expect("get tail"),
        Some(b"three".to_vec())
    );

    engine
        .put(b"c", &large_value(0x33))
        .expect("grow mutable tail c");
    engine
        .put(b"d", &large_value(0x34))
        .expect("freeze tail and catch durable watermark up");
    let after_catchup = engine.stats().expect("stats after catchup rotation");
    assert_eq!(after_catchup.active_wal_id, 2);
    assert_eq!(after_catchup.active_wal_first_sequence, 7);
    assert_eq!(after_catchup.wal_records, 0);
    assert!(!path.join(wal_file_name(1)).exists());
    assert!(path.join(wal_file_name(2)).exists());
    engine.reopen().expect("reopen after catchup rotation");
    assert_eq!(
        engine.get(b"tail-2").expect("get tail-2"),
        Some(b"four".to_vec())
    );
}

#[test]
fn canonical_orphan_wal_is_ignored_then_reclaimed_and_next_id_skips_it() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let engine = LsmEngine::create_new(&path).expect("create LSM engine");
    drop(engine);

    let orphan = Wal::create_new(&path.join(wal_file_name(99)), 99, 1).expect("create orphan WAL");
    drop(orphan);

    let mut reopened = LsmEngine::open(&path).expect("ignore unreferenced canonical WAL");
    reopened.put(b"a", &large_value(0x51)).expect("put a");
    reopened
        .put(b"b", &large_value(0x52))
        .expect("put b and rotate");
    let stats = reopened
        .stats()
        .expect("stats after orphan-skipping rotation");
    assert_eq!(stats.active_wal_id, 100);
    assert_eq!(stats.active_wal_first_sequence, 3);
    assert!(path.join(wal_file_name(100)).exists());
    assert!(!path.join(wal_file_name(99)).exists());
    assert!(!path.join(wal_file_name(1)).exists());
    reopened.reopen().expect("reopen WAL 100");
    assert_eq!(reopened.get(b"a").expect("get a"), Some(large_value(0x51)));
    assert_eq!(reopened.get(b"b").expect("get b"), Some(large_value(0x52)));
}

#[test]
fn live_lsm_rejects_external_append_before_next_mutation() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine
        .put(b"base", b"one")
        .expect("persist baseline mutation");

    let wal_path = path.join(wal_file_name(INITIAL_WAL_ID));
    let foreign = encode_record(MutationKind::Put, 2, b"foreign", b"two")
        .expect("encode valid external WAL record");
    let mut backing = fs::OpenOptions::new()
        .append(true)
        .open(&wal_path)
        .expect("open active WAL externally");
    backing
        .write_all(&foreign)
        .expect("append valid external WAL record");
    backing.sync_data().expect("sync external WAL append");
    drop(backing);
    let drifted = fs::read(&wal_path).expect("capture externally drifted WAL");

    let error = engine
        .put(b"ours", b"three")
        .expect_err("live engine must reject external WAL append drift");
    assert!(matches!(error, DbError::Corruption { .. }));
    assert_eq!(
        fs::read(&wal_path).expect("read WAL after rejected mutation"),
        drifted,
        "rejection must not mutate externally drifted WAL"
    );
    assert!(matches!(engine.get(b"base"), Err(DbError::Poisoned)));
}
