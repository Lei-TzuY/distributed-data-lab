use std::fs::OpenOptions;
use std::io::Write;

use db_core::{DbError, KvEngine};
use db_storage_log::LogEngine;
use tempfile::tempdir;

#[test]
fn reopen_rejects_same_file_rollback_below_acknowledged_record_count() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("rollback.log");

    let mut engine = LogEngine::create_new(&path).expect("create log");
    let empty_log = std::fs::read(&path).expect("capture empty valid log");

    engine
        .put(b"stable", b"acknowledged")
        .expect("persist acknowledged record");

    std::fs::write(&path, &empty_log).expect("truncate same backing file to an older valid state");

    let error = engine
        .reopen()
        .expect_err("reopen must reject rollback below acknowledged records");
    assert!(
        matches!(error, DbError::Corruption { .. })
            && error.to_string().contains("record count regressed"),
        "unexpected rollback error: {error}"
    );
    assert!(matches!(engine.get(b"stable"), Err(DbError::Poisoned)));
    assert_eq!(
        std::fs::read(&path).expect("read rolled-back file after rejected reopen"),
        empty_log,
        "rollback rejection must not mutate the backing file"
    );
}

#[test]
fn reopen_rejects_same_count_substitution_of_acknowledged_record() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("substitution.log");
    let replacement_path = directory.path().join("replacement.log");

    let mut engine = LogEngine::create_new(&path).expect("create log");
    engine
        .put(b"stable", b"original")
        .expect("persist acknowledged record");

    let mut replacement = LogEngine::create_new(&replacement_path).expect("create replacement log");
    replacement
        .put(b"stable", b"replaced")
        .expect("persist replacement record");
    drop(replacement);
    let replacement_bytes = std::fs::read(&replacement_path).expect("read valid replacement log");

    std::fs::write(&path, &replacement_bytes)
        .expect("replace acknowledged bytes while preserving the backing file identity");

    let error = engine
        .reopen()
        .expect_err("reopen must reject substitution of an acknowledged record");
    assert!(
        matches!(error, DbError::Corruption { .. })
            && error.to_string().contains("acknowledged record changed"),
        "unexpected substitution error: {error}"
    );
    assert!(matches!(engine.get(b"stable"), Err(DbError::Poisoned)));
    assert_eq!(
        std::fs::read(&path).expect("read substituted file after rejected reopen"),
        replacement_bytes,
        "substitution rejection must not mutate the backing file"
    );
}

#[test]
fn mutation_rejects_valid_external_append_beyond_acknowledged_boundary() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("external-append.log");
    let source_path = directory.path().join("source.log");

    let mut engine = LogEngine::create_new(&path).expect("create log");
    engine.put(b"stable", b"one").expect("persist local record");
    let acknowledged_bytes = std::fs::read(&path).expect("read acknowledged prefix");

    let mut source = LogEngine::create_new(&source_path).expect("create source log");
    source
        .put(b"stable", b"one")
        .expect("persist matching first source record");
    source
        .put(b"foreign", b"two")
        .expect("persist externally appended record");
    drop(source);
    let source_bytes = std::fs::read(&source_path).expect("read source log");
    assert!(source_bytes.starts_with(&acknowledged_bytes));

    let external_suffix = &source_bytes[acknowledged_bytes.len()..];
    let mut backing = OpenOptions::new()
        .append(true)
        .open(&path)
        .expect("open backing file for external append");
    backing
        .write_all(external_suffix)
        .expect("append valid foreign record");
    backing.sync_data().expect("sync external append");
    drop(backing);
    let drifted_bytes = std::fs::read(&path).expect("capture externally drifted log");

    let error = engine
        .put(b"local", b"three")
        .expect_err("mutation must reject external append drift");
    assert!(
        matches!(error, DbError::Corruption { .. })
            && error.to_string().contains("physical EOF changed"),
        "unexpected drift error: {error}"
    );
    assert!(matches!(engine.get(b"stable"), Err(DbError::Poisoned)));
    assert_eq!(
        std::fs::read(&path).expect("read backing file after rejected mutation"),
        drifted_bytes,
        "drift rejection must not append or otherwise mutate the backing file"
    );
}

#[test]
fn mutation_rejects_same_length_substitution_of_acknowledged_prefix() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("live-substitution.log");
    let replacement_path = directory.path().join("replacement.log");

    let mut engine = LogEngine::create_new(&path).expect("create log");
    engine
        .put(b"stable", b"original")
        .expect("persist acknowledged record");
    let original_bytes = std::fs::read(&path).expect("read original acknowledged log");

    let mut replacement = LogEngine::create_new(&replacement_path).expect("create replacement log");
    replacement
        .put(b"stable", b"replaced")
        .expect("persist equal-length replacement record");
    drop(replacement);
    let replacement_bytes = std::fs::read(&replacement_path).expect("read replacement log");
    assert_eq!(
        replacement_bytes.len(),
        original_bytes.len(),
        "regression requires physical EOF to remain unchanged"
    );

    std::fs::write(&path, &replacement_bytes)
        .expect("substitute acknowledged bytes on the same live backing file");
    let substituted_bytes = std::fs::read(&path).expect("capture substituted backing file");

    let error = engine
        .put(b"local", b"next")
        .expect_err("mutation must reject changed acknowledged prefix");
    assert!(
        matches!(error, DbError::Corruption { .. })
            && error.to_string().contains("acknowledged record changed"),
        "unexpected substitution error: {error}"
    );
    assert!(matches!(engine.get(b"stable"), Err(DbError::Poisoned)));
    assert_eq!(
        std::fs::read(&path).expect("read backing file after rejected mutation"),
        substituted_bytes,
        "substitution rejection must not append or otherwise mutate the backing file"
    );
}

#[test]
fn mutation_rejects_external_truncation_below_acknowledged_boundary() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("external-truncation.log");

    let mut engine = LogEngine::create_new(&path).expect("create log");
    let empty_log = std::fs::read(&path).expect("capture empty valid log");
    engine
        .put(b"stable", b"acknowledged")
        .expect("persist acknowledged record");

    std::fs::write(&path, &empty_log)
        .expect("truncate live backing file below acknowledged durable boundary");
    let truncated_bytes = std::fs::read(&path).expect("capture externally truncated log");

    let error = engine
        .put(b"local", b"next")
        .expect_err("mutation must reject external truncation");
    assert!(
        matches!(error, DbError::Corruption { .. })
            && error.to_string().contains("physical EOF changed"),
        "unexpected truncation error: {error}"
    );
    assert!(matches!(engine.get(b"stable"), Err(DbError::Poisoned)));
    assert_eq!(
        std::fs::read(&path).expect("read backing file after rejected mutation"),
        truncated_bytes,
        "truncation rejection must not append or otherwise mutate the backing file"
    );
}
