#![cfg(windows)]

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use db_cli::generation_directory::{
    canonical_generation_name, canonical_marker_name, canonical_reservation_name,
    canonical_staging_marker_name, verify_generation_directory,
};
use db_cli::generation_marker::{crc32_ieee, encode_commit_marker, CommittedPrefix};
use db_core::KvEngine;
use db_storage_log::LogEngine;
use serde_json::Value;
use tempfile::tempdir;

#[test]
fn windows_switch_publishes_write_through_candidate_then_marker_authority() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("世代-資料");
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
    write_fixture_marker(&directory, 1);

    {
        let mut engine =
            LogEngine::open_managed_generation(&source).expect("open authoritative generation");
        engine
            .put(b"post-marker", b"durable")
            .expect("append after fixture marker");
    }
    let source_before = LogEngine::inspect(&source, true).expect("inspect source");
    assert_eq!(source_before.verification.record_count, 6);
    assert_eq!(source_before.verification.live_keys, 3);

    let switched = run_switch(&directory);
    assert_success("Windows compact switch", &switched);
    let summary: Value = serde_json::from_slice(&switched.stdout).expect("decode switch summary");
    assert_eq!(
        summary["protocol"],
        "append_log_offline_generation_compact_switch_windows_v1"
    );
    assert_eq!(summary["old_generation"], 1);
    assert_eq!(summary["new_generation"], 2);
    assert_eq!(
        summary["reservation"]["protocol"],
        "append_log_generation_reservation_windows_v1"
    );
    assert_eq!(summary["reservation"]["generation"], 2);
    assert_eq!(
        summary["compaction"]["protocol"],
        "append_log_compact_copy_v1"
    );
    assert_eq!(summary["compaction"]["source_record_count"], 6);
    assert_eq!(summary["compaction"]["compacted_record_count"], 3);
    assert_eq!(
        summary["publication"]["protocol"],
        "append_log_generation_marker_publication_windows_v1"
    );
    assert_eq!(summary["publication"]["generation"], 2);
    assert_eq!(summary["publication"]["staging_retained"], false);
    assert_eq!(summary["final_generation"]["authoritative_generation"], 2);

    assert!(generation_path(&directory, 1).is_file());
    assert!(marker_path(&directory, 1).is_file());
    assert!(directory.join(canonical_reservation_name(2)).is_file());
    assert!(generation_path(&directory, 2).is_file());
    assert!(marker_path(&directory, 2).is_file());
    assert!(!directory.join(canonical_staging_marker_name(2)).exists());

    let compacted = LogEngine::inspect(generation_path(&directory, 2), true)
        .expect("inspect compact generation");
    assert_eq!(compacted.entries, source_before.entries);
    assert_eq!(compacted.verification.record_count, 3);

    let verified = verify_generation_directory(&directory).expect("verify switched directory");
    assert_eq!(verified.summary().authoritative_generation, 2);
    assert_eq!(verified.summary().marker_generation_ids, vec![1, 2]);
    assert_eq!(verified.summary().reservation_generation_ids, vec![2]);
}

#[test]
fn windows_standalone_publisher_remains_fail_closed_for_arbitrary_generation() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("standalone-boundary");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, b"old");
    write_fixture_marker(&directory, 1);
    create_generation(&directory, 2, b"candidate");

    let published = run_publish(&directory, 2);
    assert_failure_contains(&published, "unsupported on this platform");
    assert!(!marker_path(&directory, 2).exists());
    let verified = verify_generation_directory(&directory).expect("verify old authority");
    assert_eq!(verified.summary().authoritative_generation, 1);
}

fn create_generation(directory: &Path, id: u64, value: &[u8]) {
    let mut engine = LogEngine::create_new_managed_generation(generation_path(directory, id))
        .expect("create generation");
    engine.put(b"key", value).expect("put generation value");
}

fn write_fixture_marker(directory: &Path, id: u64) {
    let generation = generation_path(directory, id);
    let report = LogEngine::verify(&generation).expect("verify fixture generation");
    assert!(report.recoverable_tail.is_none());
    let bytes = fs::read(&generation).expect("read fixture generation");
    let proof = CommittedPrefix {
        bytes: report.file_bytes,
        crc32: crc32_ieee(&bytes),
        record_count: report.record_count,
        next_sequence: report.next_sequence,
    };
    fs::write(
        marker_path(directory, id),
        encode_commit_marker(id, proof).expect("encode fixture marker"),
    )
    .expect("write fixture marker");
}

fn generation_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_generation_name(id))
}

fn marker_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_marker_name(id))
}

fn run_switch(directory: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-compact-switch"))
        .arg("--directory")
        .arg(directory)
        .output()
        .expect("run compact switch")
}

fn run_publish(directory: &Path, id: u64) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-publish"))
        .arg("--directory")
        .arg(directory)
        .arg("--generation")
        .arg(id.to_string())
        .output()
        .expect("run standalone publisher")
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
