use std::fs;
use std::process::{Command, Output};

use db_core::KvEngine;
use db_storage_log::LogEngine;
use serde_json::Value;
use tempfile::tempdir;

#[test]
fn compact_binary_publishes_only_complete_live_state_without_touching_source() {
    let directory = tempdir().expect("temporary directory");
    let source = directory.path().join("source.db");
    let output = directory.path().join("compacted.db");
    {
        let mut engine = LogEngine::create_new(&source).expect("create source log");
        engine.put(b"a", b"one").expect("put a one");
        engine.put(b"a", b"two").expect("put a two");
        engine.put(b"b", b"three").expect("put b");
        engine.delete(b"b").expect("delete b");
        engine.put(b"c", b"").expect("put c");
        engine.delete(b"missing").expect("delete missing");
    }
    let source_before = fs::read(&source).expect("read source before compaction");
    let source_inspection = LogEngine::inspect(&source, true).expect("inspect source");
    assert_eq!(source_inspection.verification.record_count, 6);
    assert_eq!(source_inspection.verification.live_keys, 2);

    let compacted = run_compact(&source, &output);
    assert_success("compact source", &compacted);
    let report: Value = serde_json::from_slice(&compacted.stdout).expect("decode compact report");
    assert_eq!(report["protocol"], "append_log_compact_copy_v1");
    assert_eq!(report["file_format_version"], 1);
    assert_eq!(report["source_record_count"], 6);
    assert_eq!(report["live_keys"], 2);
    assert_eq!(report["compacted_record_count"], 2);
    assert_eq!(report["staging_retained"], false);
    assert!(report["reclaimed_bytes"].as_u64().expect("reclaimed bytes") > 0);

    assert_eq!(
        fs::read(&source).expect("read source after compaction"),
        source_before,
        "compact-copy publication must not mutate the source bytes"
    );
    let output_inspection = LogEngine::inspect(&output, true).expect("inspect compact output");
    assert_eq!(output_inspection.entries, source_inspection.entries);
    assert_eq!(output_inspection.verification.record_count, 2);
    assert!(output_inspection.verification.recoverable_tail.is_none());
    assert!(
        !directory.path().join(".compacted.db.compacting").exists(),
        "successful publication should remove the staging name"
    );

    let mut reopened = LogEngine::open(&output).expect("open compacted log");
    assert_eq!(reopened.get(b"a").expect("get a"), Some(b"two".to_vec()));
    assert_eq!(reopened.get(b"b").expect("get b"), None);
    assert_eq!(reopened.get(b"c").expect("get c"), Some(Vec::new()));
    reopened
        .put(b"d", b"four")
        .expect("append after compaction");
    reopened.reopen().expect("reopen compacted output");
    assert_eq!(reopened.get(b"d").expect("get d"), Some(b"four".to_vec()));

    let existing = directory.path().join("existing.db");
    fs::write(&existing, b"sentinel").expect("write existing output sentinel");
    let existing_result = run_compact(&source, &existing);
    assert_failure_contains(&existing_result, "compaction output already exists");
    assert_eq!(fs::read(&existing).expect("read sentinel"), b"sentinel");

    let staged_output = directory.path().join("staged.db");
    let staged_name = directory.path().join(".staged.db.compacting");
    fs::write(&staged_name, b"orphan-staging").expect("write staging sentinel");
    let staged_result = run_compact(&source, &staged_output);
    assert_failure_contains(&staged_result, "compaction staging path already exists");
    assert!(!staged_output.exists());
    assert_eq!(
        fs::read(&staged_name).expect("read staging sentinel"),
        b"orphan-staging"
    );
}

#[cfg(windows)]
#[test]
fn windows_compact_binary_publishes_unicode_output_through_write_through_move() {
    let directory = tempdir().expect("temporary directory");
    let source = directory.path().join("來源-歷史.db");
    let output = directory.path().join("壓縮-正式.db");
    {
        let mut engine = LogEngine::create_new(&source).expect("create unicode source log");
        engine.put(b"alpha", b"one").expect("put old alpha");
        engine.put(b"alpha", b"two").expect("overwrite alpha");
        engine.put(b"beta", b"three").expect("put beta");
        engine.delete(b"beta").expect("delete beta");
    }

    let source_before = fs::read(&source).expect("read unicode source before compaction");
    let output_result = run_compact(&source, &output);
    assert_success("compact unicode Windows paths", &output_result);
    assert_eq!(
        fs::read(&source).expect("read unicode source after compaction"),
        source_before,
        "Windows durable compact publication must leave source bytes untouched"
    );
    assert!(output.is_file());
    assert!(
        !directory.path().join(".壓縮-正式.db.compacting").exists(),
        "write-through move consumes the Windows staging name"
    );
    let inspection = LogEngine::inspect(&output, true).expect("inspect unicode compact output");
    assert_eq!(inspection.verification.record_count, 1);
    assert_eq!(inspection.verification.live_keys, 1);
    assert_eq!(inspection.entries[0].key.as_slice(), b"alpha");
    assert_eq!(
        inspection.entries[0]
            .value
            .as_ref()
            .map(|value| value.as_slice()),
        Some(b"two".as_slice())
    );
}

#[test]
fn compact_binary_rejects_recoverable_tail_without_repairing_source() {
    let directory = tempdir().expect("temporary directory");
    let source = directory.path().join("tail.db");
    let output = directory.path().join("tail-compacted.db");
    {
        let mut engine = LogEngine::create_new(&source).expect("create source log");
        engine.put(b"a", b"one").expect("put a");
        engine.put(b"b", b"two").expect("put b");
    }
    let length = fs::metadata(&source).expect("source metadata").len();
    fs::OpenOptions::new()
        .write(true)
        .open(&source)
        .expect("open source for truncation")
        .set_len(length - 1)
        .expect("truncate final append");
    let source_before = fs::read(&source).expect("read truncated source");
    assert!(LogEngine::verify(&source)
        .expect("verify recoverable tail")
        .recoverable_tail
        .is_some());

    let result = run_compact(&source, &output);
    assert_failure_contains(&result, "recoverable incomplete final append");
    assert!(!output.exists());
    assert_eq!(
        fs::read(&source).expect("read source after rejected compaction"),
        source_before,
        "compaction must not invoke mutable tail recovery on the source"
    );
}

fn run_compact(source: &std::path::Path, output: &std::path::Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-compact"))
        .arg("--source")
        .arg(source)
        .arg("--output")
        .arg(output)
        .output()
        .expect("run db-lab-log-compact")
}

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
