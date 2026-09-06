use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use clap::{Args, ValueEnum};
use db_core::{
    compare_experiment_trace_counterbalanced, CounterbalancedExperimentComparisonReport,
    CounterbalancedPairOrder, DbError, ErrorClass, ExperimentTrace,
};
use db_storage_btree::{BPlusTree, BtreeError};
use db_storage_lsm::LsmEngine;
use serde::Serialize;

use super::{
    read_experiment_trace, rustc_version, validate_revision, write_new_json, CacheStateKind,
    CliError,
};

const COUNTERBALANCED_EVIDENCE_ARCHIVE_FORMAT_VERSION: u16 = 2;
const COUNTERBALANCED_ATTEMPT_ARCHIVE_FORMAT_VERSION: u16 = 3;
const PUBLICATION_EVIDENCE_ARCHIVE_FORMAT_VERSION: u16 = 4;
const PUBLICATION_ATTEMPT_ARCHIVE_FORMAT_VERSION: u16 = 5;
const COUNTERBALANCED_EXECUTION_PROTOCOL: &str = "fresh_counterbalanced_ab_ba";
const COUNTERBALANCED_ATTEMPT_PROTOCOL: &str = "record_non_success_counterbalanced_attempts";
const PUBLICATION_ADMISSION_PROTOCOL: &str = "publication_warm_v1";
const PUBLICATION_CACHE_POLICY: &str = "trace_induced_warm";
const PUBLICATION_DURABILITY_MODE: &str = "synced_single_operation";
const MAX_EXCLUSION_REASON_BYTES: usize = 4 * 1024;
const MAX_PUBLICATION_METADATA_BYTES: usize = 4 * 1024;

#[derive(Debug, Clone, Copy, ValueEnum)]
enum CounterbalancedPairOrderKind {
    LeftThenRightFirst,
    RightThenLeftFirst,
}

impl From<CounterbalancedPairOrderKind> for CounterbalancedPairOrder {
    fn from(value: CounterbalancedPairOrderKind) -> Self {
        match value {
            CounterbalancedPairOrderKind::LeftThenRightFirst => Self::LeftThenRightFirst,
            CounterbalancedPairOrderKind::RightThenLeftFirst => Self::RightThenLeftFirst,
        }
    }
}

#[derive(Debug, Clone, Copy, ValueEnum)]
enum AdmissionKind {
    /// Exploratory evidence may declare incomplete environment metadata and is not publication-grade.
    Exploratory,
    /// Strict release-only warm-cache admission with complete reproducibility metadata.
    PublicationWarmV1,
}

/// Arguments for a fresh two-repetition AB/BA evidence archive.
#[derive(Debug, Args)]
pub(super) struct CounterbalancedArchiveArgs {
    /// Versioned experiment trace JSON file.
    #[arg(long)]
    trace: PathBuf,
    /// New B+ tree page file for the first repetition.
    #[arg(long)]
    first_btree_path: PathBuf,
    /// New LSM directory for the first repetition.
    #[arg(long)]
    first_lsm_path: PathBuf,
    /// New B+ tree page file for the second repetition.
    #[arg(long)]
    second_btree_path: PathBuf,
    /// New LSM directory for the second repetition.
    #[arg(long)]
    second_lsm_path: PathBuf,
    /// Which whole-run order executes first in the two-run pair.
    #[arg(
        long,
        value_enum,
        default_value_t = CounterbalancedPairOrderKind::LeftThenRightFirst
    )]
    pair_order: CounterbalancedPairOrderKind,
    /// B+ tree validated-page cache capacity used by both repetitions.
    #[arg(long, default_value_t = 64)]
    btree_cache_pages: usize,
    /// Exact source revision represented by the binary/run, normally a full Git commit SHA.
    #[arg(long)]
    revision: String,
    /// New archive directory; existing paths are never overwritten.
    #[arg(long)]
    archive_dir: PathBuf,
    /// Human-readable host identity without secrets (for example, `lab-5090-a`).
    #[arg(long)]
    host_label: Option<String>,
    /// Host CPU model/topology label required by publication admission.
    #[arg(long)]
    host_cpu: Option<String>,
    /// Host memory configuration label required by publication admission.
    #[arg(long)]
    host_memory: Option<String>,
    /// Filesystem under test, when known (for example, `ntfs`, `ext4`, `apfs`).
    #[arg(long)]
    filesystem: Option<String>,
    /// Filesystem mount options required by publication admission.
    #[arg(long)]
    mount_options: Option<String>,
    /// Storage device/model label, when known. Do not place credentials or serial numbers here.
    #[arg(long)]
    storage_device: Option<String>,
    /// Declared cache preparation state for both repetitions.
    #[arg(long, value_enum, default_value_t = CacheStateKind::Unspecified)]
    cache_state: CacheStateKind,
    /// Evidence admission policy. Publication mode is intentionally stricter than exploratory mode.
    #[arg(long, value_enum, default_value_t = AdmissionKind::Exploratory)]
    admission: AdmissionKind,
    /// Optimization/Rust flags used for the measured binary; required by publication admission.
    #[arg(long)]
    optimization_flags: Option<String>,
    /// Version/commit of the analysis script intended to consume this archive.
    #[arg(long)]
    analysis_script_version: Option<String>,
    /// Reviewed host-noise budget/threshold identifier for this run.
    #[arg(long)]
    noise_budget: Option<String>,
    /// Record a methodological exclusion without creating either repetition.
    #[arg(long)]
    exclude_reason: Option<String>,
    /// Optional free-form experiment note. Do not include secrets.
    #[arg(long)]
    notes: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
struct PublicationAdmissionRecord {
    admission_protocol: &'static str,
    rust_target_triple: String,
    host_label: String,
    host_cpu: String,
    host_memory: String,
    storage_device: String,
    filesystem: String,
    mount_options: String,
    cache_policy: &'static str,
    cache_state: &'static str,
    durability_mode: &'static str,
    repetition_count: u8,
    optimization_flags: String,
    analysis_script_version: String,
    noise_budget: String,
}

#[derive(Debug, Serialize)]
struct CounterbalancedEvidenceArchiveEnvironment {
    format_version: u16,
    repository_revision: String,
    execution_protocol: &'static str,
    pair_order: CounterbalancedPairOrder,
    db_lab_version: &'static str,
    target_os: &'static str,
    target_arch: &'static str,
    build_profile: &'static str,
    rustc_version: Option<String>,
    host_label: Option<String>,
    filesystem: Option<String>,
    storage_device: Option<String>,
    cache_state: &'static str,
    btree_cache_pages: usize,
    recorded_unix_seconds: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    publication_admission: Option<PublicationAdmissionRecord>,
    notes: Option<String>,
}

#[derive(Debug, Serialize)]
struct CounterbalancedEvidenceArchiveIndex {
    format_version: u16,
    repository_revision: String,
    execution_protocol: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    admission_protocol: Option<&'static str>,
    files: [&'static str; 3],
}

#[derive(Debug, Serialize)]
struct CounterbalancedAttemptArchiveIndex {
    format_version: u16,
    repository_revision: String,
    execution_protocol: &'static str,
    attempt_protocol: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    admission_protocol: Option<&'static str>,
    files: [&'static str; 3],
}

#[derive(Debug, Serialize)]
struct CounterbalancedAttemptRecord {
    format_version: u16,
    pair_order: CounterbalancedPairOrder,
    #[serde(flatten)]
    outcome: CounterbalancedAttemptOutcome,
}

#[derive(Debug, Serialize)]
#[serde(tag = "status", rename_all = "snake_case")]
enum CounterbalancedAttemptOutcome {
    Failed {
        error_class: ErrorClass,
        error: String,
    },
    Excluded {
        reason: String,
    },
}

pub(super) fn run(args: CounterbalancedArchiveArgs) -> Result<(), CliError> {
    validate_revision(&args.revision)?;
    let trace = read_experiment_trace(&args.trace)?;
    ensure_fresh_counterbalanced_archive_targets(
        &args.first_btree_path,
        &args.first_lsm_path,
        &args.second_btree_path,
        &args.second_lsm_path,
        &args.archive_dir,
    )?;
    let pair_order: CounterbalancedPairOrder = args.pair_order.into();
    let publication_admission =
        validate_publication_admission(&args, current_build_profile(), rustc_host_triple())?;
    let evidence_format_version = if publication_admission.is_some() {
        PUBLICATION_EVIDENCE_ARCHIVE_FORMAT_VERSION
    } else {
        COUNTERBALANCED_EVIDENCE_ARCHIVE_FORMAT_VERSION
    };
    let attempt_format_version = if publication_admission.is_some() {
        PUBLICATION_ATTEMPT_ARCHIVE_FORMAT_VERSION
    } else {
        COUNTERBALANCED_ATTEMPT_ARCHIVE_FORMAT_VERSION
    };

    if let Some(reason) = args.exclude_reason.as_deref() {
        let reason = validate_exclusion_reason(reason)?;
        let environment = build_environment(
            attempt_format_version,
            pair_order,
            &args,
            publication_admission,
        )?;
        let attempt = CounterbalancedAttemptRecord {
            format_version: attempt_format_version,
            pair_order,
            outcome: CounterbalancedAttemptOutcome::Excluded { reason },
        };
        return write_counterbalanced_attempt_archive(
            &args.archive_dir,
            &args.revision,
            &trace,
            &attempt,
            &environment,
        );
    }

    match execute_counterbalanced(&trace, pair_order, &args) {
        Ok(comparison) => {
            let environment = build_environment(
                evidence_format_version,
                pair_order,
                &args,
                publication_admission,
            )?;
            write_counterbalanced_evidence_archive(
                &args.archive_dir,
                &args.revision,
                &trace,
                &comparison,
                &environment,
            )
        }
        Err(error) => {
            let environment = build_environment(
                attempt_format_version,
                pair_order,
                &args,
                publication_admission,
            )?;
            let attempt = CounterbalancedAttemptRecord {
                format_version: attempt_format_version,
                pair_order,
                outcome: CounterbalancedAttemptOutcome::Failed {
                    error_class: error.class(),
                    error: error.to_string(),
                },
            };
            write_counterbalanced_attempt_archive(
                &args.archive_dir,
                &args.revision,
                &trace,
                &attempt,
                &environment,
            )?;
            Err(CliError::Database(error))
        }
    }
}

fn validate_exclusion_reason(reason: &str) -> Result<String, CliError> {
    validate_bounded_metadata("--exclude-reason", reason, MAX_EXCLUSION_REASON_BYTES)
}

fn validate_publication_admission(
    args: &CounterbalancedArchiveArgs,
    build_profile: &'static str,
    rust_target_triple: Option<String>,
) -> Result<Option<PublicationAdmissionRecord>, CliError> {
    match args.admission {
        AdmissionKind::Exploratory => {
            if args.host_cpu.is_some()
                || args.host_memory.is_some()
                || args.mount_options.is_some()
                || args.optimization_flags.is_some()
                || args.analysis_script_version.is_some()
                || args.noise_budget.is_some()
            {
                return Err(CliError::Usage(
                    "publication-only metadata requires --admission publication-warm-v1".to_owned(),
                ));
            }
            Ok(None)
        }
        AdmissionKind::PublicationWarmV1 => {
            if build_profile != "release" {
                return Err(CliError::Usage(
                    "publication-warm-v1 requires a release build; debug binaries are not admitted"
                        .to_owned(),
                ));
            }
            if !matches!(args.cache_state, CacheStateKind::Warm) {
                return Err(CliError::Usage(
                    "publication-warm-v1 requires --cache-state warm; cold_best_effort is not accepted as proof of a cold OS/device cache"
                        .to_owned(),
                ));
            }
            let rust_target_triple = rust_target_triple.ok_or_else(|| {
                CliError::Usage(
                    "publication-warm-v1 requires a Rust host target triple from `rustc -vV`"
                        .to_owned(),
                )
            })?;
            Ok(Some(PublicationAdmissionRecord {
                admission_protocol: PUBLICATION_ADMISSION_PROTOCOL,
                rust_target_triple: validate_bounded_metadata(
                    "rust target triple",
                    &rust_target_triple,
                    MAX_PUBLICATION_METADATA_BYTES,
                )?,
                host_label: required_publication_metadata(
                    "--host-label",
                    args.host_label.as_deref(),
                )?,
                host_cpu: required_publication_metadata("--host-cpu", args.host_cpu.as_deref())?,
                host_memory: required_publication_metadata(
                    "--host-memory",
                    args.host_memory.as_deref(),
                )?,
                storage_device: required_publication_metadata(
                    "--storage-device",
                    args.storage_device.as_deref(),
                )?,
                filesystem: required_publication_metadata(
                    "--filesystem",
                    args.filesystem.as_deref(),
                )?,
                mount_options: required_publication_metadata(
                    "--mount-options",
                    args.mount_options.as_deref(),
                )?,
                cache_policy: PUBLICATION_CACHE_POLICY,
                cache_state: "warm",
                durability_mode: PUBLICATION_DURABILITY_MODE,
                repetition_count: 2,
                optimization_flags: required_publication_metadata(
                    "--optimization-flags",
                    args.optimization_flags.as_deref(),
                )?,
                analysis_script_version: required_publication_metadata(
                    "--analysis-script-version",
                    args.analysis_script_version.as_deref(),
                )?,
                noise_budget: required_publication_metadata(
                    "--noise-budget",
                    args.noise_budget.as_deref(),
                )?,
            }))
        }
    }
}

fn required_publication_metadata(label: &str, value: Option<&str>) -> Result<String, CliError> {
    let value =
        value.ok_or_else(|| CliError::Usage(format!("publication-warm-v1 requires {label}")))?;
    validate_bounded_metadata(label, value, MAX_PUBLICATION_METADATA_BYTES)
}

fn validate_bounded_metadata(
    label: &str,
    value: &str,
    maximum_bytes: usize,
) -> Result<String, CliError> {
    let value = value.trim();
    if value.is_empty() || value.len() > maximum_bytes {
        return Err(CliError::Usage(format!(
            "{label} must contain 1..={maximum_bytes} UTF-8 bytes after trimming"
        )));
    }
    Ok(value.to_owned())
}

fn execute_counterbalanced(
    trace: &ExperimentTrace,
    pair_order: CounterbalancedPairOrder,
    args: &CounterbalancedArchiveArgs,
) -> Result<CounterbalancedExperimentComparisonReport, DbError> {
    let mut btree_paths = [
        args.first_btree_path.clone(),
        args.second_btree_path.clone(),
    ]
    .into_iter();
    let mut lsm_paths = [args.first_lsm_path.clone(), args.second_lsm_path.clone()].into_iter();
    let btree_cache_pages = args.btree_cache_pages;
    compare_experiment_trace_counterbalanced(
        trace,
        pair_order,
        || {
            let path = btree_paths.next().ok_or_else(|| {
                DbError::InvalidInput(
                    "counterbalanced B+ tree factory requested more than two fresh instances"
                        .to_owned(),
                )
            })?;
            BPlusTree::create_new(path, btree_cache_pages).map_err(btree_error_into_db_error)
        },
        || {
            let path = lsm_paths.next().ok_or_else(|| {
                DbError::InvalidInput(
                    "counterbalanced LSM factory requested more than two fresh instances"
                        .to_owned(),
                )
            })?;
            LsmEngine::create_new(path)
        },
    )
}

fn build_environment(
    format_version: u16,
    pair_order: CounterbalancedPairOrder,
    args: &CounterbalancedArchiveArgs,
    publication_admission: Option<PublicationAdmissionRecord>,
) -> Result<CounterbalancedEvidenceArchiveEnvironment, CliError> {
    Ok(CounterbalancedEvidenceArchiveEnvironment {
        format_version,
        repository_revision: args.revision.clone(),
        execution_protocol: COUNTERBALANCED_EXECUTION_PROTOCOL,
        pair_order,
        db_lab_version: env!("CARGO_PKG_VERSION"),
        target_os: std::env::consts::OS,
        target_arch: std::env::consts::ARCH,
        build_profile: current_build_profile(),
        rustc_version: rustc_version(),
        host_label: args.host_label.clone(),
        filesystem: args.filesystem.clone(),
        storage_device: args.storage_device.clone(),
        cache_state: args.cache_state.as_str(),
        btree_cache_pages: args.btree_cache_pages,
        recorded_unix_seconds: SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|error| CliError::Usage(format!("system clock precedes Unix epoch: {error}")))?
            .as_secs(),
        publication_admission,
        notes: args.notes.clone(),
    })
}

const fn current_build_profile() -> &'static str {
    if cfg!(debug_assertions) {
        "debug"
    } else {
        "release"
    }
}

fn rustc_host_triple() -> Option<String> {
    let output = std::process::Command::new("rustc")
        .arg("-vV")
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let output = String::from_utf8(output.stdout).ok()?;
    output
        .lines()
        .find_map(|line| line.strip_prefix("host: "))
        .map(str::trim)
        .filter(|host| !host.is_empty())
        .map(str::to_owned)
}

fn ensure_fresh_counterbalanced_archive_targets(
    first_btree_path: &Path,
    first_lsm_path: &Path,
    second_btree_path: &Path,
    second_lsm_path: &Path,
    archive_dir: &Path,
) -> Result<(), CliError> {
    let targets = [
        ("first B+ tree", first_btree_path),
        ("first LSM", first_lsm_path),
        ("second B+ tree", second_btree_path),
        ("second LSM", second_lsm_path),
        ("archive", archive_dir),
    ];
    for left in 0..targets.len() {
        for right in (left + 1)..targets.len() {
            if targets[left].1 == targets[right].1 {
                return Err(CliError::Usage(format!(
                    "counterbalanced archive targets must be distinct: {} and {} both use {}",
                    targets[left].0,
                    targets[right].0,
                    targets[left].1.display()
                )));
            }
        }
    }
    for (label, path) in targets {
        if path.exists() {
            return Err(CliError::Usage(format!(
                "counterbalanced experiment {label} path already exists: {}",
                path.display()
            )));
        }
    }
    Ok(())
}

fn btree_error_into_db_error(error: BtreeError) -> DbError {
    match error {
        BtreeError::InvalidInput(message) => DbError::InvalidInput(message),
        BtreeError::Io(error) => DbError::Io(error),
        BtreeError::Corruption { offset, reason } => DbError::Corruption { offset, reason },
        BtreeError::UnsupportedVersion { found, supported } => DbError::UnsupportedVersion {
            format: "B+ tree",
            found,
            supported,
        },
        BtreeError::Poisoned => DbError::Poisoned,
    }
}

fn write_counterbalanced_evidence_archive(
    archive_dir: &Path,
    revision: &str,
    trace: &ExperimentTrace,
    comparison: &CounterbalancedExperimentComparisonReport,
    environment: &CounterbalancedEvidenceArchiveEnvironment,
) -> Result<(), CliError> {
    fs::create_dir(archive_dir)?;
    let result = (|| {
        write_new_json(&archive_dir.join("trace.json"), trace)?;
        write_new_json(&archive_dir.join("counterbalanced.json"), comparison)?;
        write_new_json(&archive_dir.join("environment.json"), environment)?;
        write_new_json(
            &archive_dir.join("index.json"),
            &CounterbalancedEvidenceArchiveIndex {
                format_version: environment.format_version,
                repository_revision: revision.to_owned(),
                execution_protocol: COUNTERBALANCED_EXECUTION_PROTOCOL,
                admission_protocol: environment
                    .publication_admission
                    .as_ref()
                    .map(|_| PUBLICATION_ADMISSION_PROTOCOL),
                files: ["trace.json", "counterbalanced.json", "environment.json"],
            },
        )
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(archive_dir);
    }
    result
}

fn write_counterbalanced_attempt_archive(
    archive_dir: &Path,
    revision: &str,
    trace: &ExperimentTrace,
    attempt: &CounterbalancedAttemptRecord,
    environment: &CounterbalancedEvidenceArchiveEnvironment,
) -> Result<(), CliError> {
    fs::create_dir(archive_dir)?;
    let result = (|| {
        write_new_json(&archive_dir.join("trace.json"), trace)?;
        write_new_json(&archive_dir.join("attempt.json"), attempt)?;
        write_new_json(&archive_dir.join("environment.json"), environment)?;
        write_new_json(
            &archive_dir.join("index.json"),
            &CounterbalancedAttemptArchiveIndex {
                format_version: environment.format_version,
                repository_revision: revision.to_owned(),
                execution_protocol: COUNTERBALANCED_EXECUTION_PROTOCOL,
                attempt_protocol: COUNTERBALANCED_ATTEMPT_PROTOCOL,
                admission_protocol: environment
                    .publication_admission
                    .as_ref()
                    .map(|_| PUBLICATION_ADMISSION_PROTOCOL),
                files: ["trace.json", "attempt.json", "environment.json"],
            },
        )
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(archive_dir);
    }
    result
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::PathBuf;

    use db_core::{generate_experiment_trace, ExperimentGeneratorConfig, ExperimentProfile};
    use serde_json::Value;
    use tempfile::tempdir;

    use super::{
        ensure_fresh_counterbalanced_archive_targets, run, validate_publication_admission,
        AdmissionKind, CounterbalancedArchiveArgs, CounterbalancedPairOrderKind,
        PUBLICATION_ADMISSION_PROTOCOL, PUBLICATION_CACHE_POLICY, PUBLICATION_DURABILITY_MODE,
    };
    use crate::CacheStateKind;

    #[test]
    fn duplicate_counterbalanced_targets_fail_before_creation() {
        let directory = tempdir().expect("temporary directory");
        let duplicate = directory.path().join("same");
        let error = ensure_fresh_counterbalanced_archive_targets(
            &duplicate,
            &duplicate,
            &directory.path().join("btree-b.db"),
            &directory.path().join("lsm-b"),
            &directory.path().join("archive"),
        )
        .expect_err("duplicate paths must fail");
        assert!(error.to_string().contains("must be distinct"));
    }

    #[test]
    fn real_counterbalanced_archive_records_pair_provenance() {
        let directory = tempdir().expect("temporary directory");
        let trace_path = write_trace(&directory);
        let archive_dir = directory.path().join("archive");

        run(base_args(&directory, trace_path, archive_dir.clone()))
            .expect("write counterbalanced archive");

        let comparison: Value = serde_json::from_slice(
            &fs::read(archive_dir.join("counterbalanced.json")).expect("read comparison"),
        )
        .expect("parse comparison");
        assert_eq!(comparison["pair_order"], "right_then_left_first");
        assert_eq!(comparison["first"]["execution_order"], "right_then_left");
        assert_eq!(comparison["second"]["execution_order"], "left_then_right");

        let environment: Value = serde_json::from_slice(
            &fs::read(archive_dir.join("environment.json")).expect("read environment"),
        )
        .expect("parse environment");
        assert_eq!(environment["format_version"], 2);
        assert_eq!(
            environment["execution_protocol"],
            "fresh_counterbalanced_ab_ba"
        );
        assert_eq!(environment["pair_order"], "right_then_left_first");
        assert_eq!(environment["cache_state"], "warm");
        assert!(environment.get("publication_admission").is_none());

        let index: Value =
            serde_json::from_slice(&fs::read(archive_dir.join("index.json")).expect("read index"))
                .expect("parse index");
        assert_eq!(index["format_version"], 2);
        assert!(index.get("admission_protocol").is_none());
        assert_eq!(
            index["files"],
            serde_json::json!(["trace.json", "counterbalanced.json", "environment.json"])
        );
    }

    #[test]
    fn excluded_attempt_is_archived_without_creating_engines() {
        let directory = tempdir().expect("temporary directory");
        let trace_path = write_trace(&directory);
        let archive_dir = directory.path().join("excluded-archive");
        let mut args = base_args(&directory, trace_path, archive_dir.clone());
        args.exclude_reason = Some("host load exceeded the frozen admission threshold".to_owned());
        let first_btree_path = args.first_btree_path.clone();
        let first_lsm_path = args.first_lsm_path.clone();

        run(args).expect("record excluded attempt");

        assert!(!first_btree_path.exists());
        assert!(!first_lsm_path.exists());
        let attempt: Value = serde_json::from_slice(
            &fs::read(archive_dir.join("attempt.json")).expect("read attempt"),
        )
        .expect("parse attempt");
        assert_eq!(attempt["format_version"], 3);
        assert_eq!(attempt["status"], "excluded");
        assert_eq!(
            attempt["reason"],
            "host load exceeded the frozen admission threshold"
        );
    }

    #[test]
    fn failed_attempt_is_archived_and_still_returns_an_error() {
        let directory = tempdir().expect("temporary directory");
        let trace_path = write_trace(&directory);
        let archive_dir = directory.path().join("failed-archive");
        let mut args = base_args(&directory, trace_path, archive_dir.clone());
        args.first_btree_path = directory.path().join("missing-parent").join("tree.db");

        let error = run(args).expect_err("execution failure must retain non-zero CLI semantics");
        assert!(error.to_string().contains("I/O error"));

        let attempt: Value = serde_json::from_slice(
            &fs::read(archive_dir.join("attempt.json")).expect("read attempt"),
        )
        .expect("parse attempt");
        assert_eq!(attempt["format_version"], 3);
        assert_eq!(attempt["status"], "failed");
        assert_eq!(attempt["error_class"], "io");
    }

    #[test]
    fn publication_admission_rejects_debug_and_unverified_cold_state() {
        let directory = tempdir().expect("temporary directory");
        let trace = write_trace(&directory);
        let mut args = publication_args(&directory, trace, directory.path().join("publication"));

        let debug_error = validate_publication_admission(
            &args,
            "debug",
            Some("x86_64-unknown-linux-gnu".to_owned()),
        )
        .expect_err("debug build must not be admitted");
        assert!(debug_error.to_string().contains("release build"));

        args.cache_state = CacheStateKind::ColdBestEffort;
        let cold_error = validate_publication_admission(
            &args,
            "release",
            Some("x86_64-unknown-linux-gnu".to_owned()),
        )
        .expect_err("best-effort cold cache must not be admitted");
        assert!(cold_error
            .to_string()
            .contains("requires --cache-state warm"));
    }

    #[test]
    fn publication_admission_freezes_complete_warm_protocol_metadata() {
        let directory = tempdir().expect("temporary directory");
        let trace = write_trace(&directory);
        let args = publication_args(&directory, trace, directory.path().join("publication"));

        let admission = validate_publication_admission(
            &args,
            "release",
            Some("x86_64-unknown-linux-gnu".to_owned()),
        )
        .expect("validate publication admission")
        .expect("publication record");
        assert_eq!(admission.admission_protocol, PUBLICATION_ADMISSION_PROTOCOL);
        assert_eq!(admission.cache_policy, PUBLICATION_CACHE_POLICY);
        assert_eq!(admission.cache_state, "warm");
        assert_eq!(admission.durability_mode, PUBLICATION_DURABILITY_MODE);
        assert_eq!(admission.repetition_count, 2);
        assert_eq!(admission.filesystem, "ext4");
        assert_eq!(admission.mount_options, "rw,noatime");
        assert_eq!(admission.analysis_script_version, "analysis@abc123");
    }

    #[test]
    fn exploratory_mode_rejects_publication_only_metadata() {
        let directory = tempdir().expect("temporary directory");
        let trace = write_trace(&directory);
        let mut args = base_args(&directory, trace, directory.path().join("archive"));
        args.host_cpu = Some("cpu".to_owned());
        let error = validate_publication_admission(&args, "release", None)
            .expect_err("publication metadata without admission must fail");
        assert!(error
            .to_string()
            .contains("requires --admission publication-warm-v1"));
    }

    fn write_trace(directory: &tempfile::TempDir) -> PathBuf {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 0x2026_0829,
            profile: ExperimentProfile::RandomWrite,
            operations: 4,
            key_space: 8,
            value_bytes: 8,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");
        let trace_path = directory.path().join("trace.json");
        fs::write(
            &trace_path,
            serde_json::to_vec_pretty(&trace).expect("serialize trace"),
        )
        .expect("write trace");
        trace_path
    }

    fn base_args(
        directory: &tempfile::TempDir,
        trace: PathBuf,
        archive_dir: PathBuf,
    ) -> CounterbalancedArchiveArgs {
        CounterbalancedArchiveArgs {
            trace,
            first_btree_path: directory.path().join("btree-a.db"),
            first_lsm_path: directory.path().join("lsm-a"),
            second_btree_path: directory.path().join("btree-b.db"),
            second_lsm_path: directory.path().join("lsm-b"),
            pair_order: CounterbalancedPairOrderKind::RightThenLeftFirst,
            btree_cache_pages: 8,
            revision: "test-revision".to_owned(),
            archive_dir,
            host_label: Some("test-host".to_owned()),
            host_cpu: None,
            host_memory: None,
            filesystem: None,
            mount_options: None,
            storage_device: None,
            cache_state: CacheStateKind::Warm,
            admission: AdmissionKind::Exploratory,
            optimization_flags: None,
            analysis_script_version: None,
            noise_budget: None,
            exclude_reason: None,
            notes: None,
        }
    }

    fn publication_args(
        directory: &tempfile::TempDir,
        trace: PathBuf,
        archive_dir: PathBuf,
    ) -> CounterbalancedArchiveArgs {
        let mut args = base_args(directory, trace, archive_dir);
        args.admission = AdmissionKind::PublicationWarmV1;
        args.host_label = Some("perf-host-01".to_owned());
        args.host_cpu = Some("Example CPU / pinned topology".to_owned());
        args.host_memory = Some("64 GiB / fixed channels".to_owned());
        args.filesystem = Some("ext4".to_owned());
        args.mount_options = Some("rw,noatime".to_owned());
        args.storage_device = Some("Example NVMe model".to_owned());
        args.optimization_flags = Some("--release; RUSTFLAGS=-C target-cpu=native".to_owned());
        args.analysis_script_version = Some("analysis@abc123".to_owned());
        args.noise_budget = Some("host-noise-budget-v1".to_owned());
        args
    }
}
