use std::collections::BTreeMap;
use std::fs::{self, OpenOptions};
use std::io::{self, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::{SystemTime, UNIX_EPOCH};

use clap::{Parser, ValueEnum};
use db_core::{
    run_counterbalanced_experiment_batch_captured, CounterbalancedBatchComparisonFailureEvidence,
    CounterbalancedExperimentBatchReport, DbError, ExperimentAttemptAdmission,
    ExperimentInstanceContext, ExperimentTrace, MAX_EXPERIMENT_BATCH_PAIRS,
};
use db_storage_btree::{BPlusTree, BtreeError};
use db_storage_lsm::LsmEngine;
use serde::Serialize;
use thiserror::Error;

const MAX_JSON_INPUT_BYTES: u64 = 64 * 1024 * 1024;
const MAX_EXCLUSION_REASON_BYTES: usize = 4 * 1024;
const MAX_PUBLICATION_METADATA_BYTES: usize = 4 * 1024;
const BATCH_ARCHIVE_FORMAT_VERSION: u16 = 6;
const BATCH_PUBLICATION_ARCHIVE_FORMAT_VERSION: u16 = 7;
const BATCH_CONTEXTUAL_FAILURE_ARCHIVE_FORMAT_VERSION: u16 = 10;
const BATCH_PUBLICATION_CONTEXTUAL_FAILURE_ARCHIVE_FORMAT_VERSION: u16 = 11;
const BATCH_EXECUTION_PROTOCOL: &str = "fresh_counterbalanced_repeated_batch_v1";
const BATCH_ATTEMPT_PROTOCOL: &str = "retain_all_requested_pairs_v1";
const BATCH_COMPARISON_FAILURE_PROTOCOL: &str = "ordered_comparison_failure_sidecar_v2";
const ENGINE_LAYOUT: &str = "pair-{pair_index:06}/repetition-{repetition_index}/{btree.db|lsm}";
const PUBLICATION_ADMISSION_PROTOCOL: &str = "publication_warm_v1";
const PUBLICATION_CACHE_POLICY: &str = "trace_induced_warm";
const PUBLICATION_DURABILITY_MODE: &str = "synced_single_operation";
const PUBLICATION_PAIR_ORDER_POLICY: &str = "pair_seed_low_bit_then_alternate";

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-batch",
    version,
    about = "Archive every included, failed, and excluded Phase 4 counterbalanced pair"
)]
struct Cli {
    /// Versioned experiment trace JSON file.
    #[arg(long)]
    trace: PathBuf,
    /// New root directory used for all fresh B+ tree and LSM instances.
    #[arg(long)]
    engine_root: PathBuf,
    /// New immutable evidence archive directory.
    #[arg(long)]
    archive_dir: PathBuf,
    /// Seed whose low bit chooses the first outer pair order; later pairs alternate.
    #[arg(long)]
    pair_seed: u64,
    /// Number of fresh counterbalanced pairs to request.
    #[arg(long)]
    pairs: u32,
    /// B+ tree validated-page cache capacity for every fresh instance.
    #[arg(long, default_value_t = 64)]
    btree_cache_pages: usize,
    /// Exact source revision represented by the binary/run, normally a full Git commit SHA.
    #[arg(long)]
    revision: String,
    /// Exclude one zero-based pair before engine creation as `INDEX=REASON`; may be repeated.
    #[arg(long = "exclude-pair")]
    exclude_pairs: Vec<String>,
    /// Human-readable host identity without secrets.
    #[arg(long)]
    host_label: Option<String>,
    /// Host CPU model/topology label required by publication admission.
    #[arg(long)]
    host_cpu: Option<String>,
    /// Host memory configuration label required by publication admission.
    #[arg(long)]
    host_memory: Option<String>,
    /// Filesystem under test, when known.
    #[arg(long)]
    filesystem: Option<String>,
    /// Filesystem mount options required by publication admission.
    #[arg(long)]
    mount_options: Option<String>,
    /// Storage device/model label, when known. Do not include serial numbers or credentials.
    #[arg(long)]
    storage_device: Option<String>,
    /// Declared cache state for this batch.
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
    /// Optional free-form experiment note. Do not include secrets.
    #[arg(long)]
    notes: Option<String>,
}

#[derive(Debug, Clone, Copy, ValueEnum)]
enum CacheStateKind {
    Unspecified,
    Warm,
    ColdBestEffort,
}

impl CacheStateKind {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Unspecified => "unspecified",
            Self::Warm => "warm",
            Self::ColdBestEffort => "cold_best_effort",
        }
    }
}

#[derive(Debug, Clone, Copy, ValueEnum)]
enum AdmissionKind {
    /// Exploratory evidence may contain incomplete environment metadata and is not publication-grade.
    Exploratory,
    /// Strict release-only warm-cache admission with complete reproducibility metadata.
    PublicationWarmV1,
}

#[derive(Debug, Clone, Serialize)]
struct BatchPublicationAdmissionRecord {
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
    pair_order_policy: &'static str,
    requested_pairs: u32,
    ordered_comparisons_per_included_pair: u8,
    optimization_flags: String,
    analysis_script_version: String,
    noise_budget: String,
}

#[derive(Debug, Serialize)]
struct BatchArchiveEnvironment {
    format_version: u16,
    repository_revision: String,
    execution_protocol: &'static str,
    attempt_protocol: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    comparison_failure_protocol: Option<&'static str>,
    pair_seed: u64,
    requested_pairs: u32,
    engine_layout: &'static str,
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
    publication_admission: Option<BatchPublicationAdmissionRecord>,
    notes: Option<String>,
}

#[derive(Debug, Serialize)]
struct BatchArchiveIndex {
    format_version: u16,
    repository_revision: String,
    execution_protocol: &'static str,
    attempt_protocol: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    comparison_failure_protocol: Option<&'static str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    admission_protocol: Option<&'static str>,
    files: Vec<&'static str>,
}

#[derive(Debug, Error)]
enum CliError {
    #[error("{0}")]
    Usage(String),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
    #[error("invalid JSON: {0}")]
    Json(#[from] serde_json::Error),
    #[error("batch archive retained {failed_pairs} failed pair(s); evidence was written to {archive_dir}")]
    BatchFailures {
        failed_pairs: u32,
        archive_dir: String,
    },
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}

fn run(args: Cli) -> Result<(), CliError> {
    validate_revision(&args.revision)?;
    if args.pairs == 0 || args.pairs > MAX_EXPERIMENT_BATCH_PAIRS {
        return Err(CliError::Usage(format!(
            "--pairs is {}; expected 1..={MAX_EXPERIMENT_BATCH_PAIRS}",
            args.pairs
        )));
    }
    let trace = read_experiment_trace(&args.trace)?;
    let exclusions = parse_exclusions(&args.exclude_pairs, args.pairs)?;
    ensure_fresh_targets(&args.engine_root, &args.archive_dir)?;
    let publication_admission = validate_publication_admission(
        &args,
        &exclusions,
        current_build_profile(),
        rustc_host_triple(),
    )?;

    fs::create_dir_all(&args.engine_root)?;
    let engine_root = args.engine_root.clone();
    let btree_cache_pages = args.btree_cache_pages;
    let captured = run_counterbalanced_experiment_batch_captured(
        &trace,
        args.pair_seed,
        args.pairs,
        |context| {
            let path = instance_path(&engine_root, context, "btree.db");
            prepare_instance_parent(&path)?;
            BPlusTree::create_new(path, btree_cache_pages).map_err(btree_error_into_db_error)
        },
        |context| {
            let path = instance_path(&engine_root, context, "lsm");
            prepare_instance_parent(&path)?;
            LsmEngine::create_new(path)
        },
        |context| match exclusions.get(&context.pair_index) {
            Some(reason) => ExperimentAttemptAdmission::Exclude {
                reason: reason.clone(),
            },
            None => ExperimentAttemptAdmission::Include,
        },
    )?;
    let has_comparison_failures = !captured.comparison_failures.is_empty();
    let archive_format_version =
        select_archive_format_version(publication_admission.is_some(), has_comparison_failures);
    let environment = build_environment(
        &args,
        archive_format_version,
        publication_admission,
        has_comparison_failures,
    )?;
    write_batch_archive(
        &args.archive_dir,
        &args.revision,
        &trace,
        &captured.batch,
        &captured.comparison_failures,
        &environment,
    )?;

    if captured.batch.failed_pairs > 0 {
        return Err(CliError::BatchFailures {
            failed_pairs: captured.batch.failed_pairs,
            archive_dir: args.archive_dir.display().to_string(),
        });
    }
    Ok(())
}

const fn select_archive_format_version(publication: bool, has_comparison_failures: bool) -> u16 {
    match (publication, has_comparison_failures) {
        (false, false) => BATCH_ARCHIVE_FORMAT_VERSION,
        (true, false) => BATCH_PUBLICATION_ARCHIVE_FORMAT_VERSION,
        (false, true) => BATCH_CONTEXTUAL_FAILURE_ARCHIVE_FORMAT_VERSION,
        (true, true) => BATCH_PUBLICATION_CONTEXTUAL_FAILURE_ARCHIVE_FORMAT_VERSION,
    }
}

fn read_experiment_trace(path: &Path) -> Result<ExperimentTrace, CliError> {
    let metadata = fs::metadata(path)?;
    if metadata.len() > MAX_JSON_INPUT_BYTES {
        return Err(CliError::Usage(format!(
            "experiment trace file has {} bytes; maximum is {MAX_JSON_INPUT_BYTES}",
            metadata.len()
        )));
    }
    let encoded = fs::read(path)?;
    let trace: ExperimentTrace = serde_json::from_slice(&encoded)?;
    trace.validate()?;
    Ok(trace)
}

fn parse_exclusions(values: &[String], pairs: u32) -> Result<BTreeMap<u32, String>, CliError> {
    let mut exclusions = BTreeMap::new();
    for encoded in values {
        let (index, reason) = encoded.split_once('=').ok_or_else(|| {
            CliError::Usage(format!(
                "--exclude-pair must use INDEX=REASON; got {encoded:?}"
            ))
        })?;
        let index = index.trim().parse::<u32>().map_err(|_| {
            CliError::Usage(format!(
                "--exclude-pair index must be a zero-based integer; got {index:?}"
            ))
        })?;
        if index >= pairs {
            return Err(CliError::Usage(format!(
                "--exclude-pair index {index} is outside requested pair range 0..{}",
                pairs - 1
            )));
        }
        let reason = reason.trim();
        if reason.is_empty() || reason.len() > MAX_EXCLUSION_REASON_BYTES {
            return Err(CliError::Usage(format!(
                "--exclude-pair reason must contain 1..={MAX_EXCLUSION_REASON_BYTES} UTF-8 bytes after trimming"
            )));
        }
        if exclusions.insert(index, reason.to_owned()).is_some() {
            return Err(CliError::Usage(format!(
                "--exclude-pair index {index} was specified more than once"
            )));
        }
    }
    Ok(exclusions)
}

fn validate_publication_admission(
    args: &Cli,
    exclusions: &BTreeMap<u32, String>,
    build_profile: &'static str,
    rust_target_triple: Option<String>,
) -> Result<Option<BatchPublicationAdmissionRecord>, CliError> {
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
            if exclusions.len() == args.pairs as usize {
                return Err(CliError::Usage(
                    "publication-warm-v1 requires at least one included pair; all requested pairs were excluded"
                        .to_owned(),
                ));
            }
            let rust_target_triple = rust_target_triple.ok_or_else(|| {
                CliError::Usage(
                    "publication-warm-v1 requires a Rust host target triple from `rustc -vV`"
                        .to_owned(),
                )
            })?;
            Ok(Some(BatchPublicationAdmissionRecord {
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
                pair_order_policy: PUBLICATION_PAIR_ORDER_POLICY,
                requested_pairs: args.pairs,
                ordered_comparisons_per_included_pair: 2,
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

fn ensure_fresh_targets(engine_root: &Path, archive_dir: &Path) -> Result<(), CliError> {
    if engine_root == archive_dir
        || engine_root.starts_with(archive_dir)
        || archive_dir.starts_with(engine_root)
    {
        return Err(CliError::Usage(
            "--engine-root and --archive-dir must be distinct, non-nested paths".to_owned(),
        ));
    }
    for (label, path) in [("engine root", engine_root), ("archive", archive_dir)] {
        if path.exists() {
            return Err(CliError::Usage(format!(
                "batch experiment {label} path already exists: {}",
                path.display()
            )));
        }
    }
    Ok(())
}

fn instance_path(root: &Path, context: ExperimentInstanceContext, leaf: &str) -> PathBuf {
    root.join(format!("pair-{:06}", context.attempt.pair_index))
        .join(format!("repetition-{}", context.repetition_index))
        .join(leaf)
}

fn prepare_instance_parent(path: &Path) -> Result<(), DbError> {
    let parent = path.parent().ok_or_else(|| {
        DbError::InvalidInput(format!(
            "engine instance path has no parent: {}",
            path.display()
        ))
    })?;
    fs::create_dir_all(parent).map_err(DbError::Io)
}

fn validate_revision(revision: &str) -> Result<(), CliError> {
    let revision = revision.trim();
    if revision.is_empty()
        || revision.len() > 128
        || !revision
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    {
        return Err(CliError::Usage(
            "--revision must be 1..=128 ASCII alphanumeric, '-', '_', or '.' characters".to_owned(),
        ));
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

fn build_environment(
    args: &Cli,
    format_version: u16,
    publication_admission: Option<BatchPublicationAdmissionRecord>,
    has_comparison_failures: bool,
) -> Result<BatchArchiveEnvironment, CliError> {
    Ok(BatchArchiveEnvironment {
        format_version,
        repository_revision: args.revision.clone(),
        execution_protocol: BATCH_EXECUTION_PROTOCOL,
        attempt_protocol: BATCH_ATTEMPT_PROTOCOL,
        comparison_failure_protocol: has_comparison_failures
            .then_some(BATCH_COMPARISON_FAILURE_PROTOCOL),
        pair_seed: args.pair_seed,
        requested_pairs: args.pairs,
        engine_layout: ENGINE_LAYOUT,
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

fn rustc_version() -> Option<String> {
    let output = std::process::Command::new("rustc")
        .arg("--version")
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    String::from_utf8(output.stdout)
        .ok()
        .map(|value| value.trim().to_owned())
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

fn write_batch_archive(
    archive_dir: &Path,
    revision: &str,
    trace: &ExperimentTrace,
    report: &CounterbalancedExperimentBatchReport,
    comparison_failures: &[CounterbalancedBatchComparisonFailureEvidence],
    environment: &BatchArchiveEnvironment,
) -> Result<(), CliError> {
    fs::create_dir(archive_dir)?;
    let result = (|| {
        write_new_json(&archive_dir.join("trace.json"), trace)?;
        write_new_json(&archive_dir.join("batch.json"), report)?;
        write_new_json(&archive_dir.join("environment.json"), environment)?;
        let mut files = vec!["trace.json", "batch.json", "environment.json"];
        if !comparison_failures.is_empty() {
            write_new_json(
                &archive_dir.join("comparison-failures.json"),
                &comparison_failures,
            )?;
            files.push("comparison-failures.json");
        }
        write_new_json(
            &archive_dir.join("index.json"),
            &BatchArchiveIndex {
                format_version: environment.format_version,
                repository_revision: revision.to_owned(),
                execution_protocol: BATCH_EXECUTION_PROTOCOL,
                attempt_protocol: BATCH_ATTEMPT_PROTOCOL,
                comparison_failure_protocol: environment.comparison_failure_protocol,
                admission_protocol: environment
                    .publication_admission
                    .as_ref()
                    .map(|_| PUBLICATION_ADMISSION_PROTOCOL),
                files,
            },
        )
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(archive_dir);
    }
    result
}

fn write_new_json(path: &Path, value: &impl Serialize) -> Result<(), CliError> {
    let file = OpenOptions::new().write(true).create_new(true).open(path)?;
    let mut writer = BufWriter::new(file);
    serde_json::to_writer_pretty(&mut writer, value)?;
    writer.write_all(b"\n")?;
    writer.flush()?;
    writer.get_ref().sync_all()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::fs;
    use std::path::PathBuf;

    use db_core::{
        generate_experiment_trace, CounterbalancedBatchComparisonFailureEvidence,
        CounterbalancedComparisonFailureEvidence, CounterbalancedExperimentBatchReport,
        CounterbalancedPairOrder, ErrorClass, ExperimentAttemptContext, ExperimentExecutionOrder,
        ExperimentGeneratorConfig, ExperimentProfile, OperationalTimingFailureSample,
        OperationalTimingReport, OrderedExperimentFailureEvidence,
    };
    use serde_json::Value;
    use tempfile::tempdir;

    use super::{
        build_environment, parse_exclusions, run, select_archive_format_version,
        validate_publication_admission, write_batch_archive, AdmissionKind, CacheStateKind, Cli,
        BATCH_COMPARISON_FAILURE_PROTOCOL, PUBLICATION_ADMISSION_PROTOCOL,
        PUBLICATION_CACHE_POLICY, PUBLICATION_DURABILITY_MODE, PUBLICATION_PAIR_ORDER_POLICY,
    };

    #[test]
    fn exclusion_specs_are_bounded_unique_and_in_range() {
        let values = vec!["1=scheduled cooldown".to_owned(), "3=host noise".to_owned()];
        let parsed = parse_exclusions(&values, 4).expect("parse exclusions");
        assert_eq!(
            parsed.get(&1).map(String::as_str),
            Some("scheduled cooldown")
        );
        assert_eq!(parsed.get(&3).map(String::as_str), Some("host noise"));

        let duplicate = vec!["1=first".to_owned(), "1=second".to_owned()];
        assert!(parse_exclusions(&duplicate, 2)
            .expect_err("duplicate index must fail")
            .to_string()
            .contains("more than once"));
        assert!(parse_exclusions(&["2=outside".to_owned()], 2)
            .expect_err("out-of-range index must fail")
            .to_string()
            .contains("outside requested pair range"));
    }

    #[test]
    fn real_batch_archive_retains_included_and_excluded_pairs() {
        let directory = tempdir().expect("temporary directory");
        let trace_path = write_trace(&directory);
        let engine_root = directory.path().join("engines");
        let archive_dir = directory.path().join("archive");
        run(base_args(
            &directory,
            trace_path,
            engine_root.clone(),
            archive_dir.clone(),
        ))
        .expect("archive batch");

        let batch: Value =
            serde_json::from_slice(&fs::read(archive_dir.join("batch.json")).expect("read batch"))
                .expect("parse batch");
        assert_eq!(batch["requested_pairs"], 3);
        assert_eq!(batch["included_pairs"], 2);
        assert_eq!(batch["failed_pairs"], 0);
        assert_eq!(batch["excluded_pairs"], 1);
        assert_eq!(
            batch["attempts"][0]["context"]["pair_order"],
            "left_then_right_first"
        );
        assert_eq!(batch["attempts"][1]["disposition"], "excluded");
        assert_eq!(
            batch["attempts"][1]["exclusion_reason"],
            "scheduled thermal cooldown"
        );
        assert_eq!(
            batch["attempts"][2]["context"]["pair_order"],
            "left_then_right_first"
        );

        let environment: Value = serde_json::from_slice(
            &fs::read(archive_dir.join("environment.json")).expect("read environment"),
        )
        .expect("parse environment");
        assert_eq!(environment["format_version"], 6);
        assert_eq!(
            environment["execution_protocol"],
            "fresh_counterbalanced_repeated_batch_v1"
        );
        assert_eq!(
            environment["attempt_protocol"],
            "retain_all_requested_pairs_v1"
        );
        assert_eq!(environment["pair_seed"], 0);
        assert_eq!(environment["requested_pairs"], 3);
        assert_eq!(environment["cache_state"], "warm");
        assert!(environment.get("publication_admission").is_none());
        assert!(environment.get("comparison_failure_protocol").is_none());
        assert!(!archive_dir.join("comparison-failures.json").exists());

        let index: Value =
            serde_json::from_slice(&fs::read(archive_dir.join("index.json")).expect("read index"))
                .expect("parse index");
        assert_eq!(index["format_version"], 6);
        assert!(index.get("admission_protocol").is_none());
        assert!(index.get("comparison_failure_protocol").is_none());
        assert_eq!(
            index["files"],
            serde_json::json!(["trace.json", "batch.json", "environment.json"])
        );

        assert!(engine_root
            .join("pair-000000/repetition-0/btree.db")
            .exists());
        assert!(engine_root.join("pair-000000/repetition-0/lsm").exists());
        assert!(!engine_root.join("pair-000001").exists());
        assert!(engine_root.join("pair-000002/repetition-1/lsm").exists());
    }

    #[test]
    fn archive_format_bumps_to_contextual_sidecar_versions() {
        assert_eq!(select_archive_format_version(false, false), 6);
        assert_eq!(select_archive_format_version(true, false), 7);
        assert_eq!(select_archive_format_version(false, true), 10);
        assert_eq!(select_archive_format_version(true, true), 11);
    }

    #[test]
    fn captured_comparison_failure_writes_pair_indexed_immutable_sidecar() {
        let directory = tempdir().expect("temporary directory");
        let trace_path = write_trace(&directory);
        let trace: db_core::ExperimentTrace =
            serde_json::from_slice(&fs::read(&trace_path).expect("read generated trace"))
                .expect("decode generated trace");
        let args = base_args(
            &directory,
            trace_path,
            directory.path().join("unused-engines"),
            directory.path().join("archive"),
        );
        let failure_sample = OperationalTimingFailureSample {
            measured_step_index: Some(1),
            duration_ns: 123,
            work: None,
            error_class: ErrorClass::Io,
        };
        let mut right_timing = OperationalTimingReport::default();
        right_timing
            .compaction_stall_failure_samples
            .push(failure_sample);
        let pair_context = ExperimentAttemptContext {
            pair_index: 4,
            pair_order: CounterbalancedPairOrder::LeftThenRightFirst,
        };
        let failures = vec![CounterbalancedBatchComparisonFailureEvidence {
            context: pair_context,
            failure: CounterbalancedComparisonFailureEvidence {
                pair_order: pair_context.pair_order,
                repetition_index: 0,
                completed_first: None,
                ordered_failure: Box::new(OrderedExperimentFailureEvidence {
                    execution_order: ExperimentExecutionOrder::LeftThenRight,
                    error_class: ErrorClass::Io,
                    message: "synthetic compaction fault".to_owned(),
                    left_operational_timing: OperationalTimingReport::default(),
                    right_operational_timing: right_timing,
                }),
            },
        }];
        let report = CounterbalancedExperimentBatchReport {
            trace: trace.clone(),
            pair_seed: 0,
            requested_pairs: 5,
            included_pairs: 0,
            failed_pairs: 1,
            excluded_pairs: 4,
            attempts: Vec::new(),
        };
        let environment = build_environment(&args, 10, None, true).expect("build v10 environment");
        write_batch_archive(
            &args.archive_dir,
            &args.revision,
            &trace,
            &report,
            &failures,
            &environment,
        )
        .expect("write captured failure archive");

        let sidecar: Value = serde_json::from_slice(
            &fs::read(args.archive_dir.join("comparison-failures.json"))
                .expect("read comparison failure sidecar"),
        )
        .expect("parse comparison failure sidecar");
        assert_eq!(sidecar[0]["context"]["pair_index"], 4);
        assert_eq!(
            sidecar[0]["context"]["pair_order"],
            sidecar[0]["failure"]["pair_order"]
        );
        assert_eq!(sidecar[0]["failure"]["repetition_index"], 0);
        assert_eq!(
            sidecar[0]["failure"]["ordered_failure"]["error_class"],
            "io"
        );
        assert_eq!(
            sidecar[0]["failure"]["ordered_failure"]["right_operational_timing"]
                ["compaction_stall_failure_samples"][0]["duration_ns"],
            123
        );

        let environment: Value = serde_json::from_slice(
            &fs::read(args.archive_dir.join("environment.json")).expect("read environment"),
        )
        .expect("parse environment");
        assert_eq!(environment["format_version"], 10);
        assert_eq!(
            environment["comparison_failure_protocol"],
            BATCH_COMPARISON_FAILURE_PROTOCOL
        );
        let index: Value = serde_json::from_slice(
            &fs::read(args.archive_dir.join("index.json")).expect("read index"),
        )
        .expect("parse index");
        assert_eq!(index["format_version"], 10);
        assert_eq!(
            index["comparison_failure_protocol"],
            BATCH_COMPARISON_FAILURE_PROTOCOL
        );
        assert_eq!(
            index["files"],
            serde_json::json!([
                "trace.json",
                "batch.json",
                "environment.json",
                "comparison-failures.json"
            ])
        );
    }

    #[test]
    fn publication_admission_rejects_debug_cold_and_all_excluded_batches() {
        let directory = tempdir().expect("temporary directory");
        let trace_path = write_trace(&directory);
        let mut args = publication_args(
            &directory,
            trace_path,
            directory.path().join("engines"),
            directory.path().join("archive"),
        );
        let exclusions = BTreeMap::new();

        let debug_error = validate_publication_admission(
            &args,
            &exclusions,
            "debug",
            Some("x86_64-unknown-linux-gnu".to_owned()),
        )
        .expect_err("debug build must not be admitted");
        assert!(debug_error.to_string().contains("release build"));

        args.cache_state = CacheStateKind::ColdBestEffort;
        let cold_error = validate_publication_admission(
            &args,
            &exclusions,
            "release",
            Some("x86_64-unknown-linux-gnu".to_owned()),
        )
        .expect_err("best-effort cold state must not be admitted");
        assert!(cold_error
            .to_string()
            .contains("requires --cache-state warm"));

        args.cache_state = CacheStateKind::Warm;
        let all_excluded = BTreeMap::from([
            (0_u32, "excluded 0".to_owned()),
            (1_u32, "excluded 1".to_owned()),
            (2_u32, "excluded 2".to_owned()),
        ]);
        let excluded_error = validate_publication_admission(
            &args,
            &all_excluded,
            "release",
            Some("x86_64-unknown-linux-gnu".to_owned()),
        )
        .expect_err("all-excluded publication batch must not be admitted");
        assert!(excluded_error
            .to_string()
            .contains("at least one included pair"));
    }

    #[test]
    fn publication_admission_freezes_batch_protocol_metadata() {
        let directory = tempdir().expect("temporary directory");
        let trace_path = write_trace(&directory);
        let args = publication_args(
            &directory,
            trace_path,
            directory.path().join("engines"),
            directory.path().join("archive"),
        );
        let admission = validate_publication_admission(
            &args,
            &BTreeMap::new(),
            "release",
            Some("x86_64-unknown-linux-gnu".to_owned()),
        )
        .expect("validate publication admission")
        .expect("publication record");

        assert_eq!(admission.admission_protocol, PUBLICATION_ADMISSION_PROTOCOL);
        assert_eq!(admission.cache_policy, PUBLICATION_CACHE_POLICY);
        assert_eq!(admission.cache_state, "warm");
        assert_eq!(admission.durability_mode, PUBLICATION_DURABILITY_MODE);
        assert_eq!(admission.pair_order_policy, PUBLICATION_PAIR_ORDER_POLICY);
        assert_eq!(admission.requested_pairs, 3);
        assert_eq!(admission.ordered_comparisons_per_included_pair, 2);
        assert_eq!(admission.filesystem, "ext4");
        assert_eq!(admission.mount_options, "rw,noatime");
        assert_eq!(admission.analysis_script_version, "analysis@abc123");
    }

    #[test]
    fn exploratory_mode_rejects_publication_only_metadata() {
        let directory = tempdir().expect("temporary directory");
        let trace_path = write_trace(&directory);
        let mut args = base_args(
            &directory,
            trace_path,
            directory.path().join("engines"),
            directory.path().join("archive"),
        );
        args.host_cpu = Some("cpu".to_owned());
        let error = validate_publication_admission(&args, &BTreeMap::new(), "release", None)
            .expect_err("publication metadata without admission must fail");
        assert!(error
            .to_string()
            .contains("requires --admission publication-warm-v1"));
    }

    fn write_trace(directory: &tempfile::TempDir) -> PathBuf {
        let trace_path = directory.path().join("trace.json");
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 0x2026_0829,
            profile: ExperimentProfile::RandomWrite,
            operations: 2,
            key_space: 8,
            value_bytes: 4,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");
        fs::write(
            &trace_path,
            serde_json::to_vec_pretty(&trace).expect("encode trace"),
        )
        .expect("write trace");
        trace_path
    }

    fn base_args(
        directory: &tempfile::TempDir,
        trace: PathBuf,
        engine_root: PathBuf,
        archive_dir: PathBuf,
    ) -> Cli {
        let _ = directory;
        Cli {
            trace,
            engine_root,
            archive_dir,
            pair_seed: 0,
            pairs: 3,
            btree_cache_pages: 8,
            revision: "abc123".to_owned(),
            exclude_pairs: vec!["1=scheduled thermal cooldown".to_owned()],
            host_label: Some("test-host".to_owned()),
            host_cpu: None,
            host_memory: None,
            filesystem: Some("test-fs".to_owned()),
            mount_options: None,
            storage_device: Some("test-device".to_owned()),
            cache_state: CacheStateKind::Warm,
            admission: AdmissionKind::Exploratory,
            optimization_flags: None,
            analysis_script_version: None,
            noise_budget: None,
            notes: Some("integration test".to_owned()),
        }
    }

    fn publication_args(
        directory: &tempfile::TempDir,
        trace: PathBuf,
        engine_root: PathBuf,
        archive_dir: PathBuf,
    ) -> Cli {
        let mut args = base_args(directory, trace, engine_root, archive_dir);
        args.admission = AdmissionKind::PublicationWarmV1;
        args.exclude_pairs.clear();
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
