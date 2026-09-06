use std::fs;
use std::path::{Path, PathBuf};

use tempfile::{tempdir, TempDir};

use super::{BPlusTree, LeafEntry, StoredKey, StoredValue};
use crate::{BtreeError, DurableWriteKind, FaultMode, PageKind};

const LEFT_KEY: &[u8] = b"a";
const TARGET_KEY: &[u8] = b"m";
const RIGHT_KEY: &[u8] = b"z";
const SMALL_OLD: &[u8] = b"old";
const LARGE_LEN: usize = 12 * 1024;

fn large(byte: u8) -> Vec<u8> {
    vec![byte; LARGE_LEN]
}

fn inline_entry(key: &[u8], value: &[u8]) -> LeafEntry {
    LeafEntry {
        key: StoredKey::Inline(key.to_vec()),
        value: StoredValue::Inline(value.to_vec()),
    }
}

fn build_two_leaf_fixture(path: &Path, target_value: &[u8]) -> BPlusTree {
    let mut tree = BPlusTree::create_new(path, 8).expect("create direct fixture");
    let target_key = StoredKey::Inline(TARGET_KEY.to_vec());
    let target_value = tree
        .store_value(&target_key, target_value)
        .expect("store target fixture value");
    let left = tree
        .commit_leaf(&[
            inline_entry(LEFT_KEY, b"left"),
            LeafEntry {
                key: target_key,
                value: target_value,
            },
        ])
        .expect("commit left fixture leaf");
    let right = tree
        .commit_leaf(&[inline_entry(RIGHT_KEY, b"right")])
        .expect("commit right fixture leaf");
    let root = tree
        .commit_internal(&[left, right])
        .expect("commit direct fixture root");
    tree.pager
        .set_root(Some(root.page_id))
        .expect("publish direct fixture root");
    assert!(tree
        .reusable_page_ids_for_test()
        .expect("derive direct fixture reuse")
        .is_empty());
    tree
}

fn clone_db(source: &Path, directory: &TempDir, name: &str) -> PathBuf {
    let target = directory.path().join(name);
    fs::copy(source, &target).expect("copy database snapshot");
    target
}

fn trace_put(source: &Path, directory: &TempDir, value: &[u8]) -> Vec<DurableWriteKind> {
    let path = clone_db(source, directory, "trace-put.db");
    let mut tree = BPlusTree::open(&path, 8).expect("open trace fixture");
    tree.pager.begin_fault_trace_for_test();
    tree.put(TARGET_KEY, value).expect("trace put");
    tree.pager.fault_trace_for_test().to_vec()
}

fn trace_delete(source: &Path, directory: &TempDir) -> Vec<DurableWriteKind> {
    let path = clone_db(source, directory, "trace-delete.db");
    let mut tree = BPlusTree::open(&path, 8).expect("open delete trace fixture");
    tree.pager.begin_fault_trace_for_test();
    tree.delete(TARGET_KEY).expect("trace delete");
    tree.pager.fault_trace_for_test().to_vec()
}

fn event_index(trace: &[DurableWriteKind], site: DurableWriteKind) -> usize {
    trace
        .iter()
        .position(|candidate| *candidate == site)
        .unwrap_or_else(|| panic!("fault trace did not contain {site:?}: {trace:?}"))
}

fn expected_new(site: DurableWriteKind, mode: FaultMode) -> bool {
    site == DurableWriteKind::RootSuperblock && mode == FaultMode::AfterSync
}

struct PutFaultCase<'a> {
    case: usize,
    event_index: usize,
    site: DurableWriteKind,
    mode: FaultMode,
    old_value: &'a [u8],
    new_value: &'a [u8],
}

fn assert_put_fault(baseline: &Path, directory: &TempDir, fault: PutFaultCase<'_>) {
    let PutFaultCase {
        case,
        event_index,
        site,
        mode,
        old_value,
        new_value,
    } = fault;
    let path = clone_db(baseline, directory, &format!("put-{case}.db"));
    let mut tree = BPlusTree::open(&path, 8).expect("open put fault fixture");
    tree.pager.inject_fault_for_test(event_index, mode);
    let error = tree
        .put(TARGET_KEY, new_value)
        .expect_err("injected put must return an I/O error");
    assert!(
        matches!(error, BtreeError::Io(_)),
        "{site:?} {mode:?}: {error}"
    );
    assert!(matches!(tree.get(TARGET_KEY), Err(BtreeError::Poisoned)));
    drop(tree);

    let mut reopened = BPlusTree::open(&path, 8).expect("reopen put fault fixture");
    let expected = if expected_new(site, mode) {
        new_value
    } else {
        old_value
    };
    assert_eq!(
        reopened.get(TARGET_KEY).expect("read target after fault"),
        Some(expected.to_vec()),
        "{site:?} {mode:?}"
    );
    assert_eq!(
        reopened.get(LEFT_KEY).expect("read left sentinel"),
        Some(b"left".to_vec())
    );
    assert_eq!(
        reopened.get(RIGHT_KEY).expect("read right sentinel"),
        Some(b"right".to_vec())
    );

    if mode == FaultMode::TornWrite && matches!(site, DurableWriteKind::RecycledPage(_)) {
        let repair = large(0x44);
        reopened
            .put(TARGET_KEY, &repair)
            .expect("later mutation overwrites torn unreachable page");
        drop(reopened);
        let mut repaired = BPlusTree::open(&path, 8).expect("reopen repaired tree");
        assert_eq!(
            repaired.get(TARGET_KEY).expect("read repaired target"),
            Some(repair)
        );
    }
}

#[test]
fn append_fault_matrix_preserves_old_or_complete_new_tree() {
    let directory = tempdir().expect("temporary directory");
    let baseline = directory.path().join("append-baseline.db");
    drop(build_two_leaf_fixture(&baseline, SMALL_OLD));
    let new_value = large(0x31);
    let trace = trace_put(&baseline, &directory, &new_value);
    let sites = [
        DurableWriteKind::AppendPage(PageKind::Overflow),
        DurableWriteKind::AppendPage(PageKind::Leaf),
        DurableWriteKind::AppendPage(PageKind::Internal),
        DurableWriteKind::AllocationSuperblock,
        DurableWriteKind::RootSuperblock,
    ];
    let modes = [
        FaultMode::BeforeWrite,
        FaultMode::TornWrite,
        FaultMode::AfterSync,
    ];

    let mut case = 0;
    for site in sites {
        let index = event_index(&trace, site);
        for mode in modes {
            assert_put_fault(
                &baseline,
                &directory,
                PutFaultCase {
                    case,
                    event_index: index,
                    site,
                    mode,
                    old_value: SMALL_OLD,
                    new_value: &new_value,
                },
            );
            case += 1;
        }
    }
}

#[test]
fn recycled_fault_matrix_keeps_authoritative_root_and_repairs_torn_orphans() {
    let directory = tempdir().expect("temporary directory");
    let baseline = directory.path().join("recycled-baseline.db");
    let first = large(0x51);
    let second = large(0x52);
    let third = large(0x53);
    let mut tree = build_two_leaf_fixture(&baseline, &first);
    tree.put(TARGET_KEY, &second)
        .expect("create unreachable history for recycled fixture");
    drop(tree);

    let trace = trace_put(&baseline, &directory, &third);
    assert!(
        !trace
            .iter()
            .any(|event| matches!(event, DurableWriteKind::AppendPage(_))),
        "recycled fixture unexpectedly appended pages: {trace:?}"
    );
    let sites = [
        DurableWriteKind::RecycledPage(PageKind::Overflow),
        DurableWriteKind::RecycledPage(PageKind::Leaf),
        DurableWriteKind::RecycledPage(PageKind::Internal),
        DurableWriteKind::RootSuperblock,
    ];
    let modes = [
        FaultMode::BeforeWrite,
        FaultMode::TornWrite,
        FaultMode::AfterSync,
    ];

    let mut case = 100;
    for site in sites {
        let index = event_index(&trace, site);
        for mode in modes {
            assert_put_fault(
                &baseline,
                &directory,
                PutFaultCase {
                    case,
                    event_index: index,
                    site,
                    mode,
                    old_value: &second,
                    new_value: &third,
                },
            );
            case += 1;
        }
    }
}

#[test]
fn final_delete_root_clear_is_old_or_new_when_metadata_write_is_ambiguous() {
    let directory = tempdir().expect("temporary directory");
    let baseline = directory.path().join("delete-baseline.db");
    let mut tree = BPlusTree::create_new(&baseline, 4).expect("create delete fixture");
    tree.put(TARGET_KEY, SMALL_OLD)
        .expect("insert delete target");
    drop(tree);

    let trace = trace_delete(&baseline, &directory);
    assert_eq!(trace, vec![DurableWriteKind::RootSuperblock]);
    for (case, mode) in [
        FaultMode::BeforeWrite,
        FaultMode::TornWrite,
        FaultMode::AfterSync,
    ]
    .into_iter()
    .enumerate()
    {
        let path = clone_db(&baseline, &directory, &format!("delete-{case}.db"));
        let mut tree = BPlusTree::open(&path, 4).expect("open delete fault fixture");
        tree.pager.inject_fault_for_test(0, mode);
        let error = tree
            .delete(TARGET_KEY)
            .expect_err("injected delete must return an I/O error");
        assert!(matches!(error, BtreeError::Io(_)));
        assert!(matches!(tree.get(TARGET_KEY), Err(BtreeError::Poisoned)));
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 4).expect("reopen delete fault fixture");
        if mode == FaultMode::AfterSync {
            assert_eq!(reopened.root_page_id(), None);
            assert_eq!(reopened.get(TARGET_KEY).expect("read deleted key"), None);
        } else {
            assert!(reopened.root_page_id().is_some());
            assert_eq!(
                reopened.get(TARGET_KEY).expect("read preserved key"),
                Some(SMALL_OLD.to_vec())
            );
        }
    }
}
