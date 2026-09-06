use db_core::{DbError, KvEngine};
use db_storage_log::LogEngine;
use tempfile::tempdir;

#[test]
fn managed_missing_path_reopen_preserves_identity_anchor_against_later_replacement() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("generation-00000000000000000103.log");
    let original = directory
        .path()
        .join("generation-00000000000000000103.original.log");
    let replacement = directory.path().join("generation-00000000000000000104.log");

    let mut engine =
        LogEngine::create_new_managed_generation(&path).expect("create original managed log");
    engine
        .put(b"stable", b"original")
        .expect("write original value");

    std::fs::rename(&path, &original).expect("temporarily remove managed pathname");
    let first_error = engine
        .reopen()
        .expect_err("missing managed pathname must fail reopen");
    assert!(
        matches!(first_error, DbError::Io(ref error) if error.kind() == std::io::ErrorKind::NotFound)
    );
    assert!(matches!(engine.get(b"stable"), Err(DbError::Poisoned)));

    {
        let mut other = LogEngine::create_new_managed_generation(&replacement)
            .expect("create replacement managed log");
        other
            .put(b"stable", b"replacement")
            .expect("write replacement value");
    }
    let replacement_bytes = std::fs::read(&replacement).expect("read replacement bytes");
    std::fs::rename(&replacement, &path).expect("publish replacement at managed pathname");

    let second_error = engine
        .reopen()
        .expect_err("missing-path failure must retain managed identity anchor");
    assert!(
        second_error
            .to_string()
            .contains("backing file identity changed"),
        "unexpected second reopen error: {second_error}"
    );
    assert!(matches!(engine.get(b"stable"), Err(DbError::Poisoned)));
    assert_eq!(
        std::fs::read(&path).expect("read replacement after rejected managed reopen"),
        replacement_bytes,
        "managed identity rejection must not mutate the replacement file"
    );
}
