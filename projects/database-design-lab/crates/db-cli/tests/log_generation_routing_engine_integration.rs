use std::fs;
use std::path::{Path, PathBuf};

#[cfg(unix)]
use db_cli::generation_compaction::compact_switch_generation_offline;
use db_cli::generation_directory::{
    canonical_generation_name, canonical_marker_name, verify_generation_directory,
};
use db_cli::generation_engine::GenerationLogEngine;
use db_cli::generation_marker::{encode_commit_marker, CommittedPrefix, Crc32Ieee};
#[cfg(unix)]
use db_cli::generation_publication::publish_generation_marker;
use db_core::{DbError, KvEngine};
use db_storage_log::LogEngine;
use tempfile::tempdir;

#[test]
fn routing_engine_drives_crud_and_reopen_through_verified_generation() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    let generation = generation_path(&directory, 1);
    {
        let mut log =
            LogEngine::create_new_managed_generation(&generation).expect("create generation 1");
        log.put(b"a", b"one").expect("put initial value");
        log.put(b"remove", b"value").expect("put removable value");
    }
    write_synthetic_marker(&directory, 1);

    let mut routed = GenerationLogEngine::open(&directory).expect("open routed generation engine");
    assert_eq!(routed.authoritative_generation(), 1);
    assert_eq!(routed.capabilities().name, "append-log-generation-v2");
    assert_eq!(routed.get(b"a").expect("get a"), Some(b"one".to_vec()));
    assert_eq!(
        routed.put(b"a", b"two").expect("overwrite a"),
        Some(b"one".to_vec())
    );
    assert_eq!(
        routed.delete(b"remove").expect("delete key"),
        Some(b"value".to_vec())
    );
    routed.put(b"new", b"value").expect("put new key");
    routed.reopen().expect("reopen routed engine");
    assert_eq!(routed.authoritative_generation(), 1);
    assert_eq!(
        routed.get(b"a").expect("get overwritten a"),
        Some(b"two".to_vec())
    );
    assert_eq!(routed.get(b"remove").expect("get deleted key"), None);
    assert_eq!(
        routed.get(b"new").expect("get new key"),
        Some(b"value".to_vec())
    );
}

#[test]
fn routing_engine_never_rolls_back_after_observing_higher_authority() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"key", b"old")]);
    write_synthetic_marker(&directory, 1);
    create_generation(&directory, 2, &[(b"key", b"new")]);
    write_synthetic_marker(&directory, 2);

    let marker2 = marker_path(&directory, 2);
    let marker2_bytes = fs::read(&marker2).expect("read generation 2 marker");
    let generation1_before = fs::read(generation_path(&directory, 1)).expect("read generation 1");
    let mut routed = GenerationLogEngine::open(&directory).expect("open generation 2");
    assert_eq!(routed.authoritative_generation(), 2);

    fs::remove_file(&marker2).expect("remove generation 2 marker");
    let error = routed
        .put(b"must-not-land-old", b"value")
        .expect_err("marker rollback must fail closed");
    assert!(matches!(error, DbError::Corruption { .. }));
    assert_eq!(
        fs::read(generation_path(&directory, 1)).expect("re-read generation 1"),
        generation1_before,
        "routing failure must not append to the older committed generation"
    );

    fs::write(&marker2, &marker2_bytes).expect("restore generation 2 marker");
    assert!(matches!(routed.get(b"key"), Err(DbError::Poisoned)));
    routed.reopen().expect("reopen after restoring authority");
    assert_eq!(routed.authoritative_generation(), 2);
    assert_eq!(
        routed.get(b"key").expect("get generation 2 value"),
        Some(b"new".to_vec())
    );
}

#[test]
fn malformed_higher_marker_poisoning_never_mutates_old_generation() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"key", b"stable")]);
    write_synthetic_marker(&directory, 1);

    let mut routed = GenerationLogEngine::open(&directory).expect("open generation 1");
    let generation1_before = fs::read(generation_path(&directory, 1)).expect("read generation 1");
    fs::write(marker_path(&directory, 2), b"not-a-valid-marker")
        .expect("write malformed higher marker");

    let error = routed
        .put(b"stale", b"write")
        .expect_err("higher malformed marker must block mutation");
    assert!(matches!(error, DbError::Corruption { .. }));
    assert_eq!(
        fs::read(generation_path(&directory, 1)).expect("re-read generation 1"),
        generation1_before,
        "malformed higher authority must not fall through to the old handle"
    );
    assert!(matches!(routed.get(b"key"), Err(DbError::Poisoned)));
}

#[cfg(unix)]
#[test]
fn existing_routing_handle_adopts_generation_published_by_offline_switch() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");
    create_generation(&directory, 1, &[(b"key", b"before")]);
    publish_generation_marker(&directory, 1).expect("publish generation 1");

    let mut routed = GenerationLogEngine::open(&directory).expect("open routed generation 1");
    routed
        .put(b"before-switch", b"durable")
        .expect("mutate generation 1 through routed handle");
    assert_eq!(routed.authoritative_generation(), 1);

    let switched = compact_switch_generation_offline(&directory).expect("offline compact switch");
    assert_eq!(switched.old_generation, 1);
    assert_eq!(switched.new_generation, 2);
    assert_eq!(
        routed.authoritative_generation(),
        1,
        "handle refresh is lazy"
    );
    let old_after_switch =
        fs::read(generation_path(&directory, 1)).expect("snapshot old generation");

    routed
        .put(b"after-switch", b"new-authority")
        .expect("route first post-switch mutation");
    assert_eq!(routed.authoritative_generation(), 2);
    assert_eq!(
        fs::read(generation_path(&directory, 1)).expect("re-read old generation"),
        old_after_switch,
        "post-switch routed mutation must not append to the stale generation handle"
    );
    let mut generation2 = LogEngine::open_managed_generation(generation_path(&directory, 2))
        .expect("open generation 2");
    assert_eq!(
        generation2
            .get(b"after-switch")
            .expect("get post-switch value"),
        Some(b"new-authority".to_vec())
    );
    assert_eq!(
        generation2
            .get(b"before-switch")
            .expect("get compacted value"),
        Some(b"durable".to_vec())
    );
    let verified = verify_generation_directory(&directory).expect("verify final authority");
    assert_eq!(verified.summary().authoritative_generation, 2);
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
