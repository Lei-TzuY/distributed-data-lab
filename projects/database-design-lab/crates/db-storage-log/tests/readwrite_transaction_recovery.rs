use std::fs::OpenOptions;
use std::process::{Command, Output};

use tempfile::tempdir;

fn run(path: &str, workers: &str) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-log-tx-readwrite"))
        .args([path, workers])
        .output()
        .expect("run db-log-tx-readwrite")
}

fn assert_success(output: &Output) {
    assert!(
        output.status.success(),
        "command failed: status={:?}, stdout={}, stderr={}",
        output.status.code(),
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn stdout(output: &Output) -> String {
    String::from_utf8(output.stdout.clone())
        .expect("utf8 stdout")
        .trim()
        .to_owned()
}

#[test]
fn torn_final_readwrite_transaction_reopens_at_last_commit_boundary() {
    let dir = tempdir().expect("tempdir");
    let path_buf = dir.path().join("readwrite-crash.log");
    let path = path_buf.to_str().expect("utf8 path");

    // First invocation durably initializes counter=0 (tx=1) and increments it to 1 (tx=2).
    let first = run(path, "1");
    assert_success(&first);
    assert_eq!(stdout(&first), "counter=1");
    let committed_len = std::fs::metadata(&path_buf).expect("metadata").len();

    // A second invocation appends tx=3, moving counter from 1 to 2.
    let second = run(path, "1");
    assert_success(&second);
    assert_eq!(stdout(&second), "counter=2");
    let full_len = std::fs::metadata(&path_buf).expect("metadata").len();
    assert!(full_len > committed_len);

    // Model a crash during the final append. Recovery must reject the whole tx=3 record rather
    // than publish any part of it. This exercises the read-decision-write executable end-to-end,
    // not only the lower-level incremental transaction CLI.
    let torn_len = committed_len + (full_len - committed_len) / 2;
    OpenOptions::new()
        .write(true)
        .open(&path_buf)
        .expect("open for truncate")
        .set_len(torn_len)
        .expect("truncate final transaction record");

    let reopened = run(path, "0");
    assert_success(&reopened);
    assert_eq!(stdout(&reopened), "counter=1");
    assert_eq!(
        std::fs::metadata(&path_buf)
            .expect("repaired metadata")
            .len(),
        committed_len,
        "reopen must discard the torn read-write transaction at the append-log boundary"
    );

    // The repaired log must remain writable and preserve contiguous transaction replay.
    let next = run(path, "1");
    assert_success(&next);
    assert_eq!(stdout(&next), "counter=2");
}
