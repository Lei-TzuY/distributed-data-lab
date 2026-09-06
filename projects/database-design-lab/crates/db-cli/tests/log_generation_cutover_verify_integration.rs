#![cfg(any(unix, windows))]

use std::path::Path;
use std::process::{Command, Output};

use db_cli::generation_engine::GenerationLogEngine;
use db_core::KvEngine;
use db_storage_log::LogEngine;
use serde_json::Value;
use tempfile::tempdir;

#[test]
fn fresh_cutover_verifier_accepts_cross_platform_migration_handoff() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"a", b"one").expect("put a");
        engine.put(b"b", b"two").expect("put b");
        engine.delete(b"b").expect("delete b");
    }

    assert_success("migrate legacy", &run_migrate(&source, &target));
    assert_success("cut over legacy", &run_cutover(&source, &target));

    let verified = run_verify(&source, &target);
    assert_success("verify fresh cutover", &verified);
    let summary: Value = serde_json::from_slice(&verified.stdout).expect("decode verify summary");
    assert_eq!(
        summary["protocol"],
        "append_log_legacy_cutover_verification_v1"
    );
    assert_eq!(summary["target_generation"], 1);
    assert_eq!(summary["retained_verification"]["record_count"], 3);
    assert_eq!(summary["retained_verification"]["live_keys"], 1);
    assert_eq!(summary["final_generation"]["authoritative_generation"], 1);
}

#[test]
fn fresh_cutover_verifier_rejects_target_advanced_after_handoff() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"key", b"value").expect("put legacy value");
    }

    assert_success("migrate legacy", &run_migrate(&source, &target));
    assert_success("cut over legacy", &run_cutover(&source, &target));

    let mut routed = GenerationLogEngine::open(&target).expect("open routed target");
    routed
        .put(b"post-cutover", b"authority")
        .expect("advance generation authority");
    drop(routed);

    let verified = run_verify(&source, &target);
    assert_failure_contains(&verified, "fresh cutover evidence is no longer valid");
}

#[test]
fn fresh_cutover_verifier_rejects_retained_rollback_drift() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    let retained = root.path().join("legacy.db.retired-append-log-v1");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"key", b"value").expect("put legacy value");
    }

    assert_success("migrate legacy", &run_migrate(&source, &target));
    assert_success("cut over legacy", &run_cutover(&source, &target));

    let mut retained_engine = LogEngine::open(&retained).expect("open retained rollback evidence");
    retained_engine
        .put(b"drift", b"detected")
        .expect("mutate retained evidence for negative test");
    drop(retained_engine);

    let verified = run_verify(&source, &target);
    assert_failure_contains(
        &verified,
        "does not reproduce the retained legacy live state",
    );
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

fn run_verify(source: &Path, target: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-cutover-verify"))
        .arg("--legacy-source")
        .arg(source)
        .arg("--target-directory")
        .arg(target)
        .output()
        .expect("run fresh cutover verifier")
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
