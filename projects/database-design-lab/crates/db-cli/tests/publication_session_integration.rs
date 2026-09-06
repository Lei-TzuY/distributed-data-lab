use std::fs;
use std::path::Path;
use std::process::{Command, Output};

use db_core::{generate_experiment_trace, ExperimentGeneratorConfig, ExperimentProfile};
use serde_json::{json, Value};
use tempfile::tempdir;

const PRE_TIME: u64 = 1_788_000_000;
const ARCHIVE_TIME: u64 = 1_788_000_100;
const POST_TIME: u64 = 1_788_000_200;
const LIMITATIONS: [&str; 3] = [
    "operator attestations are recorded statements, not independently verified facts",
    "thermal equilibrium and storage/controller cache state are not portable kernel observations in this protocol",
    "a passing snapshot is a prerequisite for controlled collection, not a performance result or regression threshold",
];

#[test]
fn publication_session_v2_encloses_verified_archive_and_rejects_temporal_tampering() {
    let directory = tempdir().expect("temporary directory");
    let preflight_path = directory.path().join("preflight.json");
    let postflight_path = directory.path().join("postflight.json");
    let archive_dir = directory.path().join("archive");
    let session_dir = directory.path().join("session");
    fs::create_dir(&archive_dir).expect("create archive directory");
    write_json_file(&preflight_path, &passing_snapshot("perf-host-01", PRE_TIME));
    write_json_file(
        &postflight_path,
        &passing_snapshot("perf-host-01", POST_TIME),
    );
    write_publication_archive(&archive_dir, "perf-host-01", "abc123", ARCHIVE_TIME);

    let created = run_create(
        &preflight_path,
        &postflight_path,
        &archive_dir,
        &session_dir,
    );
    assert_success("publication session create", &created);
    let summary: Value = serde_json::from_slice(&created.stdout).expect("decode create summary");
    assert_eq!(summary["valid"], true);
    assert_eq!(summary["session_format_version"], 2);
    assert_eq!(
        summary["session_protocol"],
        "controlled_publication_session_v2"
    );
    assert_eq!(summary["host_label"], "perf-host-01");
    assert_eq!(summary["preflight_recorded_unix_seconds"], PRE_TIME);
    assert_eq!(summary["archive_recorded_unix_seconds"], ARCHIVE_TIME);
    assert_eq!(summary["postflight_recorded_unix_seconds"], POST_TIME);
    assert_eq!(summary["repository_revision"], "abc123");
    assert_eq!(summary["source_archive_format_version"], 7);
    assert_eq!(summary["evidence_files"], 4);
    assert_success("publication session verify", &run_verify(&session_dir));

    let bundled_preflight = session_dir.join("host-preflight.json");
    let original_preflight = fs::read(&bundled_preflight).expect("read bundled preflight");
    let mut preflight_value: Value =
        serde_json::from_slice(&original_preflight).expect("decode bundled preflight");
    preflight_value["host_label"] = json!("different-host");
    write_json_file(&bundled_preflight, &preflight_value);
    assert_failure_contains(&run_verify(&session_dir), "differs from expected label");
    fs::write(&bundled_preflight, &original_preflight).expect("restore bundled preflight");

    let bundled_postflight = session_dir.join("host-postflight.json");
    let original_postflight = fs::read(&bundled_postflight).expect("read bundled postflight");
    let mut postflight_value: Value =
        serde_json::from_slice(&original_postflight).expect("decode bundled postflight");
    postflight_value["recorded_unix_seconds"] = json!(ARCHIVE_TIME - 1);
    write_json_file(&bundled_postflight, &postflight_value);
    assert_failure_contains(
        &run_verify(&session_dir),
        "recording time differs from v2 session index",
    );
    fs::write(&bundled_postflight, &original_postflight).expect("restore bundled postflight");

    let environment_path = session_dir.join("evidence/environment.json");
    let original_environment = fs::read(&environment_path).expect("read bundled environment");
    let mut environment: Value =
        serde_json::from_slice(&original_environment).expect("decode bundled environment");
    environment["publication_admission"]["host_label"] = json!("different-host");
    write_json_file(&environment_path, &environment);
    assert_failure_contains(&run_verify(&session_dir), "differs from session host label");
    fs::write(&environment_path, &original_environment).expect("restore bundled environment");
    assert_success("restored publication session", &run_verify(&session_dir));

    let late_preflight = directory.path().join("late-preflight.json");
    write_json_file(
        &late_preflight,
        &passing_snapshot("perf-host-01", ARCHIVE_TIME + 1),
    );
    let late_preflight_session = directory.path().join("late-preflight-session");
    let late = run_create(
        &late_preflight,
        &postflight_path,
        &archive_dir,
        &late_preflight_session,
    );
    assert_failure_contains(&late, "preflight recording time");
    assert!(!late_preflight_session.exists());

    let early_postflight = directory.path().join("early-postflight.json");
    write_json_file(
        &early_postflight,
        &passing_snapshot("perf-host-01", ARCHIVE_TIME - 1),
    );
    let early_postflight_session = directory.path().join("early-postflight-session");
    let early = run_create(
        &preflight_path,
        &early_postflight,
        &archive_dir,
        &early_postflight_session,
    );
    assert_failure_contains(&early, "after postflight recording time");
    assert!(!early_postflight_session.exists());

    let mismatched_postflight = directory.path().join("mismatched-postflight.json");
    write_json_file(
        &mismatched_postflight,
        &passing_snapshot("other-host", POST_TIME),
    );
    let rejected_session = directory.path().join("rejected-session");
    let mismatch = run_create(
        &preflight_path,
        &mismatched_postflight,
        &archive_dir,
        &rejected_session,
    );
    assert_failure_contains(&mismatch, "postflight host label");
    assert!(!rejected_session.exists());
}

#[test]
fn verifier_keeps_retained_v1_sessions_readable() {
    let directory = tempdir().expect("temporary directory");
    let archive_dir = directory.path().join("archive");
    let session_dir = directory.path().join("legacy-session");
    fs::create_dir(&archive_dir).expect("create archive directory");
    write_publication_archive(&archive_dir, "perf-host-01", "abc123", ARCHIVE_TIME);
    write_legacy_v1_session(&session_dir, &archive_dir);

    let verified = run_verify(&session_dir);
    assert_success("legacy v1 publication session verify", &verified);
    let summary: Value = serde_json::from_slice(&verified.stdout).expect("decode verify summary");
    assert_eq!(summary["session_format_version"], 1);
    assert_eq!(
        summary["session_protocol"],
        "controlled_publication_session_v1"
    );
    assert!(summary.get("archive_recorded_unix_seconds").is_none());
    assert!(summary.get("postflight_recorded_unix_seconds").is_none());
}

fn run_create(
    preflight: &Path,
    postflight: &Path,
    archive_dir: &Path,
    session_dir: &Path,
) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-publication-session"))
        .arg("create")
        .arg("--host-preflight")
        .arg(preflight)
        .arg("--host-postflight")
        .arg(postflight)
        .arg("--archive-dir")
        .arg(archive_dir)
        .arg("--session-dir")
        .arg(session_dir)
        .arg("--expected-revision")
        .arg("abc123")
        .output()
        .expect("run publication session create")
}

fn run_verify(session_dir: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-publication-session"))
        .arg("verify")
        .arg("--session-dir")
        .arg(session_dir)
        .arg("--expected-revision")
        .arg("abc123")
        .output()
        .expect("run publication session verify")
}

fn passing_snapshot(host_label: &str, recorded_unix_seconds: u64) -> Value {
    json!({
        "protocol": "linux_controlled_host_preflight_v1",
        "recorded_unix_seconds": recorded_unix_seconds,
        "host_label": host_label,
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
            "governors": {"2": "performance", "3": "performance"},
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

fn write_publication_archive(path: &Path, host_label: &str, revision: &str, recorded: u64) {
    let trace = generate_experiment_trace(ExperimentGeneratorConfig {
        seed: 7,
        profile: ExperimentProfile::RandomWrite,
        operations: 1,
        key_space: 4,
        value_bytes: 4,
        range_limit: 1,
        reopen_every: None,
    })
    .expect("generate trace");
    let trace = serde_json::to_value(trace).expect("encode trace value");
    let batch = json!({
        "trace": trace,
        "pair_seed": 0,
        "requested_pairs": 1,
        "included_pairs": 0,
        "failed_pairs": 0,
        "excluded_pairs": 1,
        "attempts": [{
            "context": {"pair_index": 0, "pair_order": "left_then_right_first"},
            "disposition": "excluded",
            "report": null,
            "failure": null,
            "exclusion_reason": "synthetic publication-session fixture"
        }]
    });
    let admission = json!({
        "admission_protocol": "publication_warm_v1",
        "cache_policy": "trace_induced_warm",
        "cache_state": "warm",
        "durability_mode": "synced_single_operation",
        "pair_order_policy": "pair_seed_low_bit_then_alternate",
        "requested_pairs": 1,
        "ordered_comparisons_per_included_pair": 2,
        "rust_target_triple": "x86_64-unknown-linux-gnu",
        "host_label": host_label,
        "host_cpu": "Example CPU / CPUs 2-3",
        "host_memory": "64 GiB",
        "storage_device": "Example NVMe",
        "filesystem": "ext4",
        "mount_options": "rw,noatime",
        "optimization_flags": "--release; target-cpu=native",
        "analysis_script_version": "analysis-v1",
        "noise_budget": "controlled-host-preflight-v1"
    });
    let environment = json!({
        "format_version": 7,
        "repository_revision": revision,
        "execution_protocol": "fresh_counterbalanced_repeated_batch_v1",
        "attempt_protocol": "retain_all_requested_pairs_v1",
        "pair_seed": 0,
        "requested_pairs": 1,
        "engine_layout": "pair-{pair_index:06}/repetition-{repetition_index}/{btree.db|lsm}",
        "cache_state": "warm",
        "recorded_unix_seconds": recorded,
        "publication_admission": admission
    });
    let index = json!({
        "format_version": 7,
        "repository_revision": revision,
        "execution_protocol": "fresh_counterbalanced_repeated_batch_v1",
        "attempt_protocol": "retain_all_requested_pairs_v1",
        "admission_protocol": "publication_warm_v1",
        "files": ["trace.json", "batch.json", "environment.json"]
    });

    write_json_file(
        &path.join("trace.json"),
        batch.get("trace").expect("batch trace"),
    );
    write_json_file(&path.join("batch.json"), &batch);
    write_json_file(&path.join("environment.json"), &environment);
    write_json_file(&path.join("index.json"), &index);
}

fn write_legacy_v1_session(session: &Path, archive: &Path) {
    fs::create_dir(session).expect("create legacy session");
    let evidence = session.join("evidence");
    fs::create_dir(&evidence).expect("create legacy evidence directory");
    for name in ["trace.json", "batch.json", "environment.json", "index.json"] {
        fs::copy(archive.join(name), evidence.join(name)).expect("copy legacy evidence file");
    }
    write_json_file(
        &session.join("host-preflight.json"),
        &passing_snapshot("perf-host-01", PRE_TIME),
    );
    write_json_file(
        &session.join("index.json"),
        &json!({
            "format_version": 1,
            "session_protocol": "controlled_publication_session_v1",
            "host_preflight_protocol": "linux_controlled_host_preflight_v1",
            "publication_admission_protocol": "publication_warm_v1",
            "host_label": "perf-host-01",
            "preflight_recorded_unix_seconds": PRE_TIME,
            "repository_revision": "abc123",
            "source_archive_format_version": 7,
            "evidence_files": ["batch.json", "environment.json", "index.json", "trace.json"]
        }),
    );
}

fn write_json_file(path: &Path, value: &Value) {
    fs::write(path, serde_json::to_vec_pretty(value).expect("encode json")).expect("write json");
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
