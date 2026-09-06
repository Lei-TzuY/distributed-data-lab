use std::fs;
use std::path::{Path, PathBuf};

use db_core::{
    DbError, ErrorClass, KvEngine, OperationalTimingInstrumented, OperationalWorkUnit,
    MAX_KEY_BYTES,
};
use tempfile::{tempdir, TempDir};

use super::{CompactionFaultMode, CompactionWriteKind, LsmEngine, MUTABLE_MEMTABLE_BYTES_LIMIT};

fn large_value(byte: u8) -> Vec<u8> {
    vec![byte; MUTABLE_MEMTABLE_BYTES_LIMIT / 2 + 1_024]
}

fn clone_engine(source: &Path, directory: &TempDir, name: &str) -> PathBuf {
    let target = directory.path().join(name);
    fs::create_dir(&target).expect("create cloned engine directory");
    for entry in fs::read_dir(source).expect("read baseline directory") {
        let entry = entry.expect("baseline directory entry");
        assert!(entry.file_type().expect("entry type").is_file());
        fs::copy(entry.path(), target.join(entry.file_name())).expect("copy baseline file");
    }
    target
}

fn put_pair(engine: &mut LsmEngine, first: u8, expected: &mut Vec<(Vec<u8>, Vec<u8>)>) {
    for offset in 0_u8..2 {
        let index = first + offset;
        let key = format!("k-{index:02}").into_bytes();
        let value = large_value(0x40 + index);
        engine.put(&key, &value).expect("populate flush pair");
        expected.push((key, value));
    }
}

fn build_three_l0_baseline(path: &Path) -> Vec<(Vec<u8>, Vec<u8>)> {
    let mut engine = LsmEngine::create_new(path).expect("create baseline LSM");
    let mut expected = Vec::new();
    engine
        .put(b"victim", &large_value(0x30))
        .expect("put value that compaction will later delete");
    let key0 = b"k-00".to_vec();
    let value0 = large_value(0x40);
    engine.put(&key0, &value0).expect("complete first L0");
    expected.push((key0, value0));

    assert!(engine.delete(b"victim").expect("delete victim").is_some());
    put_pair(&mut engine, 1, &mut expected);
    put_pair(&mut engine, 3, &mut expected);
    let stats = engine.stats().expect("three-L0 stats");
    assert_eq!(stats.level0_sstables, 3);
    assert_eq!(stats.level1_sstables, 0);
    assert_eq!(stats.durable_sequence, 7);
    assert_eq!(stats.tombstone_gc_sequence, 0);
    assert_eq!(engine.get(b"victim").expect("deleted victim"), None);
    drop(engine);
    expected
}

fn expected_compacted(kind: CompactionWriteKind, mode: CompactionFaultMode) -> bool {
    kind == CompactionWriteKind::MirrorCurrent
        || (kind == CompactionWriteKind::FirstCurrent && mode == CompactionFaultMode::AfterSync)
}

fn tombstone_key(batch: u8, index: u8) -> Vec<u8> {
    let mut key = vec![0_u8; MAX_KEY_BYTES];
    key[0] = batch;
    key[1] = index;
    key
}

fn build_three_tombstone_l0_baseline(path: &Path) {
    let mut engine = LsmEngine::create_new(path).expect("create tombstone baseline");
    for batch in 0_u8..3 {
        for index in 0_u8..16 {
            assert_eq!(
                engine
                    .delete(&tombstone_key(batch, index))
                    .expect("append missing-key tombstone"),
                None
            );
        }
    }
    let stats = engine.stats().expect("three tombstone-L0 stats");
    assert_eq!(stats.level0_sstables, 3);
    assert_eq!(stats.sstable_entries, 48);
    assert_eq!(stats.durable_sequence, 48);
    assert_eq!(stats.tombstone_gc_sequence, 0);
}

fn assert_fault_case(
    baseline: &Path,
    directory: &TempDir,
    case: usize,
    kind: CompactionWriteKind,
    mode: CompactionFaultMode,
    baseline_expected: &[(Vec<u8>, Vec<u8>)],
) {
    let path = clone_engine(baseline, directory, &format!("fault-{case}"));
    let mut engine = LsmEngine::open(&path).expect("open cloned baseline");
    engine.inject_compaction_fault_for_test(kind, mode);

    let mut expected = baseline_expected.to_vec();
    let key6 = b"k-05".to_vec();
    let value6 = large_value(0x45);
    engine
        .put(&key6, &value6)
        .expect("first fourth-L0 mutation");
    expected.push((key6, value6));
    let key7 = b"k-06".to_vec();
    let value7 = large_value(0x46);
    engine.reset_operational_timing();
    engine.set_operational_step_index(Some(41));
    let error = engine
        .put(&key7, &value7)
        .expect_err("injected compaction fault must escape the triggering mutation");
    assert!(
        matches!(error, DbError::Io(_)),
        "{kind:?} {mode:?}: {error}"
    );
    expected.push((key7, value7));

    let timing = engine.operational_timing_report();
    assert!(timing.compaction_stall_ns.is_empty(), "{kind:?} {mode:?}");
    assert!(
        timing.compaction_stall_samples.is_empty(),
        "{kind:?} {mode:?}"
    );
    assert_eq!(
        timing.compaction_stall_failure_samples.len(),
        1,
        "{kind:?} {mode:?}"
    );
    let failure = timing.compaction_stall_failure_samples[0];
    assert_eq!(failure.measured_step_index, Some(41), "{kind:?} {mode:?}");
    assert_eq!(failure.error_class, ErrorClass::Io, "{kind:?} {mode:?}");
    let work = failure
        .work
        .expect("durable-write faults happen after the complete input scan");
    assert_eq!(
        work.unit,
        OperationalWorkUnit::LsmSstableRecordVersion,
        "{kind:?} {mode:?}"
    );
    assert!(work.units_examined > 0, "{kind:?} {mode:?}");
    assert_eq!(
        work.bytes_examined,
        engine.instrumentation().compaction_input_sstable_bytes,
        "{kind:?} {mode:?}"
    );

    assert!(matches!(engine.get(b"k-00"), Err(DbError::Poisoned)));
    drop(engine);

    let mut reopened = LsmEngine::open(&path).expect("reopen injected compaction state");
    let stats = reopened.stats().expect("stats after injected reopen");
    assert_eq!(stats.durable_sequence, 9, "{kind:?} {mode:?}");
    if expected_compacted(kind, mode) {
        assert_eq!(stats.level0_sstables, 0, "{kind:?} {mode:?}");
        assert_eq!(stats.level1_sstables, 1, "{kind:?} {mode:?}");
        assert_eq!(stats.sstables, 1, "{kind:?} {mode:?}");
        assert_eq!(stats.sstable_entries, 7, "{kind:?} {mode:?}");
        assert_eq!(stats.tombstone_gc_sequence, 9, "{kind:?} {mode:?}");
        assert_eq!(
            reopened.current_entry(b"victim").expect("read GC key"),
            None,
            "{kind:?} {mode:?}"
        );
    } else {
        assert_eq!(stats.level0_sstables, 4, "{kind:?} {mode:?}");
        assert_eq!(stats.level1_sstables, 0, "{kind:?} {mode:?}");
        assert_eq!(stats.sstables, 4, "{kind:?} {mode:?}");
        assert_eq!(stats.sstable_entries, 9, "{kind:?} {mode:?}");
        assert_eq!(stats.tombstone_gc_sequence, 0, "{kind:?} {mode:?}");
        let tombstone = reopened
            .current_entry(b"victim")
            .expect("read retained tombstone")
            .expect("old input version retains deletion marker");
        assert_eq!(tombstone.sequence, 3, "{kind:?} {mode:?}");
        assert_eq!(tombstone.value, None, "{kind:?} {mode:?}");
    }
    assert_eq!(
        reopened.get(b"victim").expect("deleted key after fault"),
        None
    );
    for (key, value) in expected {
        assert_eq!(
            reopened.get(&key).expect("read after injected reopen"),
            Some(value),
            "{kind:?} {mode:?}: key {:?}",
            String::from_utf8_lossy(&key)
        );
    }
    let verified = LsmEngine::verify(&path).expect("verify injected compaction state");
    assert_eq!(verified.memtables, stats, "{kind:?} {mode:?}");
}

#[test]
fn compaction_durable_write_trace_is_stable() {
    let directory = tempdir().expect("temporary directory");
    let baseline = directory.path().join("trace-baseline");
    let _ = build_three_l0_baseline(&baseline);
    let path = clone_engine(&baseline, &directory, "trace-run");
    let mut engine = LsmEngine::open(&path).expect("open trace fixture");
    engine.begin_compaction_fault_trace_for_test();
    let mut ignored = Vec::new();
    put_pair(&mut engine, 5, &mut ignored);
    assert_eq!(
        engine.compaction_fault_trace_for_test(),
        &[
            CompactionWriteKind::L1Sstable,
            CompactionWriteKind::Manifest,
            CompactionWriteKind::FirstCurrent,
            CompactionWriteKind::MirrorCurrent,
        ]
    );
}

#[test]
fn compaction_fault_matrix_reopens_only_complete_old_or_new_version() {
    let directory = tempdir().expect("temporary directory");
    let baseline = directory.path().join("fault-baseline");
    let expected = build_three_l0_baseline(&baseline);
    let kinds = [
        CompactionWriteKind::L1Sstable,
        CompactionWriteKind::Manifest,
        CompactionWriteKind::FirstCurrent,
        CompactionWriteKind::MirrorCurrent,
    ];
    let modes = [
        CompactionFaultMode::BeforeWrite,
        CompactionFaultMode::TornWrite,
        CompactionFaultMode::AfterSync,
    ];

    let mut case = 0_usize;
    for kind in kinds {
        for mode in modes {
            assert_fault_case(&baseline, &directory, case, kind, mode, &expected);
            case += 1;
        }
    }
}

#[test]
fn tableless_compaction_fault_matrix_preserves_old_or_gc_complete_state() {
    let directory = tempdir().expect("temporary directory");
    let baseline = directory.path().join("empty-fault-baseline");
    build_three_tombstone_l0_baseline(&baseline);
    let kinds = [
        CompactionWriteKind::Manifest,
        CompactionWriteKind::FirstCurrent,
        CompactionWriteKind::MirrorCurrent,
    ];
    let modes = [
        CompactionFaultMode::BeforeWrite,
        CompactionFaultMode::TornWrite,
        CompactionFaultMode::AfterSync,
    ];

    let mut case = 0_usize;
    for kind in kinds {
        for mode in modes {
            let path = clone_engine(&baseline, &directory, &format!("empty-fault-{case}"));
            let mut engine = LsmEngine::open(&path).expect("open tombstone baseline clone");
            engine.inject_compaction_fault_for_test(kind, mode);
            for index in 0_u8..15 {
                assert_eq!(
                    engine
                        .delete(&tombstone_key(3, index))
                        .expect("append fourth-L0 tombstone"),
                    None
                );
            }
            let error = engine
                .delete(&tombstone_key(3, 15))
                .expect_err("injected table-less compaction fault must escape");
            assert!(
                matches!(error, DbError::Io(_)),
                "{kind:?} {mode:?}: {error}"
            );
            assert!(matches!(
                engine.get(&tombstone_key(0, 0)),
                Err(DbError::Poisoned)
            ));
            drop(engine);

            let mut reopened = LsmEngine::open(&path).expect("reopen table-less fault state");
            let stats = reopened.stats().expect("table-less fault stats");
            assert_eq!(stats.durable_sequence, 64, "{kind:?} {mode:?}");
            assert_eq!(
                stats.wal_records, 16,
                "a reported compaction error prevents the later WAL-rotation step"
            );
            if expected_compacted(kind, mode) {
                assert_eq!(stats.sstables, 0, "{kind:?} {mode:?}");
                assert_eq!(stats.sstable_entries, 0, "{kind:?} {mode:?}");
                assert_eq!(stats.tombstone_gc_sequence, 64, "{kind:?} {mode:?}");
            } else {
                assert_eq!(stats.level0_sstables, 4, "{kind:?} {mode:?}");
                assert_eq!(stats.sstable_entries, 64, "{kind:?} {mode:?}");
                assert_eq!(stats.tombstone_gc_sequence, 0, "{kind:?} {mode:?}");
            }
            for batch in 0_u8..4 {
                for index in 0_u8..16 {
                    let key = tombstone_key(batch, index);
                    assert_eq!(reopened.get(&key).expect("read deleted key"), None);
                    assert_eq!(
                        reopened
                            .current_entry(&key)
                            .expect("read physical deletion state")
                            .is_none(),
                        expected_compacted(kind, mode),
                        "{kind:?} {mode:?}: batch {batch}, key {index}"
                    );
                }
            }
            let verified = LsmEngine::verify(&path).expect("verify table-less fault state");
            assert_eq!(verified.memtables, stats, "{kind:?} {mode:?}");
            case += 1;
        }
    }
}
