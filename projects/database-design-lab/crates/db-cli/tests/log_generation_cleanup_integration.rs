use std::fs;
use std::path::Path;
#[cfg(unix)]
use std::path::PathBuf;
use std::process::{Command, Output};

#[cfg(unix)]
use db_cli::generation_directory::{
    canonical_generation_name, canonical_marker_name, canonical_staging_marker_name,
    verify_generation_directory,
};
#[cfg(unix)]
use db_cli::generation_lock::acquire_generation_writer_lease;
#[cfg(unix)]
use db_cli::generation_publication::publish_generation_marker;
#[cfg(unix)]
use db_core::KvEngine;
#[cfg(unix)]
use db_storage_log::LogEngine;
#[cfg(unix)]
use serde_json::Value;
use tempfile::tempdir;

#[cfg(unix)]
#[test]
fn cleanup_removes_only_permanently_obsolete_history_and_is_idempotent() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");

    create_and_publish(&directory, 1, &[(b"key", b"one")]);
    create_and_publish(&directory, 2, &[(b"key", b"two"), (b"old", b"value")]);
    create_and_publish(&directory, 3, &[(b"key", b"three"), (b"live", b"value")]);
    create_generation(&directory, 7, &[(b"future", b"orphan")]);
    fs::write(staging_marker_path(&directory, 2), b"obsolete staging")
        .expect("write obsolete staging marker");
    fs::write(staging_marker_path(&directory, 8), b"frontier staging")
        .expect("write higher staging marker");

    let authoritative_log = generation_path(&directory, 3);
    let authoritative_marker = marker_path(&directory, 3);
    let orphan = generation_path(&directory, 7);
    let frontier_staging = staging_marker_path(&directory, 8);
    let authoritative_log_before = fs::read(&authoritative_log).expect("read authority log");
    let authoritative_marker_before =
        fs::read(&authoritative_marker).expect("read authority marker");
    let orphan_before = fs::read(&orphan).expect("read orphan");
    let frontier_staging_before = fs::read(&frontier_staging).expect("read frontier staging");

    let output = run_cleanup(&directory);
    assert_success("cleanup obsolete history", &output);
    let summary: Value = serde_json::from_slice(&output.stdout).expect("decode cleanup summary");
    assert_eq!(summary["protocol"], "append_log_generation_cleanup_unix_v1");
    assert_eq!(summary["authoritative_generation"], 3);
    assert_eq!(
        summary["removed_marker_generation_ids"],
        serde_json::json!([1, 2])
    );
    assert_eq!(summary["removed_generation_ids"], serde_json::json!([1, 2]));
    assert_eq!(
        summary["removed_staging_marker_generation_ids"],
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
    assert!(!staging_marker_path(&directory, 2).exists());
    assert_eq!(
        fs::read(&authoritative_log).expect("re-read authority log"),
        authoritative_log_before
    );
    assert_eq!(
        fs::read(&authoritative_marker).expect("re-read authority marker"),
        authoritative_marker_before
    );
    assert_eq!(fs::read(&orphan).expect("re-read orphan"), orphan_before);
    assert_eq!(
        fs::read(&frontier_staging).expect("re-read frontier staging"),
        frontier_staging_before,
        "higher staging id must remain as allocation-frontier evidence"
    );

    let verified = verify_generation_directory(&directory).expect("verify cleaned directory");
    assert_eq!(verified.summary().authoritative_generation, 3);
    assert_eq!(verified.summary().highest_observed_generation, 8);

    let second = run_cleanup(&directory);
    assert_success("repeat cleanup", &second);
    let second: Value = serde_json::from_slice(&second.stdout).expect("decode repeat summary");
    assert_eq!(
        second["removed_marker_generation_ids"],
        serde_json::json!([])
    );
    assert_eq!(second["removed_generation_ids"], serde_json::json!([]));
    assert_eq!(
        second["removed_staging_marker_generation_ids"],
        serde_json::json!([])
    );
    assert_eq!(
        second["retained_staging_marker_generation_ids"],
        serde_json::json!([8])
    );
    assert_eq!(
        second["retained_uncommitted_generation_ids"],
        serde_json::json!([7])
    );
}

#[cfg(unix)]
#[test]
fn cleanup_completes_safe_lower_generation_partial_residue() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");

    create_and_publish(&directory, 1, &[(b"a", b"one")]);
    create_and_publish(&directory, 2, &[(b"a", b"two")]);
    create_and_publish(&directory, 3, &[(b"a", b"three")]);

    fs::remove_file(marker_path(&directory, 1)).expect("simulate marker-first cleanup of gen1");
    fs::remove_file(generation_path(&directory, 2)).expect("simulate partial gen2 residue");

    let output = run_cleanup(&directory);
    assert_success("cleanup partial residue", &output);
    let summary: Value = serde_json::from_slice(&output.stdout).expect("decode cleanup summary");
    assert_eq!(
        summary["removed_marker_generation_ids"],
        serde_json::json!([2])
    );
    assert_eq!(summary["removed_generation_ids"], serde_json::json!([1]));
    assert!(!generation_path(&directory, 1).exists());
    assert!(!marker_path(&directory, 2).exists());
    assert!(generation_path(&directory, 3).is_file());
    assert!(marker_path(&directory, 3).is_file());
    assert_eq!(
        verify_generation_directory(&directory)
            .expect("verify authority after partial cleanup")
            .summary()
            .authoritative_generation,
        3
    );
}

#[cfg(unix)]
#[test]
fn held_writer_lease_blocks_cleanup_without_removing_history() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_and_publish(&directory, 1, &[(b"a", b"one")]);
    create_and_publish(&directory, 2, &[(b"a", b"two")]);

    let lease = acquire_generation_writer_lease(&directory).expect("hold writer lease");
    let output = run_cleanup(&directory);
    assert_failure_contains(&output, "already held or stale");
    assert!(generation_path(&directory, 1).is_file());
    assert!(marker_path(&directory, 1).is_file());
    assert!(generation_path(&directory, 2).is_file());
    assert!(marker_path(&directory, 2).is_file());
    drop(lease);

    assert_success("cleanup after lease release", &run_cleanup(&directory));
    assert!(!generation_path(&directory, 1).exists());
    assert!(!marker_path(&directory, 1).exists());
}

#[cfg(not(unix))]
#[test]
fn unsupported_platform_fails_before_filesystem_access() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("must-not-be-created");
    assert!(!directory.exists());

    let output = run_cleanup(&directory);
    assert_failure_contains(&output, "unsupported on this platform");
    assert!(
        !directory.exists(),
        "unsupported cleanup must fail before touching the supplied path"
    );
}

#[cfg(unix)]
fn create_and_publish(directory: &Path, id: u64, entries: &[(&[u8], &[u8])]) {
    create_generation(directory, id, entries);
    publish_generation_marker(directory, id).expect("publish generation marker");
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
    directory.join(canonical_generation_name(id))
}

#[cfg(unix)]
fn marker_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_marker_name(id))
}

#[cfg(unix)]
fn staging_marker_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_staging_marker_name(id))
}

fn run_cleanup(directory: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-cleanup"))
        .arg("--directory")
        .arg(directory)
        .output()
        .expect("run generation cleanup")
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
