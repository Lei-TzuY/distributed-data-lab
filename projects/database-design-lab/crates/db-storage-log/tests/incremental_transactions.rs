use std::fs::OpenOptions;
use std::process::{Command, Output};

use tempfile::tempdir;

fn run(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-log-tx-incremental"))
        .args(args)
        .output()
        .expect("run db-log-tx-incremental")
}

fn run_snapshot(args: &[&str]) -> Output {
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
fn incremental_batches_replay_in_order_across_reopen() {
    let dir = tempdir().expect("tempdir");
    let path_buf = dir.path().join("tx-v2.log");
    let path = path_buf.to_str().expect("utf8 path");

    let first = run(&[
        path,
        "batch",
        "put:61:6f6e65",
        "put:62:74776f",
        "put:61:7468726565",
    ]);
    assert_success(&first);
    assert_eq!(stdout(&first), "committed 3 tx=1");

    let second = run(&[path, "batch", "delete:62", "put:63:666f7572"]);
    assert_success(&second);
    assert_eq!(stdout(&second), "committed 2 tx=2");

    let a = run(&[path, "get", "61"]);
    assert_success(&a);
    assert_eq!(stdout(&a), "7468726565");

    let b = run(&[path, "get", "62"]);
    assert_success(&b);
    assert_eq!(stdout(&b), "null");

    let c = run(&[path, "get", "63"]);
    assert_success(&c);
    assert_eq!(stdout(&c), "666f7572");
}

#[test]
fn incomplete_final_incremental_transaction_recovers_all_or_none() {
    let dir = tempdir().expect("tempdir");
    let path_buf = dir.path().join("tx-v2.log");
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
        "reopen must discard the incomplete final transaction as one append-log record"
    );
}

#[test]
fn later_commit_growth_depends_on_mutation_set_not_live_snapshot() {
    let dir = tempdir().expect("tempdir");
    let path_buf = dir.path().join("tx-v2.log");
    let path = path_buf.to_str().expect("utf8 path");
    let large = "78".repeat(8 * 1024);
    let first_arg = format!("put:626967:{large}");

    let first = run(&[path, "batch", &first_arg]);
    assert_success(&first);
    let after_large = std::fs::metadata(&path_buf).expect("metadata").len();

    let second = run(&[path, "batch", "put:736d616c6c:31"]);
    assert_success(&second);
    let after_small = std::fs::metadata(&path_buf).expect("metadata").len();
    let second_append = after_small - after_large;

    assert!(
        second_append < 512,
        "small transaction unexpectedly rewrote live snapshot: appended {second_append} bytes"
    );

    let big = run(&[path, "get", "626967"]);
    assert_success(&big);
    assert_eq!(stdout(&big).len(), large.len());
}

#[test]
fn incremental_cli_rejects_legacy_snapshot_database_without_mutating_it() {
    let dir = tempdir().expect("tempdir");
    let path_buf = dir.path().join("legacy.log");
    let path = path_buf.to_str().expect("utf8 path");

    let legacy = run_snapshot(&[path, "batch", "put:61:31"]);
    assert_success(&legacy);
    let before = std::fs::read(&path_buf).expect("legacy bytes");

    let v2 = run(&[path, "get", "61"]);
    assert!(!v2.status.success());
    assert!(
        String::from_utf8_lossy(&v2.stderr).contains("legacy snapshot transaction database"),
        "unexpected stderr: {}",
        String::from_utf8_lossy(&v2.stderr)
    );
    assert_eq!(
        std::fs::read(&path_buf).expect("bytes after rejection"),
        before
    );
}
