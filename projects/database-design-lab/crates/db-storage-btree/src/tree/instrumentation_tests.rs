use db_core::{
    validate_experiment_compatibility, AmplificationInstrumented, ConcurrencyMode, CrashRecovery,
    DistributionMode, EngineCapabilities, KvEngine, LogicalModel, OperationalTimingInstrumented,
    OperationalWorkUnit, Persistence, ReadWorkUnit, StorageArchitecture,
};
use tempfile::tempdir;

use super::BPlusTree;
use crate::PAGE_SIZE;

#[test]
fn one_leaf_trace_has_hand_computable_read_write_and_space_ratios() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("one-leaf-amplification.db");
    let mut tree = BPlusTree::create_new(&path, 2).expect("create tree");
    tree.put(b"a", b"1").expect("seed a");
    tree.put(b"b", b"2").expect("seed b");
    assert_eq!(
        tree.data_page_count(),
        2,
        "second COW leaf appends before reuse exists"
    );

    tree.reset_instrumentation();
    assert_eq!(
        tree.put(b"a", b"xx").expect("overwrite a"),
        Some(b"1".to_vec())
    );
    assert_eq!(tree.delete(b"z").expect("missing delete"), None);
    assert_eq!(tree.get(b"a").expect("point read"), Some(b"xx".to_vec()));
    let rows = tree.range_scan(b"", None, 8).expect("full range");
    assert_eq!(
        rows,
        vec![
            (b"a".to_vec(), b"xx".to_vec()),
            (b"b".to_vec(), b"2".to_vec())
        ]
    );

    let counters = tree.instrumentation();
    assert_eq!(counters.logical_mutations, 2);
    assert_eq!(
        counters.logical_mutation_bytes, 4,
        "PUT a/xx = 3 bytes; DELETE z = 1"
    );
    assert_eq!(
        counters.data_page_bytes_written, PAGE_SIZE as u64,
        "one recycled leaf image"
    );
    assert_eq!(counters.point_reads, 1);
    assert_eq!(counters.point_page_accesses, 1);
    assert_eq!(counters.range_scans, 1);
    assert_eq!(counters.range_page_accesses, 1);
    assert_eq!(counters.range_result_records, 2);

    let report = tree.amplification_report().expect("report");
    assert_eq!(report.point_read.unit, ReadWorkUnit::BtreePageAccess);
    assert_eq!(report.point_read.ratio.numerator, 1);
    assert_eq!(report.point_read.ratio.denominator, 1);
    assert_eq!(report.range_read.unit, ReadWorkUnit::BtreePageAccess);
    assert_eq!(report.range_read.ratio.numerator, 1);
    assert_eq!(report.range_read.ratio.denominator, 2);
    assert_eq!(
        report.data_write_bytes_per_logical_byte.numerator,
        PAGE_SIZE as u64
    );
    assert_eq!(report.data_write_bytes_per_logical_byte.denominator, 4);
    assert_eq!(
        report.primary_structure_bytes_per_live_byte.numerator,
        2 * PAGE_SIZE as u64
    );
    assert_eq!(
        report.primary_structure_bytes_per_live_byte.denominator, 5,
        "a+xx and b+2"
    );
    assert_eq!(
        tree.instrumentation(),
        counters,
        "report reconstruction must not pollute counters"
    );

    KvEngine::reopen(&mut tree).expect("logical reopen");
    assert_eq!(
        tree.instrumentation(),
        counters,
        "same-handle reopen preserves measurement window"
    );
}

#[test]
fn overflow_value_pages_are_counted_as_structural_read_work() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("overflow-read-amplification.db");
    let mut tree = BPlusTree::create_new(&path, 2).expect("create tree");
    let value = vec![0x5a; 8_192];
    tree.put(b"k", &value).expect("put overflow value");
    tree.reset_instrumentation();

    assert_eq!(tree.get(b"k").expect("get overflow"), Some(value.clone()));
    let after_get = tree.instrumentation();
    assert_eq!(after_get.point_reads, 1);
    assert_eq!(
        after_get.point_page_accesses, 4,
        "leaf plus three 4,048-byte overflow chunks"
    );

    let rows = tree.range_scan(b"k", None, 1).expect("scan overflow");
    assert_eq!(rows, vec![(b"k".to_vec(), value)]);
    let after_scan = tree.instrumentation();
    assert_eq!(after_scan.range_scans, 1);
    assert_eq!(after_scan.range_page_accesses, 4);
    assert_eq!(after_scan.range_result_records, 1);
}

#[test]
fn phase4_preflight_allows_architecture_and_recovery_to_differ_but_not_semantics() {
    let btree = EngineCapabilities {
        name: "btree",
        logical_model: LogicalModel::KeyValue,
        storage_architecture: StorageArchitecture::BPlusTree,
        concurrency: ConcurrencyMode::CallerSerialized,
        persistence: Persistence::Persistent,
        crash_recovery: CrashRecovery::MirroredCopyOnWritePages,
        distribution: DistributionMode::Standalone,
        ordered_range_scan: true,
        max_key_bytes: 4 * 1024,
        max_value_bytes: 1024 * 1024,
    };
    let mut lsm = btree;
    lsm.name = "lsm";
    lsm.storage_architecture = StorageArchitecture::LsmTree;
    lsm.crash_recovery = CrashRecovery::WriteAheadLogReplay;
    validate_experiment_compatibility(btree, lsm, true).expect("architectures should compare");

    lsm.max_value_bytes -= 1;
    let error = validate_experiment_compatibility(btree, lsm, true)
        .expect_err("different common value bound must fail preflight");
    assert!(error.to_string().contains("max_value_bytes"));

    let mut no_range = btree;
    no_range.ordered_range_scan = false;
    assert!(validate_experiment_compatibility(btree, no_range, true).is_err());
}

#[test]
fn common_amplification_trait_uses_the_same_report_shape() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("trait-report.db");
    let mut tree = BPlusTree::create_new(&path, 2).expect("create tree");
    tree.put(b"key", b"value").expect("put");
    AmplificationInstrumented::reset_amplification(&mut tree);
    tree.get(b"key").expect("get");
    let report = AmplificationInstrumented::amplification_report(&mut tree)
        .expect("common amplification report");
    assert_eq!(report.point_read.unit, ReadWorkUnit::BtreePageAccess);
    assert_eq!(report.point_read.ratio.denominator, 1);
}

#[test]
fn reopen_sample_is_step_associated_and_counts_open_validation_work() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("reopen-work.db");
    let mut tree = BPlusTree::create_new(&path, 4).expect("create tree");
    tree.put(b"key", b"value").expect("put");
    tree.reset_operational_timing();
    tree.set_operational_step_index(Some(7));
    KvEngine::reopen(&mut tree).expect("reopen");

    let timing = tree.operational_timing_report();
    assert_eq!(timing.compaction_stall_samples, Vec::new());
    assert_eq!(timing.reopen_samples.len(), 1);
    let sample = timing.reopen_samples[0];
    assert_eq!(timing.reopen_ns, vec![sample.duration_ns]);
    assert_eq!(sample.measured_step_index, Some(7));
    assert_eq!(sample.work.unit, OperationalWorkUnit::BtreePageAccess);
    assert_eq!(
        sample.work.units_examined, 2,
        "open validates the root for tree integrity and reuse discovery"
    );
    assert_eq!(sample.work.bytes_examined, 2 * PAGE_SIZE as u64);
    assert!(sample.duration_ns > 0);
}
