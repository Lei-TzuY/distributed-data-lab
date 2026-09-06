use std::fs;
use std::path::Path;
use std::process::{Command, Output};

use db_cli::generation_lock::{acquire_generation_writer_lease, generation_writer_lock_path};
use serde_json::Value;
use tempfile::tempdir;

#[test]
fn admin_cli_inspects_and_clears_only_matching_explicitly_confirmed_stale_lock() {
    let root = tempdir().expect("temporary root");
    let directory = root.path().join("generations");
    fs::create_dir(&directory).expect("create generation directory");

    let absent = run_inspect(&directory);
    assert_success("inspect absent lock", &absent);
    let absent: Value = serde_json::from_slice(&absent.stdout).expect("decode absent inspection");
    assert_eq!(absent["present"], false);
    assert_eq!(absent["record_hex"], Value::Null);

    let lease = acquire_generation_writer_lease(&directory).expect("acquire diagnostic lease");
    let stale_record = lease.owner_record().to_vec();
    let lock_path = lease.lock_path().to_path_buf();
    drop(lease);
    assert!(!lock_path.exists());
    fs::write(&lock_path, &stale_record)
        .expect("restore stale lock evidence after simulated crash");

    let inspected = run_inspect(&directory);
    assert_success("inspect stale lock", &inspected);
    let inspection: Value =
        serde_json::from_slice(&inspected.stdout).expect("decode stale inspection");
    assert_eq!(inspection["present"], true);
    assert_eq!(
        inspection["recorded_lock_protocol"],
        "append_log_generation_writer_lock_v1"
    );
    assert!(inspection["recorded_pid"].as_u64().is_some());
    assert!(inspection["acquisition_id"].as_str().is_some());
    let expected_hex = inspection["record_hex"]
        .as_str()
        .expect("observed exact record hex")
        .to_owned();

    let unconfirmed = run_clear(&directory, &expected_hex, false);
    assert_failure_contains(&unconfirmed, "requires explicit confirmation");
    assert_eq!(
        fs::read(&lock_path).expect("stale lock retained"),
        stale_record
    );

    let mismatched = run_clear(&directory, "00", true);
    assert_failure_contains(&mismatched, "bytes changed since inspection");
    assert_eq!(
        fs::read(&lock_path).expect("mismatched lock retained"),
        stale_record
    );

    let cleared = run_clear(&directory, &expected_hex, true);
    assert_success("clear stale lock", &cleared);
    let clear_summary: Value =
        serde_json::from_slice(&cleared.stdout).expect("decode clear summary");
    assert_eq!(
        clear_summary["protocol"],
        "append_log_generation_writer_lock_clear_v1"
    );
    assert_eq!(
        clear_summary["removed_record_hex"], expected_hex,
        "clear summary must identify the exact evidence removed"
    );
    assert!(!lock_path.exists());

    let after = run_inspect(&directory);
    assert_success("inspect cleared lock", &after);
    let after: Value = serde_json::from_slice(&after.stdout).expect("decode cleared inspection");
    assert_eq!(after["present"], false);

    let lease = acquire_generation_writer_lease(&directory).expect("acquire after stale clear");
    drop(lease);
    assert_eq!(
        generation_writer_lock_path(&directory).expect("derive lock path"),
        lock_path
    );
}

fn run_inspect(directory: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-lock"))
        .arg("inspect")
        .arg("--directory")
        .arg(directory)
        .output()
        .expect("run lock inspect")
}

fn run_clear(directory: &Path, expected_hex: &str, confirm: bool) -> Output {
    let mut command = Command::new(env!("CARGO_BIN_EXE_db-lab-log-generation-lock"));
    command
        .arg("clear-stale")
        .arg("--directory")
        .arg(directory)
        .arg("--expected-record-hex")
        .arg(expected_hex);
    if confirm {
        command.arg("--confirm-no-live-writer");
    }
    command.output().expect("run lock clear")
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
