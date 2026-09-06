use std::fs;
use std::path::Path;
use std::process::{Command, Output};

use db_core::{ByteString, KvEngine, Workload, WorkloadStep, WORKLOAD_FORMAT_VERSION};
use db_storage_log::LogEngine;
use tempfile::tempdir;

#[test]
fn mutating_raw_cli_rejects_canonical_generation_path_but_read_only_tools_still_work() {
    let root = tempdir().expect("temporary root");
    let generation_path = root.path().join("generation-00000000000000000001.log");
    let workload_path = root.path().join("workload.json");

    {
        let mut engine = LogEngine::create_new_managed_generation(&generation_path)
            .expect("create canonical-named managed log fixture");
        engine.put(b"existing", b"value").expect("seed log");
    }
    let before = fs::read(&generation_path).expect("read log before guarded run");
    write_put_workload(&workload_path);

    let run = run_db_lab([
        "run",
        "--engine",
        "log",
        "--path",
        generation_path.to_str().expect("UTF-8 generation path"),
        workload_path.to_str().expect("UTF-8 workload path"),
    ]);
    assert_failure_contains(
        &run,
        "raw append-log mutation refuses canonical generation path",
    );
    assert_failure_contains(&run, "db-lab-log-generation-run");
    assert_eq!(
        fs::read(&generation_path).expect("read log after guarded run"),
        before,
        "rejected raw run must not mutate the canonical-named log"
    );

    let verify = run_db_lab([
        "verify",
        generation_path.to_str().expect("UTF-8 generation path"),
    ]);
    assert_success("verify canonical generation evidence", &verify);

    let inspect = run_db_lab([
        "inspect",
        generation_path.to_str().expect("UTF-8 generation path"),
        "--show-values",
    ]);
    assert_success("inspect canonical generation evidence", &inspect);
}

#[test]
fn differential_raw_log_rejects_reserved_generation_name_before_creation() {
    let root = tempdir().expect("temporary root");
    let generation_path = root.path().join("generation-00000000000000000042.log");
    let workload_path = root.path().join("workload.json");
    write_put_workload(&workload_path);

    let differential = run_db_lab([
        "differential",
        "--engine",
        "log",
        "--path",
        generation_path.to_str().expect("UTF-8 generation path"),
        workload_path.to_str().expect("UTF-8 workload path"),
    ]);
    assert_failure_contains(
        &differential,
        "raw append-log mutation refuses canonical generation path",
    );
    assert!(
        !generation_path.exists(),
        "guard must fail before creating a canonical generation pathname"
    );
}

#[test]
fn noncanonical_raw_log_path_remains_supported() {
    let root = tempdir().expect("temporary root");
    let raw_path = root.path().join("ordinary-raw.log");
    let workload_path = root.path().join("workload.json");
    write_put_workload(&workload_path);

    let differential = run_db_lab([
        "differential",
        "--engine",
        "log",
        "--path",
        raw_path.to_str().expect("UTF-8 raw path"),
        workload_path.to_str().expect("UTF-8 workload path"),
    ]);
    assert_success("ordinary raw append-log differential", &differential);
    assert!(raw_path.exists());
}

fn write_put_workload(path: &Path) {
    let workload = Workload {
        format_version: WORKLOAD_FORMAT_VERSION,
        seed: Some(7),
        steps: vec![WorkloadStep::Put {
            key: ByteString::from(b"key".to_vec()),
            value: ByteString::from(b"value".to_vec()),
        }],
    };
    fs::write(
        path,
        serde_json::to_vec_pretty(&workload).expect("encode workload"),
    )
    .expect("write workload");
}

fn run_db_lab<const N: usize>(args: [&str; N]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab"))
        .args(args)
        .output()
        .expect("run db-lab")
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
