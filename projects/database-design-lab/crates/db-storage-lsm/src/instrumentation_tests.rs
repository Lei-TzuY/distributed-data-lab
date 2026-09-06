use std::fs;
use std::path::Path;

use db_core::{KvEngine, OperationalTimingInstrumented, OperationalWorkUnit, ReadWorkUnit};
use db_storage_memory::MemoryEngine;
use tempfile::tempdir;

use super::wal::RECORD_HEADER_LEN;
use super::{AmplificationRatio, LsmEngine, MUTABLE_MEMTABLE_BYTES_LIMIT};

fn large_value(byte: u8) -> Vec<u8> {
    vec![byte; MUTABLE_MEMTABLE_BYTES_LIMIT / 2 + 1_024]
}

fn fixed_key(index: u8) -> Vec<u8> {
    format!("k{index:03}").into_bytes()
}

fn canonical_sstable_bytes(path: &Path) -> u64 {
    fs::read_dir(path)
        .expect("read engine directory")
        .filter_map(Result::ok)
        .filter(|entry| {
            let name = entry.file_name();
            let name = name.to_string_lossy();
            name.starts_with("sst-") && name.ends_with(".sst")
        })
        .map(|entry| entry.metadata().expect("SSTable metadata").len())
        .sum()
}

fn assert_same_state(reference: &mut MemoryEngine, engine: &mut LsmEngine) {
    assert_eq!(
        engine.range_scan(b"", None, 128).expect("LSM full range"),
        reference
            .range_scan(b"", None, 128)
            .expect("oracle full range")
    );
}

fn paired_put(reference: &mut MemoryEngine, engine: &mut LsmEngine, key: &[u8], value: &[u8]) {
    assert_eq!(
        engine.put(key, value).expect("LSM put"),
        reference.put(key, value).expect("oracle put")
    );
}

fn paired_delete(reference: &mut MemoryEngine, engine: &mut LsmEngine, key: &[u8]) {
    assert_eq!(
        engine.delete(key).expect("LSM delete"),
        reference.delete(key).expect("oracle delete")
    );
}

#[test]
fn deterministic_full_set_compactions_match_memory_oracle_across_reopen() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    let mut reference = MemoryEngine::new();

    for index in 0_u8..8 {
        paired_put(
            &mut reference,
            &mut engine,
            &fixed_key(index),
            &large_value(0x20 + index),
        );
        if index % 2 == 1 {
            assert_same_state(&mut reference, &mut engine);
        }
    }
    let first = engine.stats().expect("first compacted stats");
    assert_eq!(first.level0_sstables, 0);
    assert_eq!(first.level1_sstables, 1);
    assert_eq!(first.sstable_entries, 8);
    engine.reopen().expect("reopen first compacted version");
    assert_same_state(&mut reference, &mut engine);

    for round in 0_u8..4 {
        paired_delete(&mut reference, &mut engine, &fixed_key(round));
        paired_put(
            &mut reference,
            &mut engine,
            &fixed_key(8 + round),
            &large_value(0x40 + round),
        );
        paired_put(
            &mut reference,
            &mut engine,
            &fixed_key(4 + round),
            &large_value(0x50 + round),
        );
        assert_same_state(&mut reference, &mut engine);
    }

    let second = engine.stats().expect("second compacted stats");
    assert_eq!(second.level0_sstables, 0);
    assert_eq!(second.level1_sstables, 1);
    assert_eq!(second.sstable_entries, 8);
    assert_eq!(second.tombstone_gc_sequence, second.durable_sequence);
    for index in 0_u8..4 {
        assert_eq!(engine.get(&fixed_key(index)).expect("deleted key"), None);
    }
    engine.reopen().expect("reopen second compacted version");
    assert_same_state(&mut reference, &mut engine);
    assert_eq!(LsmEngine::verify(&path).expect("verify").memtables, second);
}

#[test]
fn amplification_counters_match_hand_computable_compaction_and_read_trace() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine.reset_instrumentation();

    let value_len = large_value(0x11).len();
    let logical_per_put = fixed_key(0).len() + value_len;
    for index in 0_u8..8 {
        engine
            .put(&fixed_key(index), &large_value(0x60 + index))
            .expect("build four L0 flushes and compact");
    }

    let counters = engine.instrumentation();
    assert_eq!(counters.logical_mutations, 8);
    assert_eq!(counters.flushes, 4);
    assert_eq!(counters.compactions, 1);
    assert_eq!(
        counters.logical_mutation_bytes,
        u64::try_from(8 * logical_per_put).expect("logical bytes fit u64")
    );
    assert_eq!(
        counters.wal_record_bytes_written,
        u64::try_from(8 * (RECORD_HEADER_LEN + logical_per_put)).expect("WAL bytes fit u64")
    );
    assert_eq!(
        counters.compaction_input_sstable_bytes, counters.flush_sstable_bytes_written,
        "first full-set compaction consumes exactly the four flush outputs"
    );
    let physical_after_compaction = canonical_sstable_bytes(&path);
    assert_eq!(
        counters.compaction_output_sstable_bytes_written, physical_after_compaction,
        "only the replacement L1 remains authoritative after cleanup"
    );

    let report = engine.amplification_report().expect("amplification report");
    assert_eq!(
        report.data_write_bytes_per_logical_byte,
        AmplificationRatio {
            numerator: counters
                .wal_record_bytes_written
                .saturating_add(counters.flush_sstable_bytes_written)
                .saturating_add(counters.compaction_output_sstable_bytes_written),
            denominator: counters.logical_mutation_bytes,
        }
    );
    assert_eq!(
        report.primary_structure_bytes_per_live_byte,
        AmplificationRatio {
            numerator: physical_after_compaction,
            denominator: u64::try_from(8 * logical_per_put).expect("durable bytes fit u64"),
        }
    );

    engine.reset_instrumentation();
    engine
        .put(&fixed_key(0), &large_value(0x91))
        .expect("overwrite into newest L0");
    engine
        .put(&fixed_key(8), &large_value(0x92))
        .expect("flush one L0 over L1");
    let layered = engine.stats().expect("layered stats");
    assert_eq!(layered.level0_sstables, 1);
    assert_eq!(layered.level1_sstables, 1);
    assert_eq!(layered.sstable_entries, 10);

    engine.reset_instrumentation();
    assert!(engine.get(&fixed_key(0)).expect("newest L0 key").is_some());
    assert!(engine.get(&fixed_key(1)).expect("older L1 key").is_some());
    assert_eq!(engine.get(b"zzzz").expect("absent key"), None);
    let range = engine
        .range_scan(b"", None, 128)
        .expect("full layered range");
    assert_eq!(range.len(), 9);

    let counters = engine.instrumentation();
    assert_eq!(counters.point_reads, 3);
    assert_eq!(counters.point_sstable_consults, 5);
    assert_eq!(counters.range_scans, 1);
    assert_eq!(counters.range_sstable_records_decoded, 10);
    assert_eq!(counters.range_result_records, 9);
    let layered_report = engine
        .amplification_report()
        .expect("layered amplification report");
    assert_eq!(
        layered_report.point_read.ratio,
        AmplificationRatio {
            numerator: 5,
            denominator: 3,
        }
    );
    assert_eq!(
        layered_report.point_read.unit,
        ReadWorkUnit::LsmSstableConsult
    );
    assert_eq!(
        layered_report.range_read.ratio,
        AmplificationRatio {
            numerator: 10,
            denominator: 9,
        }
    );
    assert_eq!(
        layered_report.range_read.unit,
        ReadWorkUnit::LsmSstableVersionDecoded
    );
    assert_eq!(
        layered_report
            .primary_structure_bytes_per_live_byte
            .numerator,
        canonical_sstable_bytes(&path)
    );
    assert_eq!(
        layered_report
            .primary_structure_bytes_per_live_byte
            .denominator,
        u64::try_from(9 * logical_per_put).expect("layered durable bytes fit u64")
    );
}

#[test]
fn instrumentation_survives_logical_reopen_and_reset_is_state_neutral() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine.put(b"a", b"one").expect("put a");
    assert_eq!(engine.get(b"a").expect("get a"), Some(b"one".to_vec()));
    let before = engine.instrumentation();
    assert_eq!(before.logical_mutations, 1);
    assert_eq!(before.point_reads, 1);

    engine.reopen().expect("logical reopen");
    assert_eq!(engine.instrumentation(), before);
    assert_eq!(
        engine.get(b"a").expect("get after reopen"),
        Some(b"one".to_vec())
    );
    assert_eq!(engine.instrumentation().point_reads, 2);

    let logical = engine
        .range_scan(b"", None, 16)
        .expect("state before reset");
    engine.reset_instrumentation();
    assert_eq!(engine.instrumentation(), Default::default());
    assert_eq!(
        engine.range_scan(b"", None, 16).expect("state after reset"),
        logical
    );
}

#[test]
fn operational_samples_bind_compaction_and_reopen_to_deterministic_work() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("operational-work-engine");
    let mut engine = LsmEngine::create_new(&path).expect("create LSM engine");
    engine.reset_instrumentation();
    engine.reset_operational_timing();

    for index in 0_u8..7 {
        engine
            .put(&fixed_key(index), &large_value(0x20 + index))
            .expect("seed pre-trigger puts");
    }
    engine.set_operational_step_index(Some(7));
    engine
        .put(&fixed_key(7), &large_value(0x27))
        .expect("trigger first full-set compaction");
    engine.set_operational_step_index(None);

    let counters = engine.instrumentation();
    let timing = engine.operational_timing_report();
    assert_eq!(timing.compaction_stall_samples.len(), 1);
    let compaction = timing.compaction_stall_samples[0];
    assert_eq!(timing.compaction_stall_ns, vec![compaction.duration_ns]);
    assert_eq!(compaction.measured_step_index, Some(7));
    assert_eq!(
        compaction.work.unit,
        OperationalWorkUnit::LsmSstableRecordVersion
    );
    assert_eq!(compaction.work.units_examined, 8);
    assert_eq!(
        compaction.work.bytes_examined,
        counters.compaction_input_sstable_bytes
    );
    assert!(compaction.duration_ns > 0);

    let authoritative_bytes = canonical_sstable_bytes(&path);
    engine.reset_operational_timing();
    engine.set_operational_step_index(Some(99));
    KvEngine::reopen(&mut engine).expect("measured reopen");
    let timing = engine.operational_timing_report();
    assert_eq!(timing.reopen_samples.len(), 1);
    let reopen = timing.reopen_samples[0];
    assert_eq!(timing.reopen_ns, vec![reopen.duration_ns]);
    assert_eq!(reopen.measured_step_index, Some(99));
    assert_eq!(reopen.work.unit, OperationalWorkUnit::LsmRecordVersion);
    assert_eq!(
        reopen.work.units_examined, 8,
        "empty rotated WAL plus eight L1 records"
    );
    assert_eq!(
        reopen.work.bytes_examined,
        40 + authoritative_bytes,
        "WAL header plus authoritative SSTable bytes"
    );
    assert!(reopen.duration_ns > 0);
}
