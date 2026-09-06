use std::path::Path;
use std::process::{Command, Output};

#[cfg(windows)]
use std::fs;
#[cfg(windows)]
use std::path::PathBuf;

#[cfg(windows)]
use db_cli::generation_directory::{
    canonical_generation_name, canonical_marker_name, canonical_reservation_name,
    canonical_staging_marker_name, verify_generation_directory,
};
#[cfg(windows)]
use db_cli::generation_marker::{crc32_ieee, encode_commit_marker, CommittedPrefix};
#[cfg(windows)]
use db_cli::generation_orphan::inspect_generation_orphan;
#[cfg(windows)]
use db_cli::generation_reservation::reserve_next_generation;
#[cfg(windows)]
use db_core::KvEngine;
#[cfg(windows)]
use db_storage_log::LogEngine;
#[cfg(windows)]
use serde_json::Value;
use tempfile::tempdir;

#[cfg(windows)]
#[test]
fn windows_retirement_moves_orphan_out_of_namespace_and_preserves_frontier() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("世代-資料");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, b"authority");
    write_fixture_marker(&directory, 1);
    let reservation = reserve_next_generation(&directory).expect("reserve orphan generation");
    assert_eq!(reservation.generation, 2);
    create_generation(&directory, 2, b"abandoned");
    let staging = directory.join(canonical_staging_marker_name(2));
    fs::write(&staging, b"staging-evidence").expect("write staging residue");

    let inspection = inspect_generation_orphan(&directory, 2).expect("inspect orphan");
    let orphan_before = fs::read(generation_path(&directory, 2)).expect("read orphan before");
    let staging_before = fs::read(&staging).expect("read staging before");

    let retired = run_retire(
        &directory,
        2,
        1,
        inspection.orphan_fingerprint.bytes,
        inspection.orphan_fingerprint.crc32,
        inspection
            .staging_fingerprint
            .map(|fingerprint| (fingerprint.bytes, fingerprint.crc32)),
        true,
    );
    assert_success("retire Windows orphan", &retired);
    let summary: Value = serde_json::from_slice(&retired.stdout).expect("decode summary");
    assert_eq!(
        summary["protocol"],
        "append_log_generation_orphan_retire_windows_v1"
    );
    assert_eq!(summary["authoritative_generation"], 1);
    assert_eq!(summary["retired_generation"], 2);

    assert!(!generation_path(&directory, 2).exists());
    assert!(!staging.exists());
    assert!(directory.join(canonical_reservation_name(2)).is_file());

    let orphan_quarantine = PathBuf::from(
        summary["orphan_quarantine"]
            .as_str()
            .expect("orphan quarantine path"),
    );
    let staging_quarantine = PathBuf::from(
        summary["staging_quarantine"]
            .as_str()
            .expect("staging quarantine path"),
    );
    assert_eq!(
        fs::read(&orphan_quarantine).expect("read quarantined orphan"),
        orphan_before
    );
    assert_eq!(
        fs::read(&staging_quarantine).expect("read quarantined staging"),
        staging_before
    );

    let verified = verify_generation_directory(&directory).expect("verify retired namespace");
    assert_eq!(verified.summary().authoritative_generation, 1);
    assert_eq!(verified.summary().highest_observed_generation, 2);
    assert_eq!(verified.summary().reservation_generation_ids, vec![2]);
    assert!(verified.summary().uncommitted_generation_ids.is_empty());
    assert!(verified.summary().staging_marker_generation_ids.is_empty());
    assert_eq!(verified.next_generation_id().expect("next id"), 3);
}

#[cfg(windows)]
#[test]
fn windows_retirement_never_overwrites_existing_quarantine_evidence() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, b"authority");
    write_fixture_marker(&directory, 1);
    assert_eq!(
        reserve_next_generation(&directory)
            .expect("reserve orphan")
            .generation,
        2
    );
    create_generation(&directory, 2, b"candidate");
    let inspection = inspect_generation_orphan(&directory, 2).expect("inspect orphan");
    let quarantine = quarantine_path(&directory, 2, "generation", "log");
    fs::write(&quarantine, b"sentinel").expect("write quarantine sentinel");

    let output = run_retire(
        &directory,
        2,
        1,
        inspection.orphan_fingerprint.bytes,
        inspection.orphan_fingerprint.crc32,
        None,
        true,
    );
    assert_failure_contains(&output, "quarantine target already exists");
    assert!(generation_path(&directory, 2).is_file());
    assert_eq!(fs::read(&quarantine).expect("read sentinel"), b"sentinel");
}

#[cfg(not(windows))]
#[test]
fn non_windows_companion_fails_before_filesystem_access() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("must-not-exist");
    let output = run_retire(&directory, 2, 1, 1, 2, None, true);
    assert_failure_contains(&output, "unsupported on this platform");
    assert!(!directory.exists());
}

#[cfg(windows)]
fn create_generation(directory: &Path, id: u64, value: &[u8]) {
    let mut engine = LogEngine::create_new_managed_generation(generation_path(directory, id))
        .expect("create generation");
    engine.put(b"key", value).expect("put value");
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
        directory.join(canonical_marker_name(id)),
        encode_commit_marker(id, proof).expect("encode marker"),
    )
    .expect("write marker fixture");
}

#[cfg(windows)]
fn generation_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_generation_name(id))
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

fn run_retire(
    directory: &Path,
    generation: u64,
    authority: u64,
    orphan_bytes: u64,
    orphan_crc32: u32,
    staging: Option<(u64, u32)>,
    confirm: bool,
) -> Output {
    let mut command = Command::new(env!(
        "CARGO_BIN_EXE_db-lab-log-generation-orphan-retire-windows"
    ));
    command
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
    command.output().expect("run Windows orphan retirement")
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
