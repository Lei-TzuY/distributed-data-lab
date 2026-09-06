use std::fs;
use std::io::Write;

use db_core::{
    compare_workload, generate_workload, ByteString, ErrorClass, GeneratorConfig, KvEngine,
    StorageArchitecture, Workload, WorkloadStep, MAX_KEY_BYTES, MAX_VALUE_BYTES,
    WORKLOAD_FORMAT_VERSION,
};
use db_storage_memory::MemoryEngine;
use tempfile::tempdir;

use super::wal::{
    checked_record_end, encode_record, file_name as wal_file_name, MutationKind, INITIAL_WAL_ID,
    RECORD_HEADER_LEN, WAL_HEADER_LEN,
};
use super::{LsmEngine, MUTABLE_MEMTABLE_BYTES_LIMIT};

fn bytes(value: &[u8]) -> ByteString {
    ByteString::from(value)
}

#[test]
fn common_semantics_match_reference_across_reopens() {
    let directory = tempdir().expect("temporary directory");
    let mut persistent =
        LsmEngine::create_new(directory.path().join("engine")).expect("create LSM engine");
    assert_eq!(
        persistent.capabilities().storage_architecture,
        StorageArchitecture::LsmTree
    );
    assert!(persistent.capabilities().ordered_range_scan);
    let mut reference = MemoryEngine::new();
    let workload = Workload {
        format_version: WORKLOAD_FORMAT_VERSION,
        seed: None,
        steps: vec![
            WorkloadStep::Put {
                key: bytes(b""),
                value: bytes(b""),
            },
            WorkloadStep::Put {
                key: ByteString::from(vec![0x00, 0xff, 0x80]),
                value: ByteString::from(vec![0xff, 0x00, 0x7f]),
            },
            WorkloadStep::Put {
                key: bytes(b"key"),
                value: bytes(b"one"),
            },
            WorkloadStep::Put {
                key: bytes(b"key"),
                value: bytes(b"two"),
            },
            WorkloadStep::Delete {
                key: bytes(b"missing"),
            },
            WorkloadStep::Delete { key: bytes(b"key") },
            WorkloadStep::Reopen,
            WorkloadStep::Put {
                key: bytes(b"key"),
                value: bytes(b"three"),
            },
            WorkloadStep::Reopen,
            WorkloadStep::Get { key: bytes(b"key") },
        ],
    };

    let report = compare_workload(&mut reference, &mut persistent, &workload)
        .expect("LSM engine must match oracle");
    assert_eq!(report.steps_checked, workload.steps.len());
}

#[test]
fn recorded_seed_state_machines_and_reopen_after_every_operation_match() {
    const SEEDS: &[u64] = &[0, 1, 0x5eed, 0xdead_beef, u64::MAX];
    for &seed in SEEDS {
        let directory = tempdir().expect("temporary directory");
        let workload = generate_workload(GeneratorConfig {
            seed,
            operations: 240,
            key_space: 17,
            max_value_bytes: 97,
            reopen_every: Some(if seed == 0x5eed { 1 } else { 13 }),
        })
        .expect("generate deterministic workload");
        let mut reference = MemoryEngine::new();
        let mut persistent =
            LsmEngine::create_new(directory.path().join("engine")).expect("create LSM engine");
        compare_workload(&mut reference, &mut persistent, &workload)
            .unwrap_or_else(|error| panic!("recorded seed {seed:#018x} failed: {error}"));
    }
}

#[test]
fn frozen_memtables_preserve_newest_values_tombstones_and_ordered_ranges() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    let mut reference = MemoryEngine::new();
    let large = MUTABLE_MEMTABLE_BYTES_LIMIT / 2 + 1_024;

    assert_eq!(
        engine.put(b"a", &vec![0x11; large]).expect("put a old"),
        reference
            .put(b"a", &vec![0x11; large])
            .expect("oracle put a old")
    );
    assert_eq!(
        engine.put(b"b", &vec![0x22; large]).expect("put b"),
        reference
            .put(b"b", &vec![0x22; large])
            .expect("oracle put b")
    );
    let first_flush = engine.stats().expect("stats after first flush");
    assert_eq!(first_flush.immutable_memtables, 0);
    assert_eq!(first_flush.sstables, 1);
    assert!(first_flush.durable_sequence > 0);
    assert_eq!(
        engine.put(b"a", &vec![0x33; large]).expect("put a newest"),
        reference
            .put(b"a", &vec![0x33; large])
            .expect("oracle put a newest")
    );
    assert_eq!(
        engine.delete(b"b").expect("tombstone b"),
        reference.delete(b"b").expect("oracle delete b")
    );
    assert_eq!(
        engine.put(b"c", &vec![0x44; large]).expect("put c"),
        reference
            .put(b"c", &vec![0x44; large])
            .expect("oracle put c")
    );
    assert_eq!(
        engine.put(b"d", b"tail").expect("put mutable tail"),
        reference.put(b"d", b"tail").expect("oracle put d")
    );

    let before = engine.stats().expect("stats before reopen");
    assert_eq!(before.immutable_memtables, 0);
    assert!(before.sstables >= 2);
    assert!(before.durable_sequence > 0);
    let expected = reference
        .range_scan(b"a", Some(b"e"), 16)
        .expect("oracle range");
    assert_eq!(
        engine
            .range_scan(b"a", Some(b"e"), 16)
            .expect("scan frozen tables"),
        expected
    );
    engine.reopen().expect("replay frozen tables");
    assert_eq!(engine.stats().expect("stats after reopen"), before);
    assert_eq!(engine.get(b"b").expect("read tombstone"), None);
    assert_eq!(
        engine.range_scan(b"b", None, 2).expect("limited range"),
        reference
            .range_scan(b"b", None, 2)
            .expect("oracle limited range")
    );
    assert!(engine
        .range_scan(b"same", Some(b"same"), 10)
        .expect("equal bounds")
        .is_empty());
    assert!(engine
        .range_scan(b"a", None, 0)
        .expect("zero limit")
        .is_empty());
    assert!(engine.range_scan(b"z", Some(b"a"), 1).is_err());
}

#[test]
fn boundary_sized_key_and_value_round_trip() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let key = vec![0xa5; MAX_KEY_BYTES];
    let value = vec![0x5a; MAX_VALUE_BYTES];
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine.put(&key, &value).expect("put boundary value");
    engine.reopen().expect("reopen boundary value");
    assert_eq!(engine.get(&key).expect("get boundary value"), Some(value));
}

#[test]
fn interrupted_final_record_recovers_at_structural_prefixes() {
    let directory = tempdir().expect("temporary directory");
    let complete_path = directory.path().join("complete");
    {
        let mut engine = LsmEngine::create_new(&complete_path).expect("create complete LSM");
        engine.put(b"a", b"one").expect("first put");
        engine.put(b"b", b"two").expect("second put");
    }
    let complete =
        fs::read(complete_path.join(wal_file_name(INITIAL_WAL_ID))).expect("read complete WAL");
    let first_record_len = RECORD_HEADER_LEN + b"a".len() + b"one".len();
    let second_record_offset = WAL_HEADER_LEN + first_record_len;
    let second_record_len = RECORD_HEADER_LEN + b"b".len() + b"two".len();
    let cut_deltas = [1, 4, 5, 6, 8, 12, 16, 20, 24, 27, 28, 31, 32, 33, 35];

    for delta in cut_deltas {
        assert!(delta < second_record_len);
        let path = directory.path().join(format!("cut-{delta}"));
        fs::create_dir(&path).expect("create cut directory");
        let wal_path = path.join(wal_file_name(INITIAL_WAL_ID));
        fs::write(&wal_path, &complete[..second_record_offset + delta]).expect("write WAL prefix");

        let report = LsmEngine::verify(&path).expect("partial WAL is reportable");
        assert!(report.wal.recoverable_tail.is_some(), "cut {delta}");
        assert_eq!(report.wal.valid_bytes, second_record_offset as u64);
        assert_eq!(
            fs::metadata(&wal_path)
                .expect("metadata before recovery")
                .len(),
            (second_record_offset + delta) as u64,
            "verify must not mutate cut {delta}"
        );

        let mut recovered = LsmEngine::open(&path).expect("recover partial WAL");
        assert!(recovered.recovered_tail().is_some(), "cut {delta}");
        assert_eq!(
            recovered.get(b"a").expect("get committed key"),
            Some(b"one".to_vec())
        );
        assert_eq!(recovered.get(b"b").expect("get cut key"), None);
        assert_eq!(
            fs::metadata(&wal_path)
                .expect("metadata after recovery")
                .len(),
            second_record_offset as u64
        );
    }
}

#[test]
fn complete_checksum_failure_and_unexplained_tail_fail_closed() {
    let directory = tempdir().expect("temporary directory");
    let corrupt_path = directory.path().join("corrupt");
    {
        let mut engine = LsmEngine::create_new(&corrupt_path).expect("create corrupt fixture");
        engine.put(b"key", b"value").expect("put fixture");
    }
    let wal_path = corrupt_path.join(wal_file_name(INITIAL_WAL_ID));
    let mut encoded = fs::read(&wal_path).expect("read WAL");
    *encoded.last_mut().expect("nonempty WAL") ^= 0x80;
    fs::write(&wal_path, encoded).expect("write checksum corruption");
    let error = LsmEngine::verify(&corrupt_path).expect_err("checksum corruption must fail");
    assert_eq!(error.class(), ErrorClass::Corruption);

    let tail_path = directory.path().join("garbage-tail");
    {
        let mut engine = LsmEngine::create_new(&tail_path).expect("create tail fixture");
        engine.put(b"key", b"value").expect("put tail fixture");
    }
    let mut wal = fs::OpenOptions::new()
        .append(true)
        .open(tail_path.join(wal_file_name(INITIAL_WAL_ID)))
        .expect("open tail WAL");
    wal.write_all(b"NO").expect("append unknown tail");
    wal.sync_all().expect("sync unknown tail");
    drop(wal);
    let error = LsmEngine::open(&tail_path).expect_err("unknown tail must fail");
    assert_eq!(error.class(), ErrorClass::Corruption);
}

#[test]
fn wal_header_checksum_and_unknown_version_fail_closed() {
    let directory = tempdir().expect("temporary directory");
    let checksum_path = directory.path().join("bad-header-checksum");
    {
        let engine = LsmEngine::create_new(&checksum_path).expect("create checksum fixture");
        drop(engine);
    }
    let checksum_wal = checksum_path.join(wal_file_name(INITIAL_WAL_ID));
    let mut encoded = fs::read(&checksum_wal).expect("read checksum fixture");
    encoded[32] ^= 0x80;
    fs::write(&checksum_wal, encoded).expect("write header corruption");
    assert_eq!(
        LsmEngine::open(&checksum_path)
            .expect_err("header checksum failure must fail")
            .class(),
        ErrorClass::Corruption
    );

    let version_path = directory.path().join("unknown-version");
    {
        let engine = LsmEngine::create_new(&version_path).expect("create version fixture");
        drop(engine);
    }
    let version_wal = version_path.join(wal_file_name(INITIAL_WAL_ID));
    let mut encoded = fs::read(&version_wal).expect("read version fixture");
    encoded[8..10].copy_from_slice(&2_u16.to_le_bytes());
    let checksum = crc32fast::hash(&encoded[..36]);
    encoded[36..40].copy_from_slice(&checksum.to_le_bytes());
    fs::write(&version_wal, encoded).expect("write unknown version");
    assert_eq!(
        LsmEngine::open(&version_path)
            .expect_err("unknown WAL version must fail")
            .class(),
        ErrorClass::UnsupportedVersion
    );
}

#[test]
fn absurd_lengths_and_sequence_discontinuity_fail_before_payload_allocation() {
    let directory = tempdir().expect("temporary directory");
    let absurd_path = directory.path().join("absurd");
    fs::create_dir(&absurd_path).expect("create absurd directory");
    let mut record = encode_record(MutationKind::Put, 1, b"k", b"").expect("encode record");
    record[16..20].copy_from_slice(&((MAX_KEY_BYTES as u32) + 1).to_le_bytes());
    let header_crc = crc32fast::hash(&record[..24]);
    record[24..28].copy_from_slice(&header_crc.to_le_bytes());
    let mut header = fresh_wal_header(directory.path());
    header.extend_from_slice(&record[..RECORD_HEADER_LEN]);
    fs::write(absurd_path.join(wal_file_name(INITIAL_WAL_ID)), header).expect("write absurd WAL");
    let error = LsmEngine::open(&absurd_path).expect_err("absurd key length must fail");
    assert_eq!(error.class(), ErrorClass::Corruption);
    assert!(error.to_string().contains("key length"));

    let sequence_path = directory.path().join("sequence");
    fs::create_dir(&sequence_path).expect("create sequence directory");
    let mut encoded = fresh_wal_header(directory.path());
    encoded.extend_from_slice(
        &encode_record(MutationKind::Put, 2, b"key", b"value").expect("encode sequence gap"),
    );
    fs::write(sequence_path.join(wal_file_name(INITIAL_WAL_ID)), encoded)
        .expect("write sequence gap");
    let error = LsmEngine::open(&sequence_path).expect_err("sequence gap must fail");
    assert_eq!(error.class(), ErrorClass::Corruption);
    assert!(error.to_string().contains("expected 1"));
}

#[test]
fn tombstones_are_retained_and_layout_is_fail_closed() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    assert_eq!(engine.delete(b"missing").expect("delete missing"), None);
    engine.put(b"key", b"one").expect("put one");
    engine.put(b"key", b"two").expect("put two");
    assert_eq!(
        engine.delete(b"key").expect("delete key"),
        Some(b"two".to_vec())
    );
    engine.put(b"key", b"three").expect("reinsert key");
    assert_eq!(engine.stats().expect("stats").wal_records, 5);
    engine.reopen().expect("reopen tombstones");
    assert_eq!(
        engine.get(b"key").expect("get reinserted"),
        Some(b"three".to_vec())
    );

    let empty = directory.path().join("empty");
    fs::create_dir(&empty).expect("create empty directory");
    assert_eq!(
        LsmEngine::open(&empty)
            .expect_err("missing WAL must fail")
            .class(),
        ErrorClass::Corruption
    );
    let unknown = directory.path().join("unknown");
    fs::create_dir(&unknown).expect("create unknown directory");
    fs::write(unknown.join("unexpected"), b"bytes").expect("write unknown file");
    assert_eq!(
        LsmEngine::open(&unknown)
            .expect_err("unknown file must fail")
            .class(),
        ErrorClass::Corruption
    );
    assert_eq!(
        LsmEngine::create_new(&path)
            .expect_err("existing directory must be rejected")
            .class(),
        ErrorClass::Io
    );
}

#[test]
fn checked_record_extent_rejects_integer_overflow() {
    let error = checked_record_end(u64::MAX - 3, 8).expect_err("record extent must overflow");
    assert_eq!(error.class(), ErrorClass::Corruption);
    assert!(error.to_string().contains("overflowed u64"));
}

fn fresh_wal_header(root: &std::path::Path) -> Vec<u8> {
    let path = root.join(format!("header-source-{}", unique_suffix(root)));
    let engine = LsmEngine::create_new(&path).expect("create header source");
    drop(engine);
    fs::read(path.join(wal_file_name(INITIAL_WAL_ID))).expect("read fresh WAL header")
}

fn unique_suffix(root: &std::path::Path) -> usize {
    fs::read_dir(root).expect("read fixture root").count()
}
