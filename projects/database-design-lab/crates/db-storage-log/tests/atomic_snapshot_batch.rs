use std::fs::OpenOptions;
use std::process::{Command, Output};

use tempfile::tempdir;

fn run(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-log-tx"))
        .args(args)
        .output()
        .expect("run db-log-tx")
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
fn atomic_batch_applies_in_order_and_survives_reopen() {
    let dir = tempdir().expect("tempdir");
    let path = dir.path().join("tx.log");
    let path = path.to_str().expect("utf8 path");

    let output = run(&[
        path,
        "batch",
        "put:61:31",
        "put:61:32",
        "put:62:33",
        "delete:61",
        "put:63:",
    ]);
    assert_success(&output);
    assert_eq!(stdout(&output), "committed 5");

    let output = run(&[path, "get", "61"]);
    assert_success(&output);
    assert_eq!(stdout(&output), "null");

    let output = run(&[path, "get", "62"]);
    assert_success(&output);
    assert_eq!(stdout(&output), "33");

    let output = run(&[path, "get", "63"]);
    assert_success(&output);
    assert_eq!(stdout(&output), "");
}

#[test]
fn incomplete_final_transaction_record_recovers_all_or_none() {
    let dir = tempdir().expect("tempdir");
    let path_buf = dir.path().join("tx.log");
    let path = path_buf.to_str().expect("utf8 path");

    let first = run(&[path, "batch", "put:61:6f6c64", "put:62:6b656570"]);
    assert_success(&first);
    let committed_len = std::fs::metadata(&path_buf).expect("metadata").len();

    let second = run(&[
        path,
        "batch",
        "put:61:6e6577",
        "delete:62",
        "put:63:7468726565",
    ]);
    assert_success(&second);
    let full_len = std::fs::metadata(&path_buf).expect("metadata").len();
    assert!(full_len > committed_len);

    let torn_len = committed_len + (full_len - committed_len) / 2;
    OpenOptions::new()
        .write(true)
        .open(&path_buf)
        .expect("open for truncate")
        .set_len(torn_len)
        .expect("truncate into final transaction record");

    let a = run(&[path, "get", "61"]);
    assert_success(&a);
    assert_eq!(stdout(&a), "6f6c64");

    let b = run(&[path, "get", "62"]);
    assert_success(&b);
    assert_eq!(stdout(&b), "6b656570");

    let c = run(&[path, "get", "63"]);
    assert_success(&c);
    assert_eq!(stdout(&c), "null");

    assert_eq!(
        std::fs::metadata(&path_buf)
            .expect("repaired metadata")
            .len(),
        committed_len,
        "reopen must discard the incomplete final transaction record as one unit"
    );
}

#[test]
fn malformed_batch_is_rejected_before_durable_state_changes() {
    let dir = tempdir().expect("tempdir");
    let path_buf = dir.path().join("tx.log");
    let path = path_buf.to_str().expect("utf8 path");

    let first = run(&[path, "batch", "put:61:31"]);
    assert_success(&first);
    let committed_len = std::fs::metadata(&path_buf).expect("metadata").len();

    let invalid = run(&[path, "batch", "put:not-hex:32"]);
    assert!(!invalid.status.success());
    assert_eq!(
        std::fs::metadata(&path_buf)
            .expect("metadata after rejection")
            .len(),
        committed_len
    );

    let read = run(&[path, "get", "61"]);
    assert_success(&read);
    assert_eq!(stdout(&read), "31");
}
