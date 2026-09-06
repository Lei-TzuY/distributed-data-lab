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
fn unix_publisher_commits_clean_generation_and_reader_ignores_staging_residue() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"a", b"one")]);

    let published = run_publish(&directory, 1);
    assert_success("publish generation 1", &published);
    let publication: Value = serde_json::from_slice(&published.stdout).expect("decode publication");
    assert_eq!(
        publication["protocol"],
        "append_log_generation_marker_publication_unix_v1"
    );
    assert_eq!(publication["marker_format_version"], 2);
    assert_eq!(publication["generation"], 1);
    assert_eq!(publication["committed_prefix"]["record_count"], 1);
    assert_eq!(publication["staging_retained"], false);
    assert!(marker_path(&directory, 1).is_file());
    assert!(!staging_marker_path(&directory, 1).exists());

    {
        let mut engine = LogEngine::open_managed_generation(generation_path(&directory, 1))
            .expect("open committed log");
        engine.put(b"b", b"two").expect("post-commit append");
    }

    fs::write(staging_marker_path(&directory, 2), b"crash residue")
        .expect("write non-authoritative staging residue");
    let verified = run_verify(&directory);
    assert_success("verify published generation", &verified);
    let summary: Value = serde_json::from_slice(&verified.stdout).expect("decode verifier summary");
    assert_eq!(summary["authoritative_generation"], 1);
    assert_eq!(summary["highest_observed_generation"], 2);
    assert_eq!(
        summary["staging_marker_generation_ids"],
        serde_json::json!([2])
    );
    assert_eq!(summary["committed_prefix_verification"]["record_count"], 1);
    assert_eq!(summary["log_verification"]["record_count"], 2);
}

#[cfg(unix)]
#[test]
fn unix_publisher_advances_monotonically_and_never_overwrites_existing_marker() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"key", b"old")]);
    assert_success("publish generation 1", &run_publish(&directory, 1));

    let original_marker = fs::read(marker_path(&directory, 1)).expect("read generation 1 marker");
    let retry = run_publish(&directory, 1);
    assert_failure_contains(&retry, "not newer than every existing committed generation");
    assert_eq!(
        fs::read(marker_path(&directory, 1)).expect("re-read generation 1 marker"),
        original_marker,
        "retry must not overwrite an existing commit marker"
    );

    create_generation(&directory, 2, &[(b"key", b"new"), (b"next", b"value")]);
    assert_success("publish generation 2", &run_publish(&directory, 2));
    let verified = run_verify(&directory);
    assert_success("verify generation 2", &verified);
    let summary: Value = serde_json::from_slice(&verified.stdout).expect("decode verifier summary");
    assert_eq!(summary["authoritative_generation"], 2);
    assert_eq!(summary["marker_generation_ids"], serde_json::json!([1, 2]));
}

#[cfg(unix)]
#[test]
fn unix_publisher_rejects_recoverable_source_before_creating_marker() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"a", b"one"), (b"b", b"two")]);
    let log = generation_path(&directory, 1);
    let length = fs::metadata(&log).expect("log metadata").len();
    fs::OpenOptions::new()
        .write(true)
        .open(&log)
        .expect("open log for truncation")
        .set_len(length - 1)
        .expect("truncate final append");

    let output = run_publish(&directory, 1);
    assert_failure_contains(&output, "complete clean append-log image");
    assert!(!marker_path(&directory, 1).exists());
    assert!(!staging_marker_path(&directory, 1).exists());
}

#[cfg(unix)]
#[test]
fn unix_publisher_rejects_unexpected_namespace_entries() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"key", b"value")]);
    fs::write(directory.join("README.txt"), b"unexpected").expect("write unexpected entry");

    let output = run_publish(&directory, 1);
    assert_failure_contains(&output, "unexpected generation directory entry");
    assert!(!marker_path(&directory, 1).exists());
}

#[cfg(not(unix))]
#[test]
fn unsupported_platform_fails_before_writing_any_marker() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");

    let output = run_publish(&directory, 1);
    assert_failure_contains(&output, "unsupported on this platform");
    let entries = fs::read_dir(&directory)
        .expect("read generation directory")
        .count();
    assert_eq!(
        entries, 0,
        "unsupported publisher must not write any artifact"
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

fn run_publish(directory: &Path, id: u64) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-publish"))
        .arg("--directory")
        .arg(directory)
        .arg("--generation")
        .arg(id.to_string())
        .output()
        .expect("run generation marker publisher")
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

fn assert_failure_contains(output: &Output, needle: &str) {
    assert!(!output.status.success(), "command unexpectedly succeeded");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains(needle),
        "stderr did not contain {needle:?}:\n{stderr}"
    );
}
