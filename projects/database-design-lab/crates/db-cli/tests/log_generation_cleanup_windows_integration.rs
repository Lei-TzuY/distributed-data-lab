use std::path::Path;
use std::process::{Command, Output};

#[cfg(windows)]
use std::fs;
#[cfg(windows)]
use std::path::PathBuf;

#[cfg(windows)]
use db_cli::generation_directory::{
    canonical_generation_name, canonical_marker_name, canonical_staging_marker_name,
    verify_generation_directory,
};
#[cfg(windows)]
use db_cli::generation_marker::{crc32_ieee, encode_commit_marker, CommittedPrefix};
#[cfg(windows)]
use db_core::KvEngine;
#[cfg(windows)]
use db_storage_log::LogEngine;
#[cfg(windows)]
use serde_json::Value;
use tempfile::tempdir;

#[cfg(windows)]
#[test]
fn windows_cleanup_retires_only_obsolete_history_and_preserves_future_evidence() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("世代-歷史");
    fs::create_dir(&directory).expect("create generation directory");

    create_generation(&directory, 1, b"one");
    write_fixture_marker(&directory, 1);
    create_generation(&directory, 2, b"two");
    write_fixture_marker(&directory, 2);
    create_generation(&directory, 3, b"three");
    write_fixture_marker(&directory, 3);
    create_generation(&directory, 7, b"future-orphan");
    let obsolete_staging = directory.join(canonical_staging_marker_name(2));
    fs::write(&obsolete_staging, b"obsolete-staging").expect("write obsolete staging");
    let future_staging = directory.join(canonical_staging_marker_name(8));
    fs::write(&future_staging, b"future-staging").expect("write future staging");

    let old1_log = fs::read(generation_path(&directory, 1)).expect("read gen1 log");
    let old1_marker = fs::read(marker_path(&directory, 1)).expect("read gen1 marker");
    let old2_log = fs::read(generation_path(&directory, 2)).expect("read gen2 log");
    let old2_marker = fs::read(marker_path(&directory, 2)).expect("read gen2 marker");
    let old2_staging = fs::read(&obsolete_staging).expect("read staging");
    let future_log = fs::read(generation_path(&directory, 7)).expect("read future orphan");
    let future_staging_bytes = fs::read(&future_staging).expect("read future staging");

    let output = run_cleanup(&directory);
    assert_success("Windows history retirement", &output);
    let summary: Value = serde_json::from_slice(&output.stdout).expect("decode cleanup summary");
    assert_eq!(
        summary["protocol"],
        "append_log_generation_cleanup_windows_v1"
    );
    assert_eq!(summary["authoritative_generation"], 3);
    assert_eq!(
        summary["retired_marker_generation_ids"],
        serde_json::json!([1, 2])
    );
    assert_eq!(summary["retired_generation_ids"], serde_json::json!([1, 2]));
    assert_eq!(
        summary["retired_staging_marker_generation_ids"],
        serde_json::json!([2])
    );
    assert_eq!(
        summary["retained_staging_marker_generation_ids"],
        serde_json::json!([8])
    );
    assert_eq!(
        summary["retained_uncommitted_generation_ids"],
        serde_json::json!([7])
    );

    assert!(!generation_path(&directory, 1).exists());
    assert!(!marker_path(&directory, 1).exists());
    assert!(!generation_path(&directory, 2).exists());
    assert!(!marker_path(&directory, 2).exists());
    assert!(!obsolete_staging.exists());
    assert_eq!(
        fs::read(quarantine_path(&directory, 1, "generation", "log")).expect("read retired gen1"),
        old1_log
    );
    assert_eq!(
        fs::read(quarantine_path(&directory, 1, "commit", "marker")).expect("read retired marker1"),
        old1_marker
    );
    assert_eq!(
        fs::read(quarantine_path(&directory, 2, "generation", "log")).expect("read retired gen2"),
        old2_log
    );
    assert_eq!(
        fs::read(quarantine_path(&directory, 2, "commit", "marker")).expect("read retired marker2"),
        old2_marker
    );
    assert_eq!(
        fs::read(quarantine_path(&directory, 2, "staging-commit", "marker"))
            .expect("read retired staging"),
        old2_staging
    );

    assert_eq!(
        fs::read(generation_path(&directory, 7)).expect("re-read future orphan"),
        future_log
    );
    assert_eq!(
        fs::read(&future_staging).expect("re-read future staging"),
        future_staging_bytes
    );
    let verified = verify_generation_directory(&directory).expect("verify cleaned namespace");
    assert_eq!(verified.summary().authoritative_generation, 3);
    assert_eq!(verified.summary().highest_observed_generation, 8);
}

#[cfg(windows)]
#[test]
fn windows_cleanup_prevalidates_every_quarantine_before_first_move() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, b"old");
    write_fixture_marker(&directory, 1);
    create_generation(&directory, 2, b"authority");
    write_fixture_marker(&directory, 2);

    let collision = quarantine_path(&directory, 1, "generation", "log");
    fs::write(&collision, b"sentinel").expect("write collision sentinel");
    let old_log_before = fs::read(generation_path(&directory, 1)).expect("read old log");
    let old_marker_before = fs::read(marker_path(&directory, 1)).expect("read old marker");

    let output = run_cleanup(&directory);
    assert_failure_contains(&output, "history quarantine target already exists");
    assert_eq!(
        fs::read(generation_path(&directory, 1)).expect("re-read old log"),
        old_log_before
    );
    assert_eq!(
        fs::read(marker_path(&directory, 1)).expect("re-read old marker"),
        old_marker_before
    );
    assert_eq!(fs::read(&collision).expect("read sentinel"), b"sentinel");
}

#[cfg(not(windows))]
#[test]
fn non_windows_companion_fails_before_filesystem_access() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("must-not-exist");
    let output = run_cleanup(&directory);
    assert_failure_contains(&output, "unsupported on this platform");
    assert!(!directory.exists());
}

#[cfg(windows)]
fn create_generation(directory: &Path, id: u64, value: &[u8]) {
    let mut engine = LogEngine::create_new_managed_generation(generation_path(directory, id))
        .expect("create generation");
    engine.put(b"key", value).expect("put generation value");
}

#[cfg(windows)]
fn write_fixture_marker(directory: &Path, id: u64) {
    let generation = generation_path(directory, id);
    let report = LogEngine::verify(&generation).expect("verify fixture generation");
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

#[cfg(windows)]
fn generation_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_generation_name(id))
}

#[cfg(windows)]
fn marker_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_marker_name(id))
}

#[cfg(windows)]
fn quarantine_path(directory: &Path, id: u64, kind: &str, extension: &str) -> PathBuf {
    let parent = directory.parent().expect("generation parent");
    let base = directory.file_name().expect("generation basename");
    let mut name = std::ffi::OsString::from(".");
    name.push(base);
    name.push(format!(".retired-{kind}-{id:020}.{extension}"));
    parent.join(name)
}

fn run_cleanup(directory: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-cleanup-windows"))
        .arg("--directory")
        .arg(directory)
        .output()
        .expect("run Windows generation cleanup")
}

#[cfg(windows)]
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
