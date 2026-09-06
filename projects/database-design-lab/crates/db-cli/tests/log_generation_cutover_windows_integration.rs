#![cfg(windows)]

use std::fs;
use std::path::Path;
use std::process::{Command, Output};

use db_cli::generation_directory::verify_generation_directory;
use db_cli::generation_engine::GenerationLogEngine;
use db_core::KvEngine;
use db_storage_log::LogEngine;
use serde_json::Value;
use tempfile::tempdir;

#[test]
fn windows_cutover_retires_legacy_path_and_preserves_exact_rollback_copy() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy-資料.db");
    let target = root.path().join("generations-資料");
    let retained = root.path().join("legacy-資料.db.retired-append-log-v1");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"a", b"one").expect("put a one");
        engine.put(b"a", b"two").expect("overwrite a");
        engine.put(b"b", b"three").expect("put b");
        engine.delete(b"b").expect("delete b");
    }
    let legacy_before = fs::read(&source).expect("read legacy bytes before migration");

    assert_success("migrate legacy", &run_migrate(&source, &target));
    let target_before = verify_generation_directory(&target).expect("verify migrated target");
    assert_eq!(target_before.summary().authoritative_generation, 1);
    assert_eq!(target_before.summary().reservation_generation_ids, [1]);
    let target_log = target_before.authoritative_log_path();
    let target_bytes_before = fs::read(&target_log).expect("read target before cutover");

    let cutover = run_cutover(&source, &target);
    assert_success("cut over Windows legacy pathname", &cutover);
    let summary: Value = serde_json::from_slice(&cutover.stdout).expect("decode cutover summary");
    assert_eq!(
        summary["protocol"],
        "append_log_legacy_cutover_sentinel_windows_v1"
    );
    assert_eq!(summary["target_generation"], 1);
    assert_eq!(summary["source_record_count"], 4);
    assert_eq!(summary["live_keys"], 1);

    let sentinel_bytes = fs::read(&source).expect("read Windows cutover sentinel");
    let sentinel: Value = serde_json::from_slice(&sentinel_bytes).expect("decode cutover sentinel");
    assert_eq!(
        sentinel["protocol"],
        "append_log_legacy_cutover_sentinel_v1"
    );
    assert_eq!(
        fs::read(&retained).expect("read retained legacy source"),
        legacy_before,
        "Windows cutover must retain an exact byte copy before replacing the pathname"
    );
    assert!(
        LogEngine::open(&source).is_err(),
        "new raw append-log opens at the retired Windows pathname must fail"
    );
    assert_eq!(
        fs::read(&target_log).expect("read target after cutover"),
        target_bytes_before,
        "pathname cutover must not mutate generation authority"
    );

    let mut routed = GenerationLogEngine::open(&target).expect("open generation-aware engine");
    routed
        .put(b"new", b"authority")
        .expect("route post-cutover mutation");
    assert_eq!(
        routed.get(b"new").expect("get routed key"),
        Some(b"authority".to_vec())
    );
    assert_eq!(
        fs::read(&source).expect("sentinel after routed write"),
        sentinel_bytes,
        "routed mutation must not modify the retired legacy pathname"
    );
    assert_eq!(
        fs::read(&retained).expect("retained evidence after routed write"),
        legacy_before,
        "routed mutation must not modify retained rollback evidence"
    );
}

#[test]
fn windows_cutover_rejects_target_mutated_after_migration() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"a", b"one").expect("put legacy value");
    }
    assert_success("migrate legacy", &run_migrate(&source, &target));

    let mut routed = GenerationLogEngine::open(&target).expect("open routed target");
    routed
        .put(b"new", b"value")
        .expect("mutate target after migration");

    let cutover = run_cutover(&source, &target);
    assert_failure_contains(&cutover, "changed since Windows migration publication");
    assert!(LogEngine::inspect(&source, true).is_ok());
    assert!(!root.path().join("legacy.db.retired-append-log-v1").exists());
}

#[test]
fn windows_cutover_never_overwrites_conflicting_retained_evidence() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    let retained = root.path().join("legacy.db.retired-append-log-v1");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"a", b"one").expect("put legacy value");
    }
    assert_success("migrate legacy", &run_migrate(&source, &target));
    fs::write(&retained, b"foreign rollback evidence").expect("write retained collision");

    let cutover = run_cutover(&source, &target);
    assert_failure_contains(&cutover, "already exists with different bytes");
    assert_eq!(
        fs::read(&retained).expect("read retained collision"),
        b"foreign rollback evidence"
    );
    assert!(LogEngine::inspect(&source, true).is_ok());
}

#[test]
fn windows_cutover_can_reuse_matching_retained_evidence_from_safe_retry() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    let retained = root.path().join("legacy.db.retired-append-log-v1");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"key", b"value").expect("put legacy value");
    }
    assert_success("migrate legacy", &run_migrate(&source, &target));
    fs::copy(&source, &retained).expect("seed exact retained retry evidence");

    let cutover = run_cutover(&source, &target);
    assert_success("cut over with exact retained evidence", &cutover);
    assert!(LogEngine::open(&source).is_err());
    assert!(LogEngine::inspect(&retained, true).is_ok());
}

fn run_migrate(source: &Path, target: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-migrate"))
        .arg("--source")
        .arg(source)
        .arg("--target-directory")
        .arg(target)
        .output()
        .expect("run legacy migration")
}

fn run_cutover(source: &Path, target: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-cutover"))
        .arg("--legacy-source")
        .arg(source)
        .arg("--target-directory")
        .arg(target)
        .output()
        .expect("run legacy cutover")
}

fn assert_success(label: &str, output: &Output) {
    assert!(
        output.status.success(),
        "{label} failed\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn assert_failure_contains(output: &Output, needle: &str) {
    assert!(!output.status.success(), "command unexpectedly succeeded");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains(needle),
        "stderr did not contain {needle:?}:\n{stderr}"
    );
}
