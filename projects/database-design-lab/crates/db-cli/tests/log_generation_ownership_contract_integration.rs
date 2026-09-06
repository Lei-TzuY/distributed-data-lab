use db_cli::generation_lock::{acquire_generation_writer_lease, GenerationWriterLockError};
use db_core::KvEngine;
use db_storage_log::LogEngine;
use tempfile::tempdir;

#[test]
fn cooperative_writer_lease_excludes_coordinated_writers_without_claiming_filesystem_sandboxing() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    std::fs::create_dir(&directory).expect("create generation directory");

    let lease =
        acquire_generation_writer_lease(&directory).expect("acquire coordinated writer lease");
    assert!(matches!(
        acquire_generation_writer_lease(&directory),
        Err(GenerationWriterLockError::Busy { .. })
    ));

    let raw_generation = directory.join("generation-00000000000000000001.log");
    let mut deliberately_uncoordinated = LogEngine::create_new_managed_generation(&raw_generation)
        .expect("explicit managed raw-path API is outside the cooperative lease contract");
    deliberately_uncoordinated
        .put(b"outside-contract", b"visible")
        .expect("deliberate raw-path mutation");
    drop(deliberately_uncoordinated);

    let mut reopened = LogEngine::open_managed_generation(&raw_generation)
        .expect("raw managed generation remains a valid append-log image");
    assert_eq!(
        reopened
            .get(b"outside-contract")
            .expect("read raw mutation"),
        Some(b"visible".to_vec())
    );
    drop(reopened);

    drop(lease);
    let reacquired = acquire_generation_writer_lease(&directory)
        .expect("coordinated ownership resumes after lease release");
    drop(reacquired);
}
