use std::fs;
use std::process::{Command, Output};

use db_core::{generate_experiment_trace, ExperimentGeneratorConfig, ExperimentProfile};
use serde_json::Value;
use tempfile::tempdir;

#[test]
fn real_batch_archive_verifies_analyzes_bundles_and_tampering_fails_closed() {
    let directory = tempdir().expect("temporary directory");
    let trace_path = directory.path().join("trace.json");
    let engine_root = directory.path().join("engines");
    let archive_dir = directory.path().join("archive");
    let bundle_dir = directory.path().join("analysis-bundle");
    let trace = generate_experiment_trace(ExperimentGeneratorConfig {
        seed: 0x2026_0830,
        profile: ExperimentProfile::RandomWrite,
        operations: 1,
        key_space: 8,
        value_bytes: 8,
        range_limit: 1,
        reopen_every: Some(1),
    })
    .expect("generate trace");
    fs::write(
        &trace_path,
        serde_json::to_vec_pretty(&trace).expect("encode trace"),
    )
    .expect("write trace");

    let producer = Command::new(env!("CARGO_BIN_EXE_db-lab-batch"))
        .arg("--trace")
        .arg(&trace_path)
        .arg("--engine-root")
        .arg(&engine_root)
        .arg("--archive-dir")
        .arg(&archive_dir)
        .arg("--pair-seed")
        .arg("0")
        .arg("--pairs")
        .arg("1")
        .arg("--btree-cache-pages")
        .arg("8")
        .arg("--revision")
        .arg("abc123")
        .output()
        .expect("run db-lab-batch");
    assert_success("db-lab-batch", &producer);

    let verified = run_verifier(&archive_dir);
    assert_success("db-lab-batch-verify", &verified);
    let summary: Value = serde_json::from_slice(&verified.stdout).expect("decode verifier summary");
    assert_eq!(summary["valid"], true);
    assert_eq!(summary["format_version"], 6);
    assert_eq!(summary["repository_revision"], "abc123");
    assert_eq!(summary["requested_pairs"], 1);
    assert_eq!(summary["included_pairs"], 1);
    assert_eq!(summary["failed_pairs"], 0);
    assert_eq!(summary["excluded_pairs"], 0);
    assert_eq!(summary["comparison_failure_sidecars"], 0);

    let analyzed = run_analyzer(&archive_dir);
    assert_success("db-lab-batch-analyze", &analyzed);
    let analysis: Value = serde_json::from_slice(&analyzed.stdout).expect("decode analyzer report");
    assert_eq!(
        analysis["analysis_protocol"],
        "verified_operational_timing_descriptive_v1"
    );
    assert_eq!(analysis["snapshot_protocol"], "copy_verify_compare_v1");
    assert_eq!(analysis["verification"]["format_version"], 6);
    assert_eq!(analysis["verification"]["included_pairs"], 1);
    assert_eq!(
        analysis["primary_complete_pairs"]["combined"]["left"]["reopen"]["duration_ns"]["samples"],
        2
    );
    assert_eq!(
        analysis["primary_complete_pairs"]["combined"]["right"]["reopen"]["duration_ns"]["samples"],
        2
    );
    assert_eq!(
        analysis["primary_complete_pairs"]["by_execution_order"]["left_then_right"]["left"]
            ["reopen"]["duration_ns"]["samples"],
        1
    );
    assert_eq!(
        analysis["primary_complete_pairs"]["by_execution_order"]["right_then_left"]["left"]
            ["reopen"]["duration_ns"]["samples"],
        1
    );
    assert_eq!(
        analysis["retained_failed_pair_evidence"]["failed_operations"]["combined"]["left"]
            ["reopen"]["duration_ns"]["samples"],
        0
    );

    let bundled = run_bundle_create(&archive_dir, &bundle_dir);
    assert_success("db-lab-batch-analysis-bundle create", &bundled);
    let bundle_summary: Value =
        serde_json::from_slice(&bundled.stdout).expect("decode bundle summary");
    assert_eq!(bundle_summary["valid"], true);
    assert_eq!(bundle_summary["bundle_format_version"], 1);
    assert_eq!(
        bundle_summary["bundle_protocol"],
        "verified_operational_timing_analysis_bundle_v1"
    );
    assert_eq!(bundle_summary["repository_revision"], "abc123");
    assert_eq!(bundle_summary["source_archive_format_version"], 6);
    assert_eq!(bundle_summary["publication_admitted"], false);
    assert_eq!(bundle_summary["evidence_files"], 4);

    let bundled_verify = run_bundle_verify(&bundle_dir);
    assert_success("db-lab-batch-analysis-bundle verify", &bundled_verify);
    let bundled_analysis: Value = serde_json::from_slice(
        &fs::read(bundle_dir.join("analysis.json")).expect("read bundled analysis"),
    )
    .expect("decode bundled analysis");
    assert_eq!(bundled_analysis, analysis);
    let bundled_evidence_verified = run_verifier(&bundle_dir.join("evidence"));
    assert_success(
        "db-lab-batch-verify bundled evidence",
        &bundled_evidence_verified,
    );

    let duplicate_bundle = run_bundle_create(&archive_dir, &bundle_dir);
    assert_failure_contains(
        "db-lab-batch-analysis-bundle duplicate create",
        &duplicate_bundle,
        "destination already exists",
    );
    assert_success(
        "db-lab-batch-analysis-bundle verify after duplicate create",
        &run_bundle_verify(&bundle_dir),
    );

    let analysis_path = bundle_dir.join("analysis.json");
    let mut tampered_analysis = bundled_analysis.clone();
    tampered_analysis["estimator"] = Value::String("tampered-estimator".to_owned());
    fs::write(
        &analysis_path,
        serde_json::to_vec_pretty(&tampered_analysis).expect("encode tampered analysis"),
    )
    .expect("write tampered analysis");
    assert_failure_contains(
        "db-lab-batch-analysis-bundle analysis tamper",
        &run_bundle_verify(&bundle_dir),
        "analysis.json does not match analysis recomputed from bundled evidence",
    );
    fs::write(
        &analysis_path,
        serde_json::to_vec_pretty(&bundled_analysis).expect("encode restored analysis"),
    )
    .expect("restore bundled analysis");
    assert_success(
        "db-lab-batch-analysis-bundle verify restored analysis",
        &run_bundle_verify(&bundle_dir),
    );

    let environment_path = archive_dir.join("environment.json");
    let mut environment: Value =
        serde_json::from_slice(&fs::read(&environment_path).expect("read environment"))
            .expect("decode environment");
    environment["repository_revision"] = Value::String("tampered-revision".to_owned());
    fs::write(
        &environment_path,
        serde_json::to_vec_pretty(&environment).expect("encode tampered environment"),
    )
    .expect("write tampered environment");

    let rejected = run_verifier(&archive_dir);
    assert_failure_contains(
        "db-lab-batch-verify",
        &rejected,
        "repository_revision differs between environment.json and index.json",
    );
    let rejected_analysis = run_analyzer(&archive_dir);
    assert_failure_contains(
        "db-lab-batch-analyze",
        &rejected_analysis,
        "repository_revision differs between environment.json and index.json",
    );

    environment["repository_revision"] = Value::String("abc123".to_owned());
    fs::write(
        &environment_path,
        serde_json::to_vec_pretty(&environment).expect("encode restored environment"),
    )
    .expect("restore environment");

    let batch_path = archive_dir.join("batch.json");
    let mut batch: Value =
        serde_json::from_slice(&fs::read(&batch_path).expect("read batch")).expect("decode batch");
    let reopen_ns = &mut batch["attempts"][0]["report"]["first"]["comparison"]["left"]
        ["operational_timing"]["reopen_ns"];
    let durations = reopen_ns
        .as_array_mut()
        .expect("left first repetition reopen_ns must be an array");
    assert!(!durations.is_empty(), "real trace must emit reopen timing");
    let original_duration = durations[0]
        .as_u64()
        .expect("real reopen duration must be unsigned");
    durations[0] = Value::from(original_duration.saturating_add(1));
    fs::write(
        &batch_path,
        serde_json::to_vec_pretty(&batch).expect("encode timing-tampered batch"),
    )
    .expect("write timing-tampered batch");

    let structurally_verified = run_verifier(&archive_dir);
    assert_success(
        "db-lab-batch-verify after timing projection tamper",
        &structurally_verified,
    );
    let rejected_timing_analysis = run_analyzer(&archive_dir);
    assert_failure_contains(
        "db-lab-batch-analyze",
        &rejected_timing_analysis,
        "differs from reopen_samples",
    );

    assert_success(
        "sealed bundle remains valid after source mutations",
        &run_bundle_verify(&bundle_dir),
    );
}

fn run_verifier(archive_dir: &std::path::Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-batch-verify"))
        .arg("--archive-dir")
        .arg(archive_dir)
        .arg("--expected-revision")
        .arg("abc123")
        .output()
        .expect("run db-lab-batch-verify")
}

fn run_analyzer(archive_dir: &std::path::Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-batch-analyze"))
        .arg("--archive-dir")
        .arg(archive_dir)
        .arg("--expected-revision")
        .arg("abc123")
        .output()
        .expect("run db-lab-batch-analyze")
}

fn run_bundle_create(archive_dir: &std::path::Path, bundle_dir: &std::path::Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-batch-analysis-bundle"))
        .arg("create")
        .arg("--archive-dir")
        .arg(archive_dir)
        .arg("--bundle-dir")
        .arg(bundle_dir)
        .arg("--expected-revision")
        .arg("abc123")
        .output()
        .expect("run db-lab-batch-analysis-bundle create")
}

fn run_bundle_verify(bundle_dir: &std::path::Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_db-lab-batch-analysis-bundle"))
        .arg("verify")
        .arg("--bundle-dir")
        .arg(bundle_dir)
        .arg("--expected-revision")
        .arg("abc123")
        .output()
        .expect("run db-lab-batch-analysis-bundle verify")
}

fn assert_success(name: &str, output: &Output) {
    assert!(
        output.status.success(),
        "{name} failed: stdout={} stderr={}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn assert_failure_contains(name: &str, output: &Output, expected: &str) {
    assert!(
        !output.status.success(),
        "{name} unexpectedly succeeded: stdout={} stderr={}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains(expected),
        "unexpected {name} error; expected {expected:?}, stderr={stderr}"
    );
}
