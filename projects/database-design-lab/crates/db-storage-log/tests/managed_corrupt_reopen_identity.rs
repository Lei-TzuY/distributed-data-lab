use db_core::{DbError, KvEngine};
use db_storage_log::LogEngine;
use tempfile::tempdir;

#[test]
fn managed_failed_reopen_preserves_identity_anchor_against_later_replacement() {
    let directory = tempdir().expect("temporary directory");
    let path = directory.path().join("generation-00000000000000000104.log");
    let corrupted_original = directory
        .path()
        .join("generation-00000000000000000104.corrupted.log");
    let replacement = directory.path().join("generation-00000000000000000105.log");

    let mut engine =
        LogEngine::create_new_managed_generation(&path).expect("create original managed log");
    engine
        .put(b"stable", b"original")
        .expect("write original value");

    let mut corrupted = std::fs::read(&path).expect("read original bytes");
    corrupted[0] ^= 0xff;
    std::fs::write(&path, &corrupted).expect("publish in-place corruption");

    let first_error = engine
        .reopen()
        .expect_err("corrupt managed generation must fail reopen");
    assert!(matches!(first_error, DbError::Corruption { .. }));
    assert!(matches!(engine.get(b"stable"), Err(DbError::Poisoned)));

    {
        let mut other = LogEngine::create_new_managed_generation(&replacement)
            .expect("create replacement managed log");
        other
            .put(b"stable", b"replacement")
            .expect("write replacement value");
    }
    let replacement_bytes = std::fs::read(&replacement).expect("read replacement bytes");

    std::fs::rename(&path, &corrupted_original).expect("move corrupted original aside");
    std::fs::rename(&replacement, &path).expect("publish replacement at managed pathname");

    let second_error = engine
        .reopen()
        .expect_err("failed managed reopen must retain original identity anchor");
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
