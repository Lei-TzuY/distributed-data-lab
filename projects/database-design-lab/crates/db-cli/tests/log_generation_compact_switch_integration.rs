#![cfg(not(windows))]

use std::fs;
use std::path::Path;
#[cfg(unix)]
use std::path::PathBuf;
use std::process::{Command, Output};

use tempfile::tempdir;

#[cfg(unix)]
use db_core::KvEngine;
#[cfg(unix)]
use db_storage_log::LogEngine;
#[cfg(unix)]
use serde_json::Value;

#[cfg(unix)]
#[test]
fn unix_offline_switch_compacts_full_authoritative_live_state_and_advances_authority() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    let source = generation_path(&directory, 1);
    {
        let mut engine =
            LogEngine::create_new_managed_generation(&source).expect("create source generation");
        engine.put(b"a", b"one").expect("put a one");
        engine.put(b"a", b"two").expect("overwrite a");
        engine.put(b"b", b"three").expect("put b");
        engine.delete(b"b").expect("delete b");
        engine.put(b"c", b"").expect("put c");
    }
    assert_success("publish generation 1", &run_publish(&directory, 1));

    {
        let mut engine =
            LogEngine::open_managed_generation(&source).expect("open authoritative generation");
        engine
            .put(b"post-marker", b"durable")
            .expect("append after marker publication");
    }
    let source_before = LogEngine::inspect(&source, true).expect("inspect authoritative source");
    assert_eq!(source_before.verification.record_count, 6);
    assert_eq!(source_before.verification.live_keys, 3);

    let switched = run_switch(&directory);
    assert_success("compact-switch generation directory", &switched);
    let summary: Value = serde_json::from_slice(&switched.stdout).expect("decode switch summary");
    assert_eq!(
        summary["protocol"],
        "append_log_offline_generation_compact_switch_unix_v2"
    );
    assert_eq!(summary["old_generation"], 1);
    assert_eq!(summary["new_generation"], 2);
    assert_eq!(summary["reservation"]["generation"], 2);
    assert_eq!(
        summary["reservation"]["reservation"],
        "reserve-00000000000000000002.frontier"
    );
    assert_eq!(summary["compaction"]["source_record_count"], 6);
    assert_eq!(summary["compaction"]["live_keys"], 3);
    assert_eq!(summary["compaction"]["compacted_record_count"], 3);
    assert_eq!(summary["publication"]["generation"], 2);
    assert_eq!(summary["final_generation"]["authoritative_generation"], 2);
    assert_eq!(
        summary["final_generation"]["reservation_generation_ids"],
        serde_json::json!([2])
    );

    assert!(generation_path(&directory, 1).is_file());
    assert!(marker_path(&directory, 1).is_file());
    assert!(generation_path(&directory, 2).is_file());
    assert!(marker_path(&directory, 2).is_file());
    let compacted =
        LogEngine::inspect(generation_path(&directory, 2), true).expect("inspect new generation");
    assert_eq!(compacted.entries, source_before.entries);
    assert_eq!(compacted.verification.record_count, 3);

    let verified = run_verify(&directory);
    assert_success("verify switched directory", &verified);
    let verified: Value =
        serde_json::from_slice(&verified.stdout).expect("decode verifier summary");
    assert_eq!(verified["authoritative_generation"], 2);
    assert_eq!(verified["marker_generation_ids"], serde_json::json!([1, 2]));
    assert_eq!(
        verified["reservation_generation_ids"],
        serde_json::json!([2])
    );
}

#[cfg(unix)]
#[test]
fn unix_offline_switch_uses_a_fresh_durable_reservation_above_all_retained_frontier_evidence() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"key", b"authoritative")]);
    assert_success("publish generation 1", &run_publish(&directory, 1));

    create_generation(&directory, 5, &[(b"orphan", b"candidate")]);
    fs::write(staging_marker_path(&directory, 7), b"crash residue").expect("write staging residue");
    let preexisting_reservation = run_reserve(&directory);
    assert_success(
        "reserve generation above orphan/staging evidence",
        &preexisting_reservation,
    );
    let preexisting: Value =
        serde_json::from_slice(&preexisting_reservation.stdout).expect("decode reservation");
    assert_eq!(preexisting["generation"], 8);

    let switched = run_switch(&directory);
    assert_success("compact-switch above retained frontier", &switched);
    let summary: Value = serde_json::from_slice(&switched.stdout).expect("decode switch summary");
    assert_eq!(summary["old_generation"], 1);
    assert_eq!(summary["new_generation"], 9);
    assert_eq!(summary["reservation"]["generation"], 9);
    assert!(generation_path(&directory, 5).is_file());
    assert!(staging_marker_path(&directory, 7).is_file());
    assert!(!generation_path(&directory, 2).exists());
    assert!(!generation_path(&directory, 8).exists());
    assert!(generation_path(&directory, 9).is_file());
    assert!(marker_path(&directory, 9).is_file());

    let verified = run_verify(&directory);
    assert_success("verify allocation frontier", &verified);
    let verified: Value =
        serde_json::from_slice(&verified.stdout).expect("decode verifier summary");
    assert_eq!(verified["authoritative_generation"], 9);
    assert_eq!(verified["highest_observed_generation"], 9);
    assert_eq!(
        verified["uncommitted_generation_ids"],
        serde_json::json!([5])
    );
    assert_eq!(
        verified["staging_marker_generation_ids"],
        serde_json::json!([7])
    );
    assert_eq!(
        verified["reservation_generation_ids"],
        serde_json::json!([8, 9])
    );
}

#[cfg(not(unix))]
#[test]
fn unsupported_platform_fails_before_touching_generation_directory() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");

    let switched = run_switch(&directory);
    assert_failure_contains(&switched, "unsupported on this platform");
    assert_eq!(
        fs::read_dir(&directory)
            .expect("read generation directory")
            .count(),
        0,
        "unsupported compact switch must not write any artifact"
    );
}

#[cfg(unix)]
fn create_generation(directory: &Path, id: u64, entries: &[(&[u8], &[u8])]) {
    let mut engine = LogEngine::create_new_managed_generation(generation_path(directory, id))
        .expect("create generation");
    for (key, value) in entries {
        engine.put(key, value).expect("put generation entry");
    }
}

#[cfg(unix)]
fn generation_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(format!("generation-{id:020}.log"))
}

#[cfg(unix)]
fn marker_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(format!("commit-{id:020}.marker"))
}

#[cfg(unix)]
fn staging_marker_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(format!("staging-commit-{id:020}.marker"))
}

fn run_switch(directory: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-compact-switch"))
        .arg("--directory")
        .arg(directory)
        .output()
        .expect("run generation compact switch")
}

#[cfg(unix)]
fn run_publish(directory: &Path, id: u64) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-publish"))
        .arg("--directory")
        .arg(directory)
        .arg("--generation")
        .arg(id.to_string())
        .output()
        .expect("run generation publisher")
}

#[cfg(unix)]
fn run_reserve(directory: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-reserve"))
        .arg("--directory")
        .arg(directory)
        .output()
        .expect("run generation reservation")
}

#[cfg(unix)]
fn run_verify(directory: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-verify"))
        .arg("--directory")
        .arg(directory)
        .output()
        .expect("run generation verifier")
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

#[cfg(not(unix))]
fn assert_failure_contains(output: &Output, needle: &str) {
    assert!(!output.status.success(), "command unexpectedly succeeded");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains(needle),
        "stderr did not contain {needle:?}:\n{stderr}"
    );
}
