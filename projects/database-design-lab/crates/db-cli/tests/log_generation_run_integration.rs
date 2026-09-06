#![cfg(any(unix, windows))]

use std::fs;
use std::path::Path;
use std::process::{Command, Output};

use db_cli::generation_directory::verify_generation_directory;
use db_cli::generation_lock::acquire_generation_writer_lease;
use db_core::{ByteString, KvEngine, Workload, WorkloadStep, WORKLOAD_FORMAT_VERSION};
use db_storage_log::LogEngine;
use serde_json::Value;
use tempfile::tempdir;

#[test]
fn generation_runner_executes_workload_only_through_authoritative_generation() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    let workload_path = root.path().join("workload.json");

    {
        let mut legacy = LogEngine::create_new(&source).expect("create legacy source");
        legacy.put(b"old", b"legacy").expect("put legacy value");
    }
    let source_before = fs::read(&source).expect("read legacy source before migration");
    assert_success("migrate legacy", &run_migrate(&source, &target));

    let workload = Workload {
        format_version: WORKLOAD_FORMAT_VERSION,
        seed: Some(42),
        steps: vec![
            WorkloadStep::Put {
                key: ByteString::from(b"new".to_vec()),
                value: ByteString::from(b"authority".to_vec()),
            },
            WorkloadStep::Get {
                key: ByteString::from(b"old".to_vec()),
            },
            WorkloadStep::Reopen,
            WorkloadStep::Delete {
                key: ByteString::from(b"old".to_vec()),
            },
            WorkloadStep::Get {
                key: ByteString::from(b"new".to_vec()),
            },
        ],
    };
    fs::write(
        &workload_path,
        serde_json::to_vec_pretty(&workload).expect("encode workload"),
    )
    .expect("write workload");

    let before = verify_generation_directory(&target).expect("verify imported generation");
    assert_eq!(before.summary().authoritative_generation, 1);
    let record_count_before = before.summary().log_verification.record_count;

    let output = run_generation(&target, &workload_path);
    assert_success("run generation-aware workload", &output);
    let summary: Value = serde_json::from_slice(&output.stdout).expect("decode run summary");
    assert_eq!(summary["engine"], "append-log-generation-v2");
    assert_eq!(summary["workload_format_version"], 1);
    assert_eq!(summary["seed"], 42);
    assert_eq!(summary["steps_executed"], 5);
    assert_eq!(summary["authoritative_generation"], 1);

    let after = verify_generation_directory(&target).expect("verify generation after workload");
    assert_eq!(after.summary().authoritative_generation, 1);
    assert_eq!(
        after.summary().log_verification.record_count,
        record_count_before + 2,
        "only PUT and DELETE should append mutation records"
    );
    let state =
        LogEngine::inspect(after.authoritative_log_path(), true).expect("inspect authority");
    assert!(state
        .entries
        .iter()
        .all(|entry| entry.key.as_slice() != b"old"));
    let new_entry = state
        .entries
        .iter()
        .find(|entry| entry.key.as_slice() == b"new")
        .expect("new authoritative entry");
    assert_eq!(
        new_entry.value.as_ref().map(|value| value.as_slice()),
        Some(b"authority" as &[u8])
    );
    assert_eq!(
        fs::read(&source).expect("read legacy source after routed workload"),
        source_before,
        "generation-aware workload execution must not mutate the legacy raw path"
    );
}

#[test]
fn generation_runner_fails_closed_while_writer_lease_is_held() {
    let root = tempdir().expect("temporary root");
    let source = root.path().join("legacy.db");
    let target = root.path().join("generations");
    let workload_path = root.path().join("workload.json");

    {
        let mut legacy = LogEngine::create_new(&source).expect("create legacy source");
        legacy.put(b"key", b"value").expect("put legacy value");
    }
    assert_success("migrate legacy", &run_migrate(&source, &target));
    let workload = Workload {
        format_version: WORKLOAD_FORMAT_VERSION,
        seed: None,
        steps: vec![WorkloadStep::Get {
            key: ByteString::from(b"key".to_vec()),
        }],
    };
    fs::write(
        &workload_path,
        serde_json::to_vec_pretty(&workload).expect("encode workload"),
    )
    .expect("write workload");

    let _lease = acquire_generation_writer_lease(&target).expect("hold writer lease");
    let output = run_generation(&target, &workload_path);
    assert_failure_contains(&output, "generation writer lock is held or stale");
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

fn run_generation(directory: &Path, workload: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-run"))
        .arg("--directory")
        .arg(directory)
        .arg("--workload")
        .arg(workload)
        .output()
        .expect("run generation-aware workload")
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
