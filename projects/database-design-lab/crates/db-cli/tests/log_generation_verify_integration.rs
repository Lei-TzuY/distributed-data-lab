use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use db_cli::generation_marker::{crc32_ieee, encode_commit_marker, CommittedPrefix};
use db_core::KvEngine;
use db_storage_log::LogEngine;
use serde_json::Value;
use tempfile::tempdir;

#[test]
fn verifier_selects_highest_commit_and_ignores_higher_uncommitted_orphans() {
    let directory = tempdir().expect("temporary directory");
    create_generation(directory.path(), 1, &[(b"a", b"old")]);
    write_marker_for_current_log(directory.path(), 1);

    create_generation(directory.path(), 2, &[(b"a", b"new"), (b"b", b"three")]);
    let committed_prefix = write_marker_for_current_log(directory.path(), 2);

    fs::remove_file(generation_path(directory.path(), 1)).expect("remove lower generation log");
    let lower_marker = marker_path(directory.path(), 1);
    let mut lower_marker_bytes = fs::read(&lower_marker).expect("read lower marker");
    lower_marker_bytes[0] ^= 0x80;
    fs::write(&lower_marker, lower_marker_bytes).expect("damage lower marker");

    create_generation(directory.path(), 3, &[(b"orphan", b"complete")]);
    fs::write(
        generation_path(directory.path(), 4),
        b"incomplete-or-corrupt-orphan",
    )
    .expect("write corrupt uncommitted orphan");

    let output = run_verify(directory.path());
    assert_success("verify generation directory", &output);
    let summary: Value = serde_json::from_slice(&output.stdout).expect("decode summary");
    assert_eq!(summary["protocol"], "append_log_generation_directory_v3");
    assert_eq!(summary["marker_format_version"], 2);
    assert_eq!(summary["authoritative_generation"], 2);
    assert_eq!(
        summary["authoritative_log"],
        "generation-00000000000000000002.log"
    );
    assert_eq!(summary["highest_observed_generation"], 4);
    assert_eq!(summary["marker_generation_ids"], serde_json::json!([1, 2]));
    assert_eq!(summary["reservation_generation_ids"], serde_json::json!([]));
    assert_eq!(
        summary["uncommitted_generation_ids"],
        serde_json::json!([3, 4])
    );
    assert_eq!(summary["committed_prefix"]["bytes"], committed_prefix.bytes);
    assert_eq!(
        summary["committed_prefix"]["record_count"],
        committed_prefix.record_count
    );
    assert_eq!(summary["committed_prefix_verification"]["record_count"], 2);
    assert!(summary["committed_prefix_verification"]["recoverable_tail"].is_null());
    assert_eq!(summary["log_verification"]["record_count"], 2);
    assert_eq!(summary["log_verification"]["live_keys"], 2);
    assert!(summary["log_verification"]["recoverable_tail"].is_null());
}

#[test]
fn corrupt_highest_marker_never_falls_back_to_older_commit() {
    let directory = tempdir().expect("temporary directory");
    create_generation(directory.path(), 1, &[(b"key", b"old")]);
    write_marker_for_current_log(directory.path(), 1);
    create_generation(directory.path(), 2, &[(b"key", b"new")]);
    write_marker_for_current_log(directory.path(), 2);

    let marker = marker_path(directory.path(), 2);
    let mut bytes = fs::read(&marker).expect("read marker");
    bytes[24] ^= 0x80;
    fs::write(&marker, bytes).expect("corrupt highest marker");

    let output = run_verify(directory.path());
    assert_failure_contains(&output, "highest commit marker checksum mismatch");
}

#[test]
fn missing_highest_committed_log_never_falls_back() {
    let directory = tempdir().expect("temporary directory");
    create_generation(directory.path(), 1, &[(b"key", b"old")]);
    write_marker_for_current_log(directory.path(), 1);
    create_generation(directory.path(), 2, &[(b"key", b"new")]);
    write_marker_for_current_log(directory.path(), 2);
    fs::remove_file(generation_path(directory.path(), 2)).expect("remove highest generation log");

    let output = run_verify(directory.path());
    assert_failure_contains(
        &output,
        "highest committed generation 2 has no generation log",
    );
}

#[test]
fn marker_bound_base_allows_later_recoverable_append_without_repairing_it() {
    let directory = tempdir().expect("temporary directory");
    create_generation(directory.path(), 1, &[(b"a", b"one")]);
    let committed_prefix = write_marker_for_current_log(directory.path(), 1);
    let log = generation_path(directory.path(), 1);

    {
        let mut engine =
            LogEngine::open_managed_generation(&log).expect("open committed generation");
        engine
            .put(b"b", b"two")
            .expect("complete post-commit append");
        engine
            .put(b"c", b"three")
            .expect("final post-commit append");
    }
    let length = fs::metadata(&log).expect("log metadata").len();
    fs::OpenOptions::new()
        .write(true)
        .open(&log)
        .expect("open log for truncation")
        .set_len(length - 1)
        .expect("truncate final append");
    let before = fs::read(&log).expect("read truncated log");

    let output = run_verify(directory.path());
    assert_success("verify marker-bound recoverable tail", &output);
    let summary: Value = serde_json::from_slice(&output.stdout).expect("decode summary");
    assert_eq!(summary["authoritative_generation"], 1);
    assert_eq!(summary["committed_prefix"]["bytes"], committed_prefix.bytes);
    assert_eq!(summary["committed_prefix_verification"]["record_count"], 1);
    assert!(summary["committed_prefix_verification"]["recoverable_tail"].is_null());
    assert!(summary["log_verification"]["recoverable_tail"].is_object());
    assert!(
        summary["log_verification"]["recoverable_tail"]["record_offset"]
            .as_u64()
            .expect("tail offset")
            >= committed_prefix.bytes
    );
    assert_eq!(
        fs::read(&log).expect("read log after verification"),
        before,
        "read-only generation verification must not repair a recoverable tail"
    );
}

#[test]
fn marker_that_binds_an_incomplete_compacted_prefix_fails_closed() {
    let directory = tempdir().expect("temporary directory");
    create_generation(directory.path(), 1, &[(b"a", b"one"), (b"b", b"two")]);
    let log = generation_path(directory.path(), 1);
    let length = fs::metadata(&log).expect("log metadata").len();
    fs::OpenOptions::new()
        .write(true)
        .open(&log)
        .expect("open log for truncation")
        .set_len(length - 1)
        .expect("truncate compacted image");

    let truncated = fs::read(&log).expect("read truncated compacted image");
    let report =
        LogEngine::verify(&log).expect("truncated final append is structurally reportable");
    assert!(report.recoverable_tail.is_some());
    let false_proof = CommittedPrefix {
        bytes: truncated.len() as u64,
        crc32: crc32_ieee(&truncated),
        record_count: report.record_count,
        next_sequence: report.next_sequence,
    };
    write_marker(directory.path(), 1, 1, false_proof);

    let output = run_verify(directory.path());
    assert_failure_contains(&output, "prefix proof failed");
    assert_failure_contains(&output, "ends in a recoverable tail");
}

#[test]
fn marker_prefix_checksum_and_structural_summary_are_reverified() {
    let directory = tempdir().expect("temporary directory");
    create_generation(directory.path(), 1, &[(b"key", b"value")]);
    let correct = proof_for_current_clean_log(directory.path(), 1);

    let mut wrong_crc = correct;
    wrong_crc.crc32 ^= 1;
    write_marker(directory.path(), 1, 1, wrong_crc);
    let checksum_failure = run_verify(directory.path());
    assert_failure_contains(&checksum_failure, "prefix checksum mismatch");

    fs::remove_file(marker_path(directory.path(), 1)).expect("remove wrong checksum marker");
    let mut wrong_summary = correct;
    wrong_summary.record_count = 0;
    wrong_summary.next_sequence = 1;
    write_marker(directory.path(), 1, 1, wrong_summary);
    let summary_failure = run_verify(directory.path());
    assert_failure_contains(&summary_failure, "prefix record count mismatch");
}

#[test]
fn marker_generation_must_match_filename_and_namespace_is_strict() {
    let directory = tempdir().expect("temporary directory");
    create_generation(directory.path(), 1, &[(b"key", b"value")]);
    let proof = proof_for_current_clean_log(directory.path(), 1);
    write_marker(directory.path(), 1, 2, proof);

    let mismatch = run_verify(directory.path());
    assert_failure_contains(&mismatch, "disagrees with filename generation 1");

    fs::remove_file(marker_path(directory.path(), 1)).expect("remove mismatched marker");
    write_marker(directory.path(), 1, 1, proof);
    fs::write(directory.path().join("README.txt"), b"unexpected").expect("write unexpected entry");
    let unexpected = run_verify(directory.path());
    assert_failure_contains(&unexpected, "unexpected generation directory entry");
}

fn create_generation(directory: &Path, id: u64, entries: &[(&[u8], &[u8])]) {
    let mut engine = LogEngine::create_new_managed_generation(generation_path(directory, id))
        .expect("create generation");
    for (key, value) in entries {
        engine.put(key, value).expect("put generation entry");
    }
}

fn write_marker_for_current_log(directory: &Path, id: u64) -> CommittedPrefix {
    let proof = proof_for_current_clean_log(directory, id);
    write_marker(directory, id, id, proof);
    proof
}

fn proof_for_current_clean_log(directory: &Path, id: u64) -> CommittedPrefix {
    let path = generation_path(directory, id);
    let report = LogEngine::verify(&path).expect("verify clean generation for marker proof");
    assert!(
        report.recoverable_tail.is_none(),
        "marker proof fixture must begin from a complete generation"
    );
    let bytes = fs::read(&path).expect("read generation for marker proof");
    assert_eq!(report.file_bytes, bytes.len() as u64);
    CommittedPrefix {
        bytes: report.file_bytes,
        crc32: crc32_ieee(&bytes),
        record_count: report.record_count,
        next_sequence: report.next_sequence,
    }
}

fn write_marker(directory: &Path, filename_id: u64, encoded_id: u64, proof: CommittedPrefix) {
    fs::write(
        marker_path(directory, filename_id),
        encode_commit_marker(encoded_id, proof).expect("encode marker"),
    )
    .expect("write marker");
}

fn generation_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(format!("generation-{id:020}.log"))
}

fn marker_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(format!("commit-{id:020}.marker"))
}

fn run_verify(directory: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-verify"))
        .arg("--directory")
        .arg(directory)
        .output()
        .expect("run generation verifier")
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
