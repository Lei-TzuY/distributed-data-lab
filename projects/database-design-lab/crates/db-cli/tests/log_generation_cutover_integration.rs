use std::fs;
use std::path::Path;
use std::process::{Command, Output};

use tempfile::tempdir;

#[cfg(unix)]
use db_cli::generation_directory::verify_generation_directory;
#[cfg(unix)]
use db_cli::generation_engine::GenerationLogEngine;
#[cfg(unix)]
use db_core::KvEngine;
#[cfg(unix)]
use db_storage_log::LogEngine;
#[cfg(unix)]
use serde_json::Value;

#[cfg(unix)]
#[test]
fn unix_cutover_retires_legacy_path_and_isolates_preexisting_raw_handle() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    let retained = root.path().join("legacy.db.retired-append-log-v1");

    let mut stale = LogEngine::create_new(&source).expect("create legacy source");
    stale.put(b"a", b"one").expect("put a one");
    stale.put(b"a", b"two").expect("overwrite a");
    stale.put(b"b", b"three").expect("put b");
    stale.delete(b"b").expect("delete b");
    let legacy_before = fs::read(&source).expect("read legacy bytes before migration");

    assert_success("migrate legacy", &run_migrate(&source, &target));
    let cutover = run_cutover(&source, &target);
    assert_success("cut over legacy pathname", &cutover);
    let summary: Value = serde_json::from_slice(&cutover.stdout).expect("decode cutover summary");
    assert_eq!(
        summary["protocol"],
        "append_log_legacy_cutover_sentinel_unix_v1"
    );
    assert_eq!(summary["target_generation"], 1);
    assert_eq!(summary["source_record_count"], 4);
    assert_eq!(summary["live_keys"], 1);

    let sentinel_before_stale_write = fs::read(&source).expect("read cutover sentinel");
    let sentinel: Value =
        serde_json::from_slice(&sentinel_before_stale_write).expect("decode cutover sentinel");
    assert_eq!(
        sentinel["protocol"],
        "append_log_legacy_cutover_sentinel_v1"
    );
    assert_eq!(
        fs::read(&retained).expect("read retained legacy source"),
        legacy_before,
        "retained hard link must preserve the exact pre-cutover legacy bytes"
    );
    assert!(
        LogEngine::open(&source).is_err(),
        "new raw append-log opens at the retired pathname must fail"
    );

    let before_target = verify_generation_directory(&target).expect("verify imported target");
    assert_eq!(before_target.summary().authoritative_generation, 1);
    let target_log = before_target.authoritative_log_path();
    let target_before_stale_write = fs::read(&target_log).expect("read target before stale write");

    stale
        .put(b"stale", b"isolated")
        .expect("preexisting raw handle can only mutate retained inode");
    assert_eq!(
        fs::read(&source).expect("re-read cutover sentinel"),
        sentinel_before_stale_write,
        "stale raw handle must not mutate the retired pathname sentinel"
    );
    assert_ne!(
        fs::read(&retained).expect("read retained source after stale write"),
        legacy_before,
        "stale handle should demonstrate that the retained inode is isolated"
    );
    assert_eq!(
        fs::read(&target_log).expect("read target after stale write"),
        target_before_stale_write,
        "stale raw handle must not mutate generation authority"
    );

    let retained_state = LogEngine::inspect(&retained, true).expect("inspect retained source");
    let stale_entry = retained_state
        .entries
        .iter()
        .find(|entry| entry.key.as_slice() == b"stale")
        .expect("retained stale entry");
    assert_eq!(
        stale_entry.value.as_ref().map(|value| value.as_slice()),
        Some(b"isolated" as &[u8])
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
        sentinel_before_stale_write
    );
}

#[cfg(unix)]
#[test]
fn unix_cutover_rejects_target_mutated_after_migration() {
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
    assert_failure_contains(&cutover, "changed since migration publication");
    assert!(LogEngine::inspect(&source, true).is_ok());
    assert!(!root.path().join("legacy.db.retired-append-log-v1").exists());
    assert!(!root
        .path()
        .join("legacy.db.cutover-sentinel-staging-v1")
        .exists());
}

#[cfg(unix)]
#[test]
fn unix_cutover_never_overwrites_retained_rollback_evidence() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    let retained = root.path().join("legacy.db.retired-append-log-v1");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"a", b"one").expect("put legacy value");
    }
    assert_success("migrate legacy", &run_migrate(&source, &target));
    fs::write(&retained, b"sentinel rollback evidence").expect("write retained collision");

    let cutover = run_cutover(&source, &target);
    assert_failure_contains(&cutover, "retained legacy source already exists");
    assert_eq!(
        fs::read(&retained).expect("read retained collision"),
        b"sentinel rollback evidence"
    );
    assert!(LogEngine::inspect(&source, true).is_ok());
}

#[cfg(not(any(unix, windows)))]
#[test]
fn unsupported_platform_fails_before_filesystem_access() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("does-not-exist.db");
    let target = root.path().join("also-does-not-exist");

    let cutover = run_cutover(&source, &target);
    assert_failure_contains(&cutover, "unsupported on this platform");
    assert!(!source.exists());
    assert!(!target.exists());
    assert_eq!(fs::read_dir(root.path()).expect("read root").count(), 0);
}

#[cfg(unix)]
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

#[cfg(unix)]
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
