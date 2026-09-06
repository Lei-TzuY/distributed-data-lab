use std::collections::BTreeMap;
use std::fs::{self, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use db_core::{KvEngine, MAX_VALUE_BYTES};
use tempfile::tempdir;

use super::manifest::{CURRENT_FILE_NAME, CURRENT_SLOT_BYTES};
use super::memtable::VersionedEntry;
use super::sstable::SsTable;
use super::{LsmEngine, MUTABLE_MEMTABLE_BYTES_LIMIT};

fn large_value(byte: u8) -> Vec<u8> {
    vec![byte; MUTABLE_MEMTABLE_BYTES_LIMIT / 2 + 1_024]
}

fn flip_byte(path: &Path, offset: u64) {
    let mut file = OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .expect("open file to corrupt");
    file.seek(SeekFrom::Start(offset))
        .expect("seek corruption byte");
    let mut byte = [0_u8; 1];
    file.read_exact(&mut byte).expect("read corruption byte");
    byte[0] ^= 0x5a;
    file.seek(SeekFrom::Start(offset))
        .expect("rewind corruption byte");
    file.write_all(&byte).expect("write corruption byte");
    file.sync_all().expect("sync corruption byte");
}

fn numbered_file(directory: &Path, prefix: &str, suffix: &str, id: u64) -> PathBuf {
    directory.join(format!("{prefix}{id:016}{suffix}"))
}

#[test]
fn published_sstable_plus_active_wal_tail_reopens_without_duplicate_application() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    let first = large_value(0x11);
    let second = large_value(0x22);

    engine.put(b"a", &first).expect("put a");
    engine.put(b"b", &second).expect("put b and flush");
    engine.put(b"tail", b"mutable").expect("put WAL tail");

    let before = engine.stats().expect("stats before reopen");
    assert_eq!(before.sstables, 1);
    assert_eq!(before.immutable_memtables, 0);
    assert_eq!(before.durable_sequence, 2);
    assert_eq!(before.active_wal_id, 2);
    assert_eq!(before.active_wal_first_sequence, 3);
    assert_eq!(before.wal_records, 1);
    assert_eq!(before.mutable_entries, 1);

    engine
        .reopen()
        .expect("reopen published SSTable plus WAL tail");
    assert_eq!(engine.stats().expect("stats after reopen"), before);
    assert_eq!(engine.get(b"a").expect("get a"), Some(first));
    assert_eq!(engine.get(b"b").expect("get b"), Some(second));
    assert_eq!(
        engine.get(b"tail").expect("get mutable tail"),
        Some(b"mutable".to_vec())
    );
}

#[test]
fn maximum_value_flushes_to_sstable_and_reopens() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let value = vec![0xa5; MAX_VALUE_BYTES];
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");

    engine.put(b"max", &value).expect("put maximum value");
    let stats = engine.stats().expect("stats after maximum value");
    assert_eq!(stats.sstables, 1);
    assert_eq!(stats.durable_sequence, 1);
    assert_eq!(stats.mutable_entries, 0);
    assert_eq!(stats.wal_records, 0);
    assert_eq!(stats.active_wal_id, 2);
    assert_eq!(stats.active_wal_first_sequence, 2);

    engine.reopen().expect("reopen maximum value");
    assert_eq!(engine.get(b"max").expect("get maximum value"), Some(value));
}

#[test]
fn referenced_sstable_corruption_fails_closed_after_wal_reclamation() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    {
        let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
        engine.put(b"a", &large_value(0x31)).expect("put a");
        engine
            .put(b"b", &large_value(0x32))
            .expect("put b and flush");
        assert_eq!(engine.stats().expect("stats").sstables, 1);
    }

    flip_byte(&numbered_file(&path, "sst-", ".sst", 1), 96);
    let error = LsmEngine::open(&path).expect_err("referenced SSTable corruption must fail");
    assert!(error.to_string().contains("corrupt"));
    let verify_error = LsmEngine::verify(&path).expect_err("verify must reject corrupted SSTable");
    assert!(verify_error.to_string().contains("corrupt"));
}

#[test]
fn referenced_sstable_bloom_corruption_fails_closed() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    {
        let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
        engine.put(b"a", &large_value(0x35)).expect("put a");
        engine
            .put(b"b", &large_value(0x36))
            .expect("put b and flush Bloom-backed SSTable");
        assert_eq!(engine.stats().expect("stats").sstables, 1);
    }

    // SSTable v2: 64-byte file header + 40-byte Bloom header, so byte 105 lies in the bit payload.
    flip_byte(&numbered_file(&path, "sst-", ".sst", 1), 105);
    let error = LsmEngine::open(&path).expect_err("Bloom corruption must fail closed");
    assert!(error.to_string().contains("corrupt"));
    let verify_error = LsmEngine::verify(&path).expect_err("verify must reject Bloom corruption");
    assert!(verify_error.to_string().contains("corrupt"));
}

#[test]
fn torn_latest_current_slot_after_rotation_uses_same_manifest_and_reclaimed_wal() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let first = large_value(0x41);
    let second = large_value(0x42);
    {
        let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
        engine.put(b"a", &first).expect("put a");
        engine
            .put(b"b", &second)
            .expect("put b and publish first SSTable");
        let stats = engine.stats().expect("published stats");
        assert_eq!(stats.durable_sequence, 2);
        assert_eq!(stats.active_wal_id, 2);
        assert_eq!(stats.wal_records, 0);
    }

    // Rotation publishes the same new manifest to generations 2 and 3. Damage generation 3 in slot 1;
    // generation 2 still references the same SSTable set and WAL 2 after WAL 1 has been reclaimed.
    flip_byte(
        &path.join(CURRENT_FILE_NAME),
        u64::try_from(CURRENT_SLOT_BYTES + 100).expect("CURRENT offset fits u64"),
    );

    let mut reopened = LsmEngine::open(&path).expect("fallback to mirrored rotated CURRENT");
    let stats = reopened.stats().expect("fallback stats");
    assert_eq!(stats.sstables, 1);
    assert_eq!(stats.durable_sequence, 2);
    assert_eq!(stats.active_wal_id, 2);
    assert_eq!(stats.active_wal_first_sequence, 3);
    assert_eq!(stats.wal_records, 0);
    assert_eq!(
        reopened.get(b"a").expect("get a after fallback"),
        Some(first)
    );
    assert_eq!(
        reopened.get(b"b").expect("get b after fallback"),
        Some(second)
    );
}

#[test]
fn authoritative_manifest_corruption_fails_closed() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    {
        let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
        engine.put(b"a", &large_value(0x51)).expect("put a");
        engine
            .put(b"b", &large_value(0x52))
            .expect("put b and flush");
        assert_eq!(engine.stats().expect("stats").active_wal_id, 2);
    }

    // Manifest 2 publishes the SSTable; manifest 3 republishes the same table set bound to WAL 2 and is
    // mirrored into both CURRENT slots before WAL 1 is removed.
    flip_byte(&numbered_file(&path, "MANIFEST-", "", 3), 70);
    let error = LsmEngine::open(&path).expect_err("authoritative manifest corruption must fail");
    assert!(error.to_string().contains("corrupt"));
}

#[test]
fn canonical_orphans_are_ignored_and_ids_skip_past_them() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let first = large_value(0x61);
    let second = large_value(0x62);
    {
        let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
        engine.put(b"a", &first).expect("put a");
        engine.put(b"b", &second).expect("put b and first flush");
    }

    fs::write(numbered_file(&path, "sst-", ".sst", 99), b"orphan sstable")
        .expect("write orphan SSTable");
    fs::write(
        numbered_file(&path, "MANIFEST-", "", 99),
        b"orphan manifest",
    )
    .expect("write orphan manifest");

    let mut reopened = LsmEngine::open(&path).expect("ignore unreferenced canonical orphans");
    assert_eq!(reopened.get(b"a").expect("get a"), Some(first));
    assert_eq!(reopened.get(b"b").expect("get b"), Some(second));

    reopened
        .put(b"c", &large_value(0x63))
        .expect("put c after orphan discovery");
    reopened
        .put(b"d", &large_value(0x64))
        .expect("put d and publish next SSTable");
    assert!(numbered_file(&path, "sst-", ".sst", 100).exists());
    assert_eq!(
        fs::read(numbered_file(&path, "sst-", ".sst", 99)).expect("read reserved orphan"),
        b"orphan sstable",
        "future allocation must not overwrite an ambiguous canonical orphan id"
    );
    reopened.reopen().expect("reopen after skipping orphan ids");
    assert!(reopened.stats().expect("final stats").sstables >= 2);
}

#[test]
fn sstable_v2_embeds_bloom_without_false_negatives_for_values_or_tombstones() {
    let directory = tempdir().expect("temporary directory");
    let mut entries = BTreeMap::new();
    for sequence in 1_u64..=512 {
        let key = format!("key-{sequence:04}").into_bytes();
        let value = (sequence % 7 != 0).then(|| sequence.to_le_bytes().to_vec());
        entries.insert(key, VersionedEntry { sequence, value });
    }
    let table = SsTable::create_new(directory.path(), 1, 512, &entries).expect("create SSTable v2");
    assert_eq!(table.format_version(), 2);
    for key in entries.keys() {
        assert_eq!(table.bloom_may_contain(key), Some(true));
        assert_eq!(
            table.get(key).expect("point read"),
            entries.get(key).cloned()
        );
    }

    let absent = (0_u64..10_000)
        .map(|value| format!("absent-{value:05}").into_bytes())
        .find(|key| table.bloom_may_contain(key) == Some(false))
        .expect("Bloom filter must reject at least one deterministic absent key");
    assert_eq!(table.get(&absent).expect("Bloom-negative read"), None);
}

#[test]
fn legacy_sstable_v1_remains_readable_without_a_filter() {
    let directory = tempdir().expect("temporary directory");
    let mut entries = BTreeMap::new();
    entries.insert(
        b"alpha".to_vec(),
        VersionedEntry {
            sequence: 1,
            value: Some(b"one".to_vec()),
        },
    );
    entries.insert(
        b"tombstone".to_vec(),
        VersionedEntry {
            sequence: 2,
            value: None,
        },
    );
    let table = SsTable::create_legacy_v1_for_test(directory.path(), 7, 2, &entries)
        .expect("create/read legacy SSTable v1");
    assert_eq!(table.format_version(), 1);
    assert_eq!(table.bloom_may_contain(b"alpha"), None);
    assert_eq!(
        table.get(b"alpha").expect("legacy point read"),
        entries.get(b"alpha".as_slice()).cloned()
    );
    assert_eq!(
        table.get(b"tombstone").expect("legacy tombstone read"),
        entries.get(b"tombstone".as_slice()).cloned()
    );
}
