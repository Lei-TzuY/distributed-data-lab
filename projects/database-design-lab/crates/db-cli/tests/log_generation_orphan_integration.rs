use std::fs;
use std::path::Path;
#[cfg(unix)]
use std::path::PathBuf;
use std::process::{Command, Output};

#[cfg(unix)]
use db_cli::generation_directory::{
    canonical_generation_name, canonical_reservation_name, canonical_staging_marker_name,
    verify_generation_directory,
};
#[cfg(unix)]
use db_cli::generation_lock::acquire_generation_writer_lease;
#[cfg(unix)]
use db_cli::generation_publication::publish_generation_marker;
#[cfg(unix)]
use db_cli::generation_reservation::reserve_next_generation;
#[cfg(unix)]
use db_core::KvEngine;
#[cfg(unix)]
use db_storage_log::LogEngine;
#[cfg(unix)]
use serde_json::Value;
use tempfile::tempdir;

#[cfg(unix)]
#[test]
fn guarded_retirement_removes_reserved_candidate_and_staging_but_preserves_id() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_and_publish(&directory, 1, &[(b"key", b"authority")]);
    let reservation = reserve_next_generation(&directory).expect("reserve orphan id");
    assert_eq!(reservation.generation, 2);
    create_generation(&directory, 2, &[(b"candidate", b"abandoned")]);
    fs::write(staging_path(&directory, 2), b"staging-proof").expect("write staging residue");

    let inspection = inspect_json(&directory, 2);
    assert_eq!(
        inspection["protocol"],
        "append_log_generation_orphan_inspect_v2"
    );
    assert_eq!(inspection["authoritative_generation"], 1);
    assert_eq!(inspection["orphan_generation"], 2);
    assert_eq!(inspection["reservation"], canonical_reservation_name(2));
    let orphan_bytes = inspection["orphan_fingerprint"]["bytes"]
        .as_u64()
        .expect("orphan bytes");
    let orphan_crc = inspection["orphan_fingerprint"]["crc32"]
        .as_u64()
        .expect("orphan crc") as u32;
    let staging_bytes = inspection["staging_fingerprint"]["bytes"]
        .as_u64()
        .expect("staging bytes");
    let staging_crc = inspection["staging_fingerprint"]["crc32"]
        .as_u64()
        .expect("staging crc") as u32;

    let unconfirmed = run_retire(
        &directory,
        2,
        1,
        orphan_bytes,
        orphan_crc,
        Some((staging_bytes, staging_crc)),
        false,
    );
    assert_failure_contains(
        &unconfirmed,
        "requires --confirm-generation-builder-stopped",
    );
    assert!(generation_path(&directory, 2).is_file());
    assert!(staging_path(&directory, 2).is_file());

    let retired = run_retire(
        &directory,
        2,
        1,
        orphan_bytes,
        orphan_crc,
        Some((staging_bytes, staging_crc)),
        true,
    );
    assert_success("retire reserved orphan", &retired);
    let summary: Value = serde_json::from_slice(&retired.stdout).expect("decode retirement");
    assert_eq!(
        summary["protocol"],
        "append_log_generation_orphan_retire_unix_v2"
    );
    assert_eq!(summary["retired_generation"], 2);
    assert_eq!(summary["reservation"], canonical_reservation_name(2));
    assert!(!generation_path(&directory, 2).exists());
    assert!(!staging_path(&directory, 2).exists());
    assert!(reservation_path(&directory, 2).is_file());

    let verified = verify_generation_directory(&directory).expect("verify after retirement");
    assert_eq!(verified.summary().authoritative_generation, 1);
    assert_eq!(verified.summary().highest_observed_generation, 2);
    assert_eq!(verified.summary().reservation_generation_ids, vec![2]);
    assert!(verified.summary().uncommitted_generation_ids.is_empty());
    assert!(verified.summary().staging_marker_generation_ids.is_empty());
    assert_eq!(verified.next_generation_id().expect("next generation"), 3);
}

#[cfg(unix)]
#[test]
fn retirement_requires_durable_reservation_and_exact_inspected_state() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_and_publish(&directory, 1, &[(b"key", b"authority")]);

    create_generation(&directory, 5, &[(b"candidate", b"unreserved")]);
    let unreserved = run_inspect(&directory, 5);
    assert_failure_contains(&unreserved, "has no durable reservation");
    fs::remove_file(generation_path(&directory, 5)).expect("remove unreserved fixture");

    let reservation = reserve_next_generation(&directory).expect("reserve candidate");
    assert_eq!(reservation.generation, 2);
    create_generation(&directory, 2, &[(b"candidate", b"before")]);
    let inspection = inspect_json(&directory, 2);
    let orphan_bytes = inspection["orphan_fingerprint"]["bytes"]
        .as_u64()
        .expect("orphan bytes");
    let orphan_crc = inspection["orphan_fingerprint"]["crc32"]
        .as_u64()
        .expect("orphan crc") as u32;

    {
        let mut engine = LogEngine::open_managed_generation(generation_path(&directory, 2))
            .expect("open candidate");
        engine.put(b"late", b"change").expect("mutate candidate");
    }
    let changed = run_retire(&directory, 2, 1, orphan_bytes, orphan_crc, None, true);
    assert_failure_contains(&changed, "fingerprint changed");
    assert!(generation_path(&directory, 2).is_file());
    assert!(reservation_path(&directory, 2).is_file());

    let current = inspect_json(&directory, 2);
    let current_bytes = current["orphan_fingerprint"]["bytes"]
        .as_u64()
        .expect("current bytes");
    let current_crc = current["orphan_fingerprint"]["crc32"]
        .as_u64()
        .expect("current crc") as u32;
    fs::write(staging_path(&directory, 2), b"appeared-after-inspect")
        .expect("write new staging residue");
    let staging_changed = run_retire(&directory, 2, 1, current_bytes, current_crc, None, true);
    assert_failure_contains(&staging_changed, "staging-marker state changed");
    assert!(generation_path(&directory, 2).is_file());
    assert!(staging_path(&directory, 2).is_file());
}

#[cfg(unix)]
#[test]
fn held_writer_lease_blocks_retirement_without_deleting_evidence() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_and_publish(&directory, 1, &[(b"key", b"authority")]);
    let reservation = reserve_next_generation(&directory).expect("reserve candidate");
    assert_eq!(reservation.generation, 2);
    create_generation(&directory, 2, &[(b"candidate", b"blocked")]);
    let inspection = inspect_json(&directory, 2);
    let bytes = inspection["orphan_fingerprint"]["bytes"]
        .as_u64()
        .expect("orphan bytes");
    let crc = inspection["orphan_fingerprint"]["crc32"]
        .as_u64()
        .expect("orphan crc") as u32;

    let lease = acquire_generation_writer_lease(&directory).expect("hold writer lease");
    let blocked = run_retire(&directory, 2, 1, bytes, crc, None, true);
    assert_failure_contains(&blocked, "already held or stale");
    assert!(generation_path(&directory, 2).is_file());
    assert!(reservation_path(&directory, 2).is_file());
    drop(lease);
}

#[cfg(not(unix))]
#[test]
fn unsupported_retirement_fails_before_filesystem_access() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("must-not-be-created");
    assert!(!directory.exists());

    let output = run_retire(&directory, 5, 1, 123, 456, None, true);
    assert_failure_contains(&output, "unsupported on this platform");
    assert!(
        !directory.exists(),
        "unsupported retirement must fail before touching the supplied path"
    );
}

#[cfg(unix)]
fn inspect_json(directory: &Path, generation: u64) -> Value {
    let output = run_inspect(directory, generation);
    assert_success("inspect orphan", &output);
    serde_json::from_slice(&output.stdout).expect("decode orphan inspection")
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
fn staging_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_staging_marker_name(id))
}

#[cfg(unix)]
fn reservation_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_reservation_name(id))
}

fn run_inspect(directory: &Path, generation: u64) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-orphan"))
        .arg("inspect")
        .arg("--directory")
        .arg(directory)
        .arg("--generation")
        .arg(generation.to_string())
        .output()
        .expect("run orphan inspection")
}

fn run_retire(
    directory: &Path,
    generation: u64,
    authority: u64,
    orphan_bytes: u64,
    orphan_crc32: u32,
    staging: Option<(u64, u32)>,
    confirm: bool,
) -> Output {
    let mut command = Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-orphan"));
    command
        .arg("retire")
        .arg("--directory")
        .arg(directory)
        .arg("--generation")
        .arg(generation.to_string())
        .arg("--expected-authority")
        .arg(authority.to_string())
        .arg("--expected-orphan-bytes")
        .arg(orphan_bytes.to_string())
        .arg("--expected-orphan-crc32")
        .arg(orphan_crc32.to_string());
    if let Some((bytes, crc32)) = staging {
        command
            .arg("--expected-staging-bytes")
            .arg(bytes.to_string())
            .arg("--expected-staging-crc32")
            .arg(crc32.to_string());
    }
    if confirm {
        command.arg("--confirm-generation-builder-stopped");
    }
    command.output().expect("run orphan retirement")
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
