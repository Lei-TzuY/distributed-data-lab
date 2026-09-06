use std::fs;
use std::io::Write;

use db_core::{
    compare_workload, generate_workload, ByteString, ErrorClass, GeneratorConfig, KvEngine,
    Workload, WorkloadStep, MAX_KEY_BYTES, MAX_VALUE_BYTES, WORKLOAD_FORMAT_VERSION,
};
use db_storage_memory::MemoryEngine;
use tempfile::tempdir;

use super::{
    checked_record_end, encode_file_header, encode_record, LogEngine, RecordKind, FILE_HEADER_LEN,
    RECORD_HEADER_LEN,
};

fn bytes(value: &[u8]) -> ByteString {
    ByteString::from(value)
}

#[test]
fn explicit_semantics_match_reference_across_reopens() {
    let directory = tempdir().expect("create temporary directory");
    let mut persistent = LogEngine::open(directory.path().join("engine.db")).expect("open log");
    let mut reference = MemoryEngine::new();
    let workload = Workload {
        format_version: WORKLOAD_FORMAT_VERSION,
        seed: None,
        steps: vec![
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
                value: bytes(b""),
            },
            WorkloadStep::Put {
                key: ByteString::from(vec![0x00, 0xff, 0x80]),
                value: ByteString::from(vec![0xff, 0x00, 0x7f]),
            },
            WorkloadStep::Reopen,
            WorkloadStep::Get { key: bytes(b"key") },
            WorkloadStep::Get {
                key: ByteString::from(vec![0x00, 0xff, 0x80]),
            },
        ],
    };

    let report =
        compare_workload(&mut reference, &mut persistent, &workload).expect("engines must agree");
    assert_eq!(report.steps_checked, workload.steps.len());
}

#[test]
fn recorded_seed_state_machines_match_reference() {
    const RECORDED_SEEDS: &[u64] = &[0, 1, 0x5eed, 0xdead_beef, 0x0123_4567_89ab_cdef, u64::MAX];

    for &seed in RECORDED_SEEDS {
        let directory = tempdir().expect("create temporary directory");
        let workload = generate_workload(GeneratorConfig {
            seed,
            operations: 400,
            key_space: 19,
            max_value_bytes: 96,
            reopen_every: Some(11),
        })
        .expect("generate recorded-seed workload");
        let mut reference = MemoryEngine::new();
        let mut persistent =
            LogEngine::open(directory.path().join("engine.db")).expect("open log engine");
        compare_workload(&mut reference, &mut persistent, &workload)
            .unwrap_or_else(|error| panic!("recorded seed {seed:#018x} failed: {error}"));
    }
}

#[test]
fn reopening_after_every_generated_operation_preserves_state() {
    let directory = tempdir().expect("create temporary directory");
    let workload = generate_workload(GeneratorConfig {
        seed: 0xa11c_e5ed,
        operations: 128,
        key_space: 7,
        max_value_bytes: 31,
        reopen_every: Some(1),
    })
    .expect("generate workload");
    let mut reference = MemoryEngine::new();
    let mut persistent =
        LogEngine::open(directory.path().join("engine.db")).expect("open log engine");
    compare_workload(&mut reference, &mut persistent, &workload)
        .expect("reopen-after-every-operation trace must match");
}

#[test]
fn boundary_sized_key_and_value_round_trip() {
    let directory = tempdir().expect("create temporary directory");
    let key = vec![0xa5; MAX_KEY_BYTES];
    let value = vec![0x5a; MAX_VALUE_BYTES];
    let workload = Workload {
        format_version: WORKLOAD_FORMAT_VERSION,
        seed: None,
        steps: vec![
            WorkloadStep::Put {
                key: ByteString::from(key.clone()),
                value: ByteString::from(value),
            },
            WorkloadStep::Reopen,
            WorkloadStep::Get {
                key: ByteString::from(key),
            },
        ],
    };
    let mut reference = MemoryEngine::new();
    let mut persistent =
        LogEngine::open(directory.path().join("engine.db")).expect("open log engine");
    compare_workload(&mut reference, &mut persistent, &workload)
        .expect("boundary-sized record must match");
}

#[test]
fn interrupted_append_is_recovered_at_each_structural_boundary() {
    let directory = tempdir().expect("create temporary directory");
    let complete_path = directory.path().join("complete.db");
    {
        let mut engine = LogEngine::open(&complete_path).expect("open log");
        engine.put(b"a", b"one").expect("first put");
        engine.put(b"b", b"two").expect("second put");
    }
    let complete = fs::read(&complete_path).expect("read complete log");
    let first_record_len = RECORD_HEADER_LEN + b"a".len() + b"one".len();
    let second_record_offset = FILE_HEADER_LEN + first_record_len;
    let second_record_len = RECORD_HEADER_LEN + b"b".len() + b"two".len();
    let cut_deltas = [1, 4, 5, 6, 8, 12, 16, 20, 24, 27, 28, 31, 32, 33, 35];

    for delta in cut_deltas {
        assert!(delta < second_record_len);
        let path = directory.path().join(format!("cut-{delta}.db"));
        fs::write(&path, &complete[..second_record_offset + delta]).expect("write cut log");

        let report = LogEngine::verify(&path).expect("partial append is reportable");
        assert!(report.recoverable_tail.is_some(), "cut delta {delta}");
        assert_eq!(report.valid_bytes, second_record_offset as u64);
        assert_eq!(
            fs::metadata(&path).expect("metadata before recovery").len(),
            (second_record_offset + delta) as u64,
            "verify must not mutate"
        );

        let mut recovered = LogEngine::open(&path).expect("recover partial append");
        assert!(recovered.recovered_tail().is_some(), "cut delta {delta}");
        assert_eq!(
            recovered.get(b"a").expect("get first key"),
            Some(b"one".to_vec())
        );
        assert_eq!(recovered.get(b"b").expect("get cut key"), None);
        assert_eq!(
            fs::metadata(&path).expect("metadata after recovery").len(),
            second_record_offset as u64
        );
        assert!(LogEngine::verify(&path)
            .expect("verify recovered file")
            .recoverable_tail
            .is_none());
    }
}

#[test]
fn fully_present_payload_bit_flip_fails_closed() {
    let directory = tempdir().expect("create temporary directory");
    let path = directory.path().join("bit-flip.db");
    {
        let mut engine = LogEngine::open(&path).expect("open log");
        engine.put(b"key", b"value").expect("put");
    }
    let mut encoded = fs::read(&path).expect("read log");
    let final_byte = encoded.last_mut().expect("nonempty log");
    *final_byte ^= 0x80;
    fs::write(&path, encoded).expect("write corrupt log");

    let error = LogEngine::verify(&path).expect_err("checksum failure must be rejected");
    assert_eq!(error.class(), ErrorClass::Corruption);
    assert!(LogEngine::open(&path).is_err());
}

#[test]
fn unexplained_tail_bytes_fail_closed() {
    let directory = tempdir().expect("create temporary directory");
    let path = directory.path().join("garbage-tail.db");
    {
        let mut engine = LogEngine::open(&path).expect("open log");
        engine.put(b"key", b"value").expect("put");
    }
    let mut file = fs::OpenOptions::new()
        .append(true)
        .open(&path)
        .expect("open for append");
    file.write_all(b"NO").expect("append unexplained bytes");
    file.sync_all().expect("sync corruption fixture");

    let error = LogEngine::verify(&path).expect_err("unknown tail must fail");
    assert_eq!(error.class(), ErrorClass::Corruption);
}

#[test]
fn absurd_declared_length_is_rejected_before_payload_allocation() {
    let directory = tempdir().expect("create temporary directory");
    let path = directory.path().join("absurd-length.db");
    let mut record = encode_record(RecordKind::Put, 1, b"k", b"").expect("encode record");
    record[16..20].copy_from_slice(&((MAX_KEY_BYTES as u32) + 1).to_le_bytes());
    let header_crc = crc32fast::hash(&record[..24]);
    record[24..28].copy_from_slice(&header_crc.to_le_bytes());

    let mut file_bytes = encode_file_header().to_vec();
    file_bytes.extend_from_slice(&record[..RECORD_HEADER_LEN]);
    fs::write(&path, file_bytes).expect("write absurd length fixture");

    let error = LogEngine::verify(&path).expect_err("absurd length must fail");
    assert_eq!(error.class(), ErrorClass::Corruption);
    assert!(error.to_string().contains("declared key length"));
}

#[test]
fn checked_offset_arithmetic_rejects_integer_overflow() {
    let error = checked_record_end(u64::MAX - 3, 8).expect_err("offset must overflow");
    assert_eq!(error.class(), ErrorClass::Corruption);
    assert!(error.to_string().contains("overflowed u64"));
}

#[test]
fn duplicate_records_and_tombstones_replay_last_state() {
    let directory = tempdir().expect("create temporary directory");
    let path = directory.path().join("duplicates.db");
    {
        let mut engine = LogEngine::open(&path).expect("open log");
        engine.put(b"key", b"one").expect("put one");
        engine.put(b"key", b"two").expect("put two");
        assert_eq!(engine.delete(b"missing").expect("delete missing"), None);
        assert_eq!(
            engine.delete(b"key").expect("delete key"),
            Some(b"two".to_vec())
        );
        engine.put(b"key", b"three").expect("reinsert");
    }

    let report = LogEngine::verify(&path).expect("verify duplicates");
    assert_eq!(report.record_count, 5);
    assert_eq!(report.live_keys, 1);
    let mut reopened = LogEngine::open(&path).expect("reopen");
    assert_eq!(reopened.get(b"key").expect("get"), Some(b"three".to_vec()));
    assert_eq!(reopened.get(b"missing").expect("get missing"), None);
}

#[test]
fn preexisting_empty_file_is_not_silently_reinitialized() {
    let directory = tempdir().expect("create temporary directory");
    let path = directory.path().join("empty.db");
    fs::write(&path, []).expect("create empty preexisting file");
    let error = LogEngine::open(&path).expect_err("empty existing file must fail");
    assert_eq!(error.class(), ErrorClass::Corruption);
}

#[test]
fn create_new_never_reuses_existing_state() {
    let directory = tempdir().expect("create temporary directory");
    let path = directory.path().join("exclusive.db");
    drop(LogEngine::create_new(&path).expect("create engine exactly once"));
    let error = LogEngine::create_new(&path).expect_err("existing path must be rejected");
    assert_eq!(error.class(), ErrorClass::Io);
}
