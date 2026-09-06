use std::collections::BTreeMap;
use std::process::{Command, Output};

use tempfile::tempdir;

fn run(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-log-tx-serialized"))
        .args(args)
        .output()
        .expect("run db-log-tx-serialized")
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
fn concurrent_cli_serializes_batches_and_reopens_committed_order() {
    let dir = tempdir().expect("tempdir");
    let path_buf = dir.path().join("serialized.log");
    let path = path_buf.to_str().expect("utf8 path");

    let output = run(&[
        path,
        "concurrent",
        "put:78:6f6e65,put:61:31",
        "put:78:74776f,delete:61",
        "put:62:33,put:78:7468726565",
    ]);
    assert_success(&output);

    let mut worker_tx = BTreeMap::new();
    for line in stdout(&output).lines() {
        let fields = line.split_whitespace().collect::<Vec<_>>();
        assert_eq!(fields.len(), 4, "unexpected output line: {line}");
        let worker = fields[0]
            .strip_prefix("worker=")
            .expect("worker prefix")
            .parse::<usize>()
            .expect("worker id");
        let tx_id = fields[3]
            .strip_prefix("tx=")
            .expect("tx prefix")
            .parse::<u64>()
            .expect("tx id");
        worker_tx.insert(worker, tx_id);
    }
    assert_eq!(worker_tx.len(), 3);
    let mut ids = worker_tx.values().copied().collect::<Vec<_>>();
    ids.sort_unstable();
    assert_eq!(ids, vec![1, 2, 3]);

    let last_x_worker = worker_tx
        .iter()
        .max_by_key(|(_, tx_id)| *tx_id)
        .map(|(worker, _)| *worker)
        .expect("last x writer");
    let expected_x = match last_x_worker {
        0 => "6f6e65",
        1 => "74776f",
        2 => "7468726565",
        other => panic!("unexpected worker {other}"),
    };

    let x = run(&[path, "get", "78"]);
    assert_success(&x);
    assert_eq!(stdout(&x), expected_x);

    let b = run(&[path, "get", "62"]);
    assert_success(&b);
    assert_eq!(stdout(&b), "33");

    let a = run(&[path, "get", "61"]);
    assert_success(&a);
    let expected_a = if worker_tx[&0] > worker_tx[&1] {
        "31"
    } else {
        "null"
    };
    assert_eq!(stdout(&a), expected_a);
}
