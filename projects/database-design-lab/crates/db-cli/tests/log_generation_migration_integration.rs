use std::fs;
use std::path::Path;
use std::process::{Command, Output};

#[cfg(any(unix, windows))]
use db_cli::generation_engine::GenerationLogEngine;
#[cfg(any(unix, windows))]
use db_core::KvEngine;
#[cfg(any(unix, windows))]
use db_storage_log::LogEngine;
#[cfg(any(unix, windows))]
use serde_json::Value;
use tempfile::tempdir;

#[cfg(any(unix, windows))]
#[test]
fn migration_imports_live_state_and_leaves_legacy_source_untouched() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy-資料.db");
    let target = root.path().join("generations-資料");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"a", b"one").expect("put a one");
        engine.put(b"a", b"two").expect("overwrite a");
        engine.put(b"b", b"three").expect("put b");
        engine.delete(b"b").expect("delete b");
        engine.put(b"c", b"").expect("put empty c");
        engine.delete(b"missing").expect("delete missing");
    }
    let source_before = fs::read(&source).expect("read legacy source before migration");

    let output = run_migrate(&source, &target);
    assert_success("migrate legacy append log", &output);
    let summary: Value = serde_json::from_slice(&output.stdout).expect("decode migration summary");
    #[cfg(unix)]
    assert_eq!(
        summary["protocol"],
        "append_log_legacy_to_generation_migration_unix_v1"
    );
    #[cfg(windows)]
    assert_eq!(
        summary["protocol"],
        "append_log_legacy_to_generation_migration_windows_v1"
    );
    assert_eq!(summary["source_file_format_version"], 1);
    assert_eq!(summary["source_record_count"], 6);
    assert_eq!(summary["live_keys"], 2);
    assert_eq!(summary["generation"], 1);
    assert_eq!(summary["final_generation"]["authoritative_generation"], 1);
    #[cfg(windows)]
    assert_eq!(
        summary["final_generation"]["reservation_generation_ids"],
        serde_json::json!([1])
    );
    assert_eq!(
        fs::read(&source).expect("read legacy source after migration"),
        source_before,
        "successful migration must not mutate the legacy source"
    );
    assert!(target.join("generation-00000000000000000001.log").is_file());
    assert!(target.join("commit-00000000000000000001.marker").is_file());
    #[cfg(windows)]
    assert!(target
        .join("reserve-00000000000000000001.frontier")
        .is_file());

    let mut routed =
        GenerationLogEngine::open(&target).expect("open migrated generation directory");
    assert_eq!(routed.get(b"a").expect("get a"), Some(b"two".to_vec()));
    assert_eq!(routed.get(b"b").expect("get b"), None);
    assert_eq!(routed.get(b"c").expect("get c"), Some(Vec::new()));
    routed
        .put(b"d", b"four")
        .expect("write through routed engine");
    routed.reopen().expect("reopen routed engine");
    assert_eq!(routed.get(b"d").expect("get d"), Some(b"four".to_vec()));
    assert_eq!(routed.authoritative_generation(), 1);
    assert_eq!(
        fs::read(&source).expect("read legacy source after routed mutation"),
        source_before,
        "post-migration routed mutations must not touch the retained legacy file"
    );
}

#[cfg(any(unix, windows))]
#[test]
fn migration_never_overwrites_an_existing_target() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"key", b"value").expect("put source value");
    }
    fs::create_dir(&target).expect("create existing target");
    let sentinel = target.join("sentinel.txt");
    fs::write(&sentinel, b"keep-me").expect("write target sentinel");

    let output = run_migrate(&source, &target);
    assert_failure_contains(&output, "migration target already exists");
    assert_eq!(fs::read(&sentinel).expect("read sentinel"), b"keep-me");
}

#[cfg(any(unix, windows))]
#[test]
fn migration_rejects_recoverable_legacy_tail_before_creating_target() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    {
        let mut engine = LogEngine::create_new(&source).expect("create legacy source");
        engine.put(b"a", b"one").expect("put a");
        engine.put(b"b", b"two").expect("put b");
    }
    let length = fs::metadata(&source).expect("source metadata").len();
    fs::OpenOptions::new()
        .write(true)
        .open(&source)
        .expect("open source for truncation")
        .set_len(length - 1)
        .expect("truncate final append");
    let source_before = fs::read(&source).expect("read truncated source");

    let output = run_migrate(&source, &target);
    assert_failure_contains(&output, "complete clean append-log image");
    assert!(
        !target.exists(),
        "invalid source must not create migration target"
    );
    assert_eq!(
        fs::read(&source).expect("read truncated source after rejection"),
        source_before,
        "migration must not repair the legacy source implicitly"
    );
}

#[cfg(not(any(unix, windows)))]
#[test]
fn unsupported_platform_fails_before_filesystem_access() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("missing-legacy.db");
    let target = root.path().join("missing-generations");

    let output = run_migrate(&source, &target);
    assert_failure_contains(&output, "unsupported on this platform");
    assert!(!source.exists());
    assert!(!target.exists());
}

fn run_migrate(source: &Path, target: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-migrate"))
        .arg("--source")
        .arg(source)
        .arg("--target-directory")
        .arg(target)
        .output()
        .expect("run generation migration")
}

#[cfg(any(unix, windows))]
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
