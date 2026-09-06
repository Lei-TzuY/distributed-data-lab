use std::fs;
use std::process::{Command, Output};

use serde_json::{json, Value};
use tempfile::tempdir;

const LIMITATIONS: [&str; 3] = [
    "operator attestations are recorded statements, not independently verified facts",
    "thermal equilibrium and storage/controller cache state are not portable kernel observations in this protocol",
    "a passing snapshot is a prerequisite for controlled collection, not a performance result or regression threshold",
];

#[test]
fn verifier_binary_accepts_valid_snapshot_and_rejects_tampering() {
    let directory = tempdir().expect("temporary directory");
    let snapshot_path = directory.path().join("preflight.json");
    let snapshot = passing_snapshot();
    write_json(&snapshot_path, &snapshot);

    let verified = run_verifier(&snapshot_path, true, Some("perf-host-01"));
    assert_success("db-lab-host-preflight-verify", &verified);
    let summary: Value = serde_json::from_slice(&verified.stdout).expect("decode summary");
    assert_eq!(summary["valid"], true);
    assert_eq!(summary["protocol"], "linux_controlled_host_preflight_v1");
    assert_eq!(summary["passed"], true);
    assert_eq!(summary["host_label"], "perf-host-01");
    assert_eq!(summary["process_cpu_affinity"], json!([2, 3]));
    assert_eq!(summary["violations"], 0);

    let wrong_host = run_verifier(&snapshot_path, true, Some("different-host"));
    assert_failure_contains(&wrong_host, "differs from expected label");

    let mut tampered = snapshot.clone();
    tampered["observation"]["process_allowed_cpus"] = json!([2, 3, 4]);
    tampered["passed"] = Value::Bool(false);
    tampered["violations"] = json!([]);
    write_json(&snapshot_path, &tampered);
    let ledger_tamper = run_verifier(&snapshot_path, false, None);
    assert_failure_contains(&ledger_tamper, "violations do not match");

    let mut failed = snapshot;
    failed["observation"]["process_allowed_cpus"] = json!([2, 3, 4]);
    failed["passed"] = Value::Bool(false);
    failed["violations"] = json!(["process CPU affinity is [2, 3, 4]; expected exactly [2, 3]"]);
    write_json(&snapshot_path, &failed);

    let auditable = run_verifier(&snapshot_path, false, None);
    assert_success("failed snapshot audit", &auditable);
    let summary: Value = serde_json::from_slice(&auditable.stdout).expect("decode failed summary");
    assert_eq!(summary["passed"], false);
    assert_eq!(summary["violations"], 1);

    let admission = run_verifier(&snapshot_path, true, None);
    assert_failure_contains(&admission, "records passed=false");
}

fn passing_snapshot() -> Value {
    json!({
        "protocol": "linux_controlled_host_preflight_v1",
        "recorded_unix_seconds": 1_788_000_000_u64,
        "host_label": "perf-host-01",
        "passed": true,
        "expected": {
            "process_cpu_affinity": [2, 3],
            "scaling_governor": "performance",
            "turbo_disabled": true,
            "max_load_per_cpu": 0.10
        },
        "observation": {
            "target_os": "linux",
            "target_arch": "x86_64",
            "kernel_release": "example-kernel",
            "cpu_model": "Example CPU",
            "process_allowed_cpus": [2, 3],
            "online_cpus": [0, 1, 2, 3],
            "governors": {
                "2": "performance",
                "3": "performance"
            },
            "turbo": {
                "interface": "/sys/devices/system/cpu/intel_pstate/no_turbo",
                "raw_value": "1",
                "disabled": true
            },
            "load_one": 0.10
        },
        "operator_attestations": {
            "thermal_control": "steady-state thermal protocol",
            "background_load_control": "benchmark services only",
            "storage_cache_control": "trace-induced warm policy"
        },
        "violations": [],
        "limitations": LIMITATIONS
    })
}

fn write_json(path: &std::path::Path, value: &Value) {
    fs::write(
        path,
        serde_json::to_vec_pretty(value).expect("encode snapshot"),
    )
    .expect("write snapshot");
}

fn run_verifier(
    snapshot_path: &std::path::Path,
    require_passed: bool,
    expected_host_label: Option<&str>,
) -> Output {
    let mut command = Command::new(env!("CARGO_BIN_EXE_db-lab-host-preflight-verify"));
    command.arg("--snapshot").arg(snapshot_path);
    if require_passed {
        command.arg("--require-passed");
    }
    if let Some(label) = expected_host_label {
        command.arg("--expected-host-label").arg(label);
    }
    command.output().expect("run host preflight verifier")
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
