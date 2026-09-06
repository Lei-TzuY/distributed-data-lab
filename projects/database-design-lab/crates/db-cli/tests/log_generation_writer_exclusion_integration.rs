use std::fs;
use std::io;
use std::path::{Path, PathBuf};
#[cfg(unix)]
use std::process::Command;

#[cfg(unix)]
use db_cli::generation_compaction::{
    compact_switch_generation_offline, OfflineGenerationCompactSwitchError,
};
use db_cli::generation_directory::{
    canonical_generation_name, canonical_marker_name, verify_generation_directory,
};
use db_cli::generation_engine::GenerationLogEngine;
use db_cli::generation_lock::{acquire_generation_writer_lease, GenerationWriterLockError};
use db_cli::generation_marker::{encode_commit_marker, CommittedPrefix, Crc32Ieee};
#[cfg(unix)]
use db_cli::generation_publication::publish_generation_marker;
#[cfg(unix)]
use db_cli::generation_reservation::GenerationReservationError;
use db_core::{DbError, KvEngine};
use db_storage_log::LogEngine;
use tempfile::tempdir;

#[test]
fn routed_mutation_cannot_cross_an_existing_writer_lease() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"key", b"before")]);
    write_synthetic_marker(&directory, 1);

    let mut routed = GenerationLogEngine::open(&directory).expect("open routed engine");
    let before = fs::read(generation_path(&directory, 1)).expect("snapshot generation 1");
    let lease = acquire_generation_writer_lease(&directory).expect("hold external writer lease");

    let error = routed
        .put(b"blocked", b"value")
        .expect_err("routed mutation must not cross another lease");
    assert!(matches!(
        error,
        DbError::Io(ref source) if source.kind() == io::ErrorKind::WouldBlock
    ));
    assert_eq!(
        fs::read(generation_path(&directory, 1)).expect("re-read generation 1"),
        before,
        "blocked routed mutation must not append any bytes"
    );

    drop(lease);
    routed
        .put(b"after-release", b"value")
        .expect("mutation after lease release");
    assert_eq!(
        routed
            .get(b"after-release")
            .expect("read released mutation"),
        Some(b"value".to_vec())
    );
}

#[test]
fn stale_writer_lock_fails_closed_without_being_stolen() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"key", b"stable")]);
    write_synthetic_marker(&directory, 1);

    let lease = acquire_generation_writer_lease(&directory).expect("derive writer lock");
    let lock_path = lease.lock_path().to_path_buf();
    drop(lease);
    fs::write(&lock_path, b"stale-lock-evidence").expect("write stale lock");

    let error =
        GenerationLogEngine::open(&directory).expect_err("stale lock must block routed open");
    assert!(matches!(
        error,
        DbError::Io(ref source) if source.kind() == io::ErrorKind::WouldBlock
    ));
    assert_eq!(
        fs::read(&lock_path).expect("read stale lock"),
        b"stale-lock-evidence",
        "routed open must not steal or rewrite a stale lock"
    );
}

#[cfg(unix)]
#[test]
fn standalone_publisher_cli_cannot_cross_an_existing_writer_lease() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"key", b"value")]);

    let lease = acquire_generation_writer_lease(&directory).expect("hold writer lease");
    let output = Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-publish"))
        .arg("--directory")
        .arg(&directory)
        .arg("--generation")
        .arg("1")
        .output()
        .expect("run publisher CLI");
    assert!(
        !output.status.success(),
        "publisher unexpectedly crossed lease"
    );
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("writer lock is already held or stale"),
        "unexpected publisher stderr: {stderr}"
    );
    assert!(!marker_path(&directory, 1).exists());
    drop(lease);
}

#[cfg(unix)]
#[test]
fn compact_switch_cannot_reserve_through_held_lease_and_starts_cleanly_after_release() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"a", b"one"), (b"b", b"two")]);
    publish_generation_marker(&directory, 1).expect("publish generation 1");

    let lease = acquire_generation_writer_lease(&directory).expect("hold competing writer lease");
    let error = compact_switch_generation_offline(&directory)
        .expect_err("held lease must stop reservation before candidate construction");
    assert!(matches!(
        error,
        OfflineGenerationCompactSwitchError::Reservation(GenerationReservationError::Lock(
            GenerationWriterLockError::Busy { .. }
        ))
    ));
    assert!(!generation_path(&directory, 2).exists());
    assert!(!marker_path(&directory, 2).exists());
    let still_old = verify_generation_directory(&directory).expect("verify old authority");
    assert_eq!(still_old.summary().authoritative_generation, 1);
    assert!(still_old.summary().reservation_generation_ids.is_empty());

    drop(lease);
    let switched = compact_switch_generation_offline(&directory).expect("retry compact switch");
    assert_eq!(
        switched.new_generation, 2,
        "a lease-blocked reservation creates no frontier evidence, so retry may reserve generation 2"
    );
    let verified = verify_generation_directory(&directory).expect("verify new authority");
    assert_eq!(verified.summary().authoritative_generation, 2);
    assert_eq!(verified.summary().reservation_generation_ids, vec![2]);
}

fn create_generation(directory: &Path, id: u64, entries: &[(&[u8], &[u8])]) {
    let mut log = LogEngine::create_new_managed_generation(generation_path(directory, id))
        .expect("create generation");
    for (key, value) in entries {
        log.put(key, value).expect("put generation entry");
    }
}

fn write_synthetic_marker(directory: &Path, id: u64) {
    let generation = generation_path(directory, id);
    let report = LogEngine::verify(&generation).expect("verify generation for marker");
    assert!(report.recoverable_tail.is_none());
    assert_eq!(report.file_bytes, report.valid_bytes);
    let bytes = fs::read(&generation).expect("read generation for marker CRC");
    let mut crc = Crc32Ieee::new();
    crc.update(&bytes);
    let marker = encode_commit_marker(
        id,
        CommittedPrefix {
            bytes: report.file_bytes,
            crc32: crc.finalize(),
            record_count: report.record_count,
            next_sequence: report.next_sequence,
        },
    )
    .expect("encode synthetic marker");
    fs::write(marker_path(directory, id), marker).expect("write synthetic marker");
}

fn generation_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_generation_name(id))
}

fn marker_path(directory: &Path, id: u64) -> PathBuf {
    directory.join(canonical_marker_name(id))
}
