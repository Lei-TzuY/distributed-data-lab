use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{self, BufReader, BufWriter, Read, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::Parser;
use db_cli::batch_archive::{verify_batch_archive, VerificationSummary, VerifyError};
use serde::Serialize;
use serde_json::{Map, Value};
use tempfile::TempDir;
use thiserror::Error;

const MAX_ANALYSIS_JSON_BYTES: u64 = 64 * 1024 * 1024;
const ANALYSIS_PROTOCOL: &str = "verified_operational_timing_descriptive_v1";
const SNAPSHOT_PROTOCOL: &str = "copy_verify_compare_v1";
const ESTIMATOR: &str = "empirical_nearest_rank_p50_p95_v1";
const INTERPRETATION_BOUNDARY: &str =
    "descriptive_only; performance claims require externally controlled pinned-host review";

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-batch-analyze",
    version,
    about = "Describe operational timing distributions only after fail-closed archive verification"
)]
struct Cli {
    /// Existing immutable repeated-batch archive directory.
    #[arg(long)]
    archive_dir: PathBuf,
    /// Optional exact repository revision expected by the caller.
    #[arg(long)]
    expected_revision: Option<String>,
    /// Reject exploratory evidence before analysis.
    #[arg(long)]
    require_publication: bool,
}

#[derive(Debug, Serialize)]
struct AnalysisReport {
    analysis_protocol: &'static str,
    snapshot_protocol: &'static str,
    estimator: &'static str,
    interpretation_boundary: &'static str,
    verification: VerificationSummary,
    primary_complete_pairs: SuccessSection,
    retained_failed_pair_evidence: RetainedFailedPairEvidence,
}

#[derive(Debug, Serialize)]
struct RetainedFailedPairEvidence {
    completed_repetitions: SuccessSection,
    failing_repetition_prefix: SuccessSection,
    failed_operations: FailureSection,
}

#[derive(Debug, Serialize)]
struct SuccessSection {
    combined: ComparisonSuccessSummary,
    by_execution_order: OrderedSuccessSummary,
}

#[derive(Debug, Serialize)]
struct OrderedSuccessSummary {
    left_then_right: ComparisonSuccessSummary,
    right_then_left: ComparisonSuccessSummary,
}

#[derive(Debug, Serialize)]
struct ComparisonSuccessSummary {
    left: EngineSuccessSummary,
    right: EngineSuccessSummary,
}

#[derive(Debug, Serialize)]
struct EngineSuccessSummary {
    reopen: SuccessfulOperationSummary,
    compaction_stall: SuccessfulOperationSummary,
}

#[derive(Debug, Serialize)]
struct SuccessfulOperationSummary {
    duration_ns: DistributionSummary,
    work_units: BTreeMap<String, u64>,
    measured_step_index_missing: u64,
}

#[derive(Debug, Serialize)]
struct FailureSection {
    combined: ComparisonFailureSummary,
    by_execution_order: OrderedFailureSummary,
}

#[derive(Debug, Serialize)]
struct OrderedFailureSummary {
    left_then_right: ComparisonFailureSummary,
    right_then_left: ComparisonFailureSummary,
}

#[derive(Debug, Serialize)]
struct ComparisonFailureSummary {
    left: EngineFailureSummary,
    right: EngineFailureSummary,
}

#[derive(Debug, Serialize)]
struct EngineFailureSummary {
    reopen: FailedOperationSummary,
    compaction_stall: FailedOperationSummary,
}

#[derive(Debug, Serialize)]
struct FailedOperationSummary {
    duration_ns: DistributionSummary,
    error_classes: BTreeMap<String, u64>,
    work_units: BTreeMap<String, u64>,
    work_missing: u64,
    measured_step_index_missing: u64,
}

#[derive(Debug, Serialize, PartialEq, Eq)]
struct DistributionSummary {
    samples: usize,
    min_ns: Option<u64>,
    nearest_rank_p50_ns: Option<u64>,
    nearest_rank_p95_ns: Option<u64>,
    max_ns: Option<u64>,
}

#[derive(Debug, Error)]
enum AnalyzeError {
    #[error(transparent)]
    Verify(#[from] VerifyError),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("invalid JSON at {path}: {source}")]
    Json {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("verified archive cannot be analyzed: {0}")]
    Invalid(String),
}

struct ArchiveSnapshot {
    directory: TempDir,
}

impl ArchiveSnapshot {
    fn path(&self) -> &Path {
        self.directory.path()
    }
}

fn main() -> ExitCode {
    let args = Cli::parse();
    match analyze(&args) {
        Ok(report) => match serde_json::to_string_pretty(&report) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode analysis report: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}

fn analyze(args: &Cli) -> Result<AnalysisReport, AnalyzeError> {
    require_regular_directory(&args.archive_dir, "source archive")?;
    let source_verification = verify_batch_archive(
        &args.archive_dir,
        args.expected_revision.as_deref(),
        args.require_publication,
    )?;

    let snapshot = snapshot_archive(&args.archive_dir)?;
    let snapshot_dir = snapshot.path();
    let verification = verify_batch_archive(
        snapshot_dir,
        args.expected_revision.as_deref(),
        args.require_publication,
    )?;
    if verification != source_verification {
        return Err(AnalyzeError::Invalid(
            "source archive changed while the verified analysis snapshot was being created"
                .to_owned(),
        ));
    }

    let batch = read_json(snapshot_dir, "batch.json")?;
    let sidecars = if matches!(verification.format_version, 10 | 11) {
        Some(read_json(snapshot_dir, "comparison-failures.json")?)
    } else {
        None
    };

    let mut primary = SuccessCollector::default();
    collect_complete_pairs(&batch, &mut primary)?;

    let mut completed_repetitions = SuccessCollector::default();
    let mut failing_prefix = SuccessCollector::default();
    let mut failures = FailureCollector::default();
    if let Some(sidecars) = sidecars.as_ref() {
        collect_failed_pair_evidence(
            sidecars,
            &mut completed_repetitions,
            &mut failing_prefix,
            &mut failures,
        )?;
    }

    let verification_after = verify_batch_archive(
        snapshot_dir,
        args.expected_revision.as_deref(),
        args.require_publication,
    )?;
    if verification_after != verification {
        return Err(AnalyzeError::Invalid(
            "verified analysis snapshot changed while it was being read".to_owned(),
        ));
    }
    let source_verification_after = verify_batch_archive(
        &args.archive_dir,
        args.expected_revision.as_deref(),
        args.require_publication,
    )?;
    if source_verification_after != source_verification {
        return Err(AnalyzeError::Invalid(
            "source archive verification summary changed during analysis".to_owned(),
        ));
    }
    verify_source_matches_snapshot(&args.archive_dir, snapshot_dir)?;

    Ok(AnalysisReport {
        analysis_protocol: ANALYSIS_PROTOCOL,
        snapshot_protocol: SNAPSHOT_PROTOCOL,
        estimator: ESTIMATOR,
        interpretation_boundary: INTERPRETATION_BOUNDARY,
        verification,
        primary_complete_pairs: primary.summarize(),
        retained_failed_pair_evidence: RetainedFailedPairEvidence {
            completed_repetitions: completed_repetitions.summarize(),
            failing_repetition_prefix: failing_prefix.summarize(),
            failed_operations: failures.summarize(),
        },
    })
}

#[derive(Default)]
struct SuccessCollector {
    combined: ComparisonSuccessCollector,
    left_then_right: ComparisonSuccessCollector,
    right_then_left: ComparisonSuccessCollector,
}

impl SuccessCollector {
    fn add_ordered_report(&mut self, report: &Value, label: &str) -> Result<(), AnalyzeError> {
        let order = required_str(report, "execution_order", label)?;
        let comparison = required_field(report, "comparison", label)?;
        let left = required_field(comparison, "left", "ordered comparison")?;
        let right = required_field(comparison, "right", "ordered comparison")?;
        let left_timing = required_field(left, "operational_timing", "left engine evidence")?;
        let right_timing = required_field(right, "operational_timing", "right engine evidence")?;

        self.combined.add(left_timing, right_timing)?;
        match order {
            "left_then_right" => self.left_then_right.add(left_timing, right_timing)?,
            "right_then_left" => self.right_then_left.add(left_timing, right_timing)?,
            other => {
                return Err(AnalyzeError::Invalid(format!(
                    "{label} has unknown execution_order {other:?}"
                )))
            }
        }
        Ok(())
    }

    fn add_failure_prefix(
        &mut self,
        ordered_failure: &Value,
        label: &str,
    ) -> Result<(), AnalyzeError> {
        let order = required_str(ordered_failure, "execution_order", label)?;
        let left = required_field(
            ordered_failure,
            "left_operational_timing",
            "ordered failure",
        )?;
        let right = required_field(
            ordered_failure,
            "right_operational_timing",
            "ordered failure",
        )?;
        self.combined.add(left, right)?;
        match order {
            "left_then_right" => self.left_then_right.add(left, right)?,
            "right_then_left" => self.right_then_left.add(left, right)?,
            other => {
                return Err(AnalyzeError::Invalid(format!(
                    "{label} has unknown execution_order {other:?}"
                )))
            }
        }
        Ok(())
    }

    fn summarize(mut self) -> SuccessSection {
        SuccessSection {
            combined: self.combined.summarize(),
            by_execution_order: OrderedSuccessSummary {
                left_then_right: self.left_then_right.summarize(),
                right_then_left: self.right_then_left.summarize(),
            },
        }
    }
}

#[derive(Default)]
struct ComparisonSuccessCollector {
    left: EngineSuccessCollector,
    right: EngineSuccessCollector,
}

impl ComparisonSuccessCollector {
    fn add(&mut self, left: &Value, right: &Value) -> Result<(), AnalyzeError> {
        self.left.add(left)?;
        self.right.add(right)?;
        Ok(())
    }

    fn summarize(&mut self) -> ComparisonSuccessSummary {
        ComparisonSuccessSummary {
            left: self.left.summarize(),
            right: self.right.summarize(),
        }
    }
}

#[derive(Default)]
struct EngineSuccessCollector {
    reopen: SuccessfulOperationCollector,
    compaction_stall: SuccessfulOperationCollector,
}

impl EngineSuccessCollector {
    fn add(&mut self, timing: &Value) -> Result<(), AnalyzeError> {
        collect_success_operation(timing, "reopen", &mut self.reopen)?;
        collect_success_operation(timing, "compaction_stall", &mut self.compaction_stall)?;
        Ok(())
    }

    fn summarize(&mut self) -> EngineSuccessSummary {
        EngineSuccessSummary {
            reopen: self.reopen.summarize(),
            compaction_stall: self.compaction_stall.summarize(),
        }
    }
}

#[derive(Default)]
struct SuccessfulOperationCollector {
    durations: Vec<u64>,
    work_units: BTreeMap<String, u64>,
    measured_step_index_missing: u64,
}

impl SuccessfulOperationCollector {
    fn summarize(&mut self) -> SuccessfulOperationSummary {
        SuccessfulOperationSummary {
            duration_ns: summarize_distribution(&mut self.durations),
            work_units: std::mem::take(&mut self.work_units),
            measured_step_index_missing: self.measured_step_index_missing,
        }
    }
}

#[derive(Default)]
struct FailureCollector {
    combined: ComparisonFailureCollector,
    left_then_right: ComparisonFailureCollector,
    right_then_left: ComparisonFailureCollector,
}

impl FailureCollector {
    fn add_ordered_failure(&mut self, ordered: &Value) -> Result<(), AnalyzeError> {
        let order = required_str(ordered, "execution_order", "ordered failure")?;
        let left = required_field(ordered, "left_operational_timing", "ordered failure")?;
        let right = required_field(ordered, "right_operational_timing", "ordered failure")?;
        self.combined.add(left, right)?;
        match order {
            "left_then_right" => self.left_then_right.add(left, right)?,
            "right_then_left" => self.right_then_left.add(left, right)?,
            other => {
                return Err(AnalyzeError::Invalid(format!(
                    "ordered failure has unknown execution_order {other:?}"
                )))
            }
        }
        Ok(())
    }

    fn summarize(mut self) -> FailureSection {
        FailureSection {
            combined: self.combined.summarize(),
            by_execution_order: OrderedFailureSummary {
                left_then_right: self.left_then_right.summarize(),
                right_then_left: self.right_then_left.summarize(),
            },
        }
    }
}

#[derive(Default)]
struct ComparisonFailureCollector {
    left: EngineFailureCollector,
    right: EngineFailureCollector,
}

impl ComparisonFailureCollector {
    fn add(&mut self, left: &Value, right: &Value) -> Result<(), AnalyzeError> {
        self.left.add(left)?;
        self.right.add(right)?;
        Ok(())
    }

    fn summarize(&mut self) -> ComparisonFailureSummary {
        ComparisonFailureSummary {
            left: self.left.summarize(),
            right: self.right.summarize(),
        }
    }
}

#[derive(Default)]
struct EngineFailureCollector {
    reopen: FailedOperationCollector,
    compaction_stall: FailedOperationCollector,
}

impl EngineFailureCollector {
    fn add(&mut self, timing: &Value) -> Result<(), AnalyzeError> {
        collect_failed_operation(timing, "reopen", &mut self.reopen)?;
        collect_failed_operation(timing, "compaction_stall", &mut self.compaction_stall)?;
        Ok(())
    }

    fn summarize(&mut self) -> EngineFailureSummary {
        EngineFailureSummary {
            reopen: self.reopen.summarize(),
            compaction_stall: self.compaction_stall.summarize(),
        }
    }
}

#[derive(Default)]
struct FailedOperationCollector {
    durations: Vec<u64>,
    error_classes: BTreeMap<String, u64>,
    work_units: BTreeMap<String, u64>,
    work_missing: u64,
    measured_step_index_missing: u64,
}

impl FailedOperationCollector {
    fn summarize(&mut self) -> FailedOperationSummary {
        FailedOperationSummary {
            duration_ns: summarize_distribution(&mut self.durations),
            error_classes: std::mem::take(&mut self.error_classes),
            work_units: std::mem::take(&mut self.work_units),
            work_missing: self.work_missing,
            measured_step_index_missing: self.measured_step_index_missing,
        }
    }
}

fn collect_complete_pairs(
    batch: &Value,
    collector: &mut SuccessCollector,
) -> Result<(), AnalyzeError> {
    let attempts = required_array(batch, "attempts", "batch.json")?;
    for attempt in attempts {
        if required_str(attempt, "disposition", "batch attempt")? != "included" {
            continue;
        }
        let report = required_field(attempt, "report", "included batch attempt")?;
        collector.add_ordered_report(
            required_field(report, "first", "counterbalanced report")?,
            "counterbalanced first report",
        )?;
        collector.add_ordered_report(
            required_field(report, "second", "counterbalanced report")?,
            "counterbalanced second report",
        )?;
    }
    Ok(())
}

fn collect_failed_pair_evidence(
    sidecars: &Value,
    completed_repetitions: &mut SuccessCollector,
    failing_prefix: &mut SuccessCollector,
    failures: &mut FailureCollector,
) -> Result<(), AnalyzeError> {
    let sidecars = sidecars.as_array().ok_or_else(|| {
        AnalyzeError::Invalid("comparison-failures.json must be an array".to_owned())
    })?;
    for sidecar in sidecars {
        let failure = required_field(sidecar, "failure", "comparison sidecar")?;
        if let Some(completed) = optional_field(failure, "completed_first", "comparison failure")? {
            if !completed.is_null() {
                completed_repetitions
                    .add_ordered_report(completed, "completed first repetition")?;
            }
        }
        let ordered = required_field(failure, "ordered_failure", "comparison failure")?;
        failing_prefix.add_failure_prefix(ordered, "ordered failure")?;
        failures.add_ordered_failure(ordered)?;
    }
    Ok(())
}

fn collect_success_operation(
    timing: &Value,
    prefix: &str,
    collector: &mut SuccessfulOperationCollector,
) -> Result<(), AnalyzeError> {
    let legacy_name = format!("{prefix}_ns");
    let samples_name = format!("{prefix}_samples");
    let legacy = required_array(timing, &legacy_name, "operational timing")?;
    let samples = required_array(timing, &samples_name, "operational timing")?;
    if legacy.len() != samples.len() {
        return Err(AnalyzeError::Invalid(format!(
            "operational timing {legacy_name} has {} entries but {samples_name} has {}",
            legacy.len(),
            samples.len()
        )));
    }
    for (index, (legacy_duration, sample)) in legacy.iter().zip(samples).enumerate() {
        let legacy_duration = legacy_duration.as_u64().ok_or_else(|| {
            AnalyzeError::Invalid(format!(
                "operational timing {legacy_name}[{index}] must be an unsigned integer"
            ))
        })?;
        let duration = required_u64(sample, "duration_ns", "successful timing sample")?;
        if duration != legacy_duration {
            return Err(AnalyzeError::Invalid(format!(
                "operational timing {legacy_name}[{index}] differs from {samples_name}[{index}].duration_ns"
            )));
        }
        if required_field(sample, "measured_step_index", "successful timing sample")?.is_null() {
            collector.measured_step_index_missing =
                collector.measured_step_index_missing.saturating_add(1);
        }
        let work = required_field(sample, "work", "successful timing sample")?;
        collect_work_unit(work, &mut collector.work_units)?;
        collector.durations.push(duration);
    }
    Ok(())
}

fn collect_failed_operation(
    timing: &Value,
    prefix: &str,
    collector: &mut FailedOperationCollector,
) -> Result<(), AnalyzeError> {
    let field = format!("{prefix}_failure_samples");
    let Some(samples_value) = optional_field(timing, &field, "operational timing")? else {
        return Ok(());
    };
    let samples = samples_value.as_array().ok_or_else(|| {
        AnalyzeError::Invalid(format!("operational timing {field} must be an array"))
    })?;
    for sample in samples {
        collector
            .durations
            .push(required_u64(sample, "duration_ns", "failed timing sample")?);
        let class = required_str(sample, "error_class", "failed timing sample")?;
        *collector.error_classes.entry(class.to_owned()).or_default() += 1;
        if required_field(sample, "measured_step_index", "failed timing sample")?.is_null() {
            collector.measured_step_index_missing =
                collector.measured_step_index_missing.saturating_add(1);
        }
        let work = required_field(sample, "work", "failed timing sample")?;
        if work.is_null() {
            collector.work_missing = collector.work_missing.saturating_add(1);
        } else {
            collect_work_unit(work, &mut collector.work_units)?;
        }
    }
    Ok(())
}

fn collect_work_unit(
    work: &Value,
    work_units: &mut BTreeMap<String, u64>,
) -> Result<(), AnalyzeError> {
    let unit = required_str(work, "unit", "operational work")?;
    required_u64(work, "units_examined", "operational work")?;
    required_u64(work, "bytes_examined", "operational work")?;
    *work_units.entry(unit.to_owned()).or_default() += 1;
    Ok(())
}

fn summarize_distribution(values: &mut [u64]) -> DistributionSummary {
    if values.is_empty() {
        return DistributionSummary {
            samples: 0,
            min_ns: None,
            nearest_rank_p50_ns: None,
            nearest_rank_p95_ns: None,
            max_ns: None,
        };
    }
    values.sort_unstable();
    let samples = values.len();
    DistributionSummary {
        samples,
        min_ns: values.first().copied(),
        nearest_rank_p50_ns: Some(values[nearest_rank_index(samples, 50)]),
        nearest_rank_p95_ns: Some(values[nearest_rank_index(samples, 95)]),
        max_ns: values.last().copied(),
    }
}

fn nearest_rank_index(samples: usize, percentile: u8) -> usize {
    let rank = (samples as u128 * u128::from(percentile)).div_ceil(100);
    usize::try_from(rank.saturating_sub(1)).unwrap_or(samples - 1)
}

fn snapshot_archive(source_dir: &Path) -> Result<ArchiveSnapshot, AnalyzeError> {
    require_regular_directory(source_dir, "source archive")?;
    let directory = tempfile::tempdir().map_err(|source| AnalyzeError::Io {
        path: std::env::temp_dir(),
        source,
    })?;
    let entries = directory_entry_names(source_dir, "source archive")?;
    for name in entries {
        let source_path = source_dir.join(&name);
        let target_path = directory.path().join(&name);
        copy_bounded_regular_file(&source_path, &target_path)?;
    }
    Ok(ArchiveSnapshot { directory })
}

fn verify_source_matches_snapshot(
    source_dir: &Path,
    snapshot_dir: &Path,
) -> Result<(), AnalyzeError> {
    require_regular_directory(source_dir, "source archive")?;
    require_regular_directory(snapshot_dir, "analysis snapshot")?;
    let source_entries = directory_entry_names(source_dir, "source archive")?;
    let snapshot_entries = directory_entry_names(snapshot_dir, "analysis snapshot")?;
    if source_entries != snapshot_entries {
        return Err(AnalyzeError::Invalid(format!(
            "source archive directory entries changed during analysis: source {source_entries:?}, snapshot {snapshot_entries:?}"
        )));
    }
    for name in source_entries {
        compare_regular_files(&source_dir.join(&name), &snapshot_dir.join(&name), &name)?;
    }
    Ok(())
}

fn require_regular_directory(path: &Path, label: &str) -> Result<(), AnalyzeError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| AnalyzeError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if !metadata.file_type().is_dir() {
        return Err(AnalyzeError::Invalid(format!(
            "{label} must be a real directory rather than a symlink or non-directory"
        )));
    }
    Ok(())
}

fn directory_entry_names(path: &Path, label: &str) -> Result<BTreeSet<String>, AnalyzeError> {
    let read_dir = fs::read_dir(path).map_err(|source| AnalyzeError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    let mut names = BTreeSet::new();
    for entry in read_dir {
        let entry = entry.map_err(|source| AnalyzeError::Io {
            path: path.to_path_buf(),
            source,
        })?;
        let name = entry.file_name().into_string().map_err(|_| {
            AnalyzeError::Invalid(format!("{label} contains a non-UTF-8 entry name"))
        })?;
        names.insert(name);
    }
    Ok(names)
}

fn copy_bounded_regular_file(source_path: &Path, target_path: &Path) -> Result<(), AnalyzeError> {
    let source_file = File::open(source_path).map_err(|source| AnalyzeError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    let metadata = source_file.metadata().map_err(|source| AnalyzeError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    if !metadata.is_file() {
        return Err(AnalyzeError::Invalid(format!(
            "source archive entry {} is not a regular file",
            source_path.display()
        )));
    }
    if metadata.len() > MAX_ANALYSIS_JSON_BYTES {
        return Err(AnalyzeError::Invalid(format!(
            "source archive entry {} has {} bytes; maximum snapshot file size is {MAX_ANALYSIS_JSON_BYTES}",
            source_path.display(),
            metadata.len()
        )));
    }

    let target_file = File::create(target_path).map_err(|source| AnalyzeError::Io {
        path: target_path.to_path_buf(),
        source,
    })?;
    let mut reader = BufReader::new(source_file);
    let mut writer = BufWriter::new(target_file);
    io::copy(&mut reader, &mut writer).map_err(|source| AnalyzeError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    writer.flush().map_err(|source| AnalyzeError::Io {
        path: target_path.to_path_buf(),
        source,
    })?;
    writer
        .get_ref()
        .sync_all()
        .map_err(|source| AnalyzeError::Io {
            path: target_path.to_path_buf(),
            source,
        })?;
    Ok(())
}

fn compare_regular_files(
    source_path: &Path,
    snapshot_path: &Path,
    name: &str,
) -> Result<(), AnalyzeError> {
    let source_file = File::open(source_path).map_err(|source| AnalyzeError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    let snapshot_file = File::open(snapshot_path).map_err(|source| AnalyzeError::Io {
        path: snapshot_path.to_path_buf(),
        source,
    })?;
    let source_metadata = source_file.metadata().map_err(|source| AnalyzeError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    let snapshot_metadata = snapshot_file
        .metadata()
        .map_err(|source| AnalyzeError::Io {
            path: snapshot_path.to_path_buf(),
            source,
        })?;
    if !source_metadata.is_file() || !snapshot_metadata.is_file() {
        return Err(AnalyzeError::Invalid(format!(
            "archive entry {name:?} ceased to be a regular file during analysis"
        )));
    }
    if source_metadata.len() != snapshot_metadata.len() {
        return Err(AnalyzeError::Invalid(format!(
            "source archive entry {name:?} changed size during analysis"
        )));
    }

    let mut source_reader = BufReader::new(source_file);
    let mut snapshot_reader = BufReader::new(snapshot_file);
    let mut source_buffer = [0_u8; 64 * 1024];
    let mut snapshot_buffer = [0_u8; 64 * 1024];
    loop {
        let source_read =
            source_reader
                .read(&mut source_buffer)
                .map_err(|source| AnalyzeError::Io {
                    path: source_path.to_path_buf(),
                    source,
                })?;
        let snapshot_read = snapshot_reader
            .read(&mut snapshot_buffer)
            .map_err(|source| AnalyzeError::Io {
                path: snapshot_path.to_path_buf(),
                source,
            })?;
        if source_read != snapshot_read
            || source_buffer[..source_read] != snapshot_buffer[..snapshot_read]
        {
            return Err(AnalyzeError::Invalid(format!(
                "source archive entry {name:?} changed content during analysis"
            )));
        }
        if source_read == 0 {
            break;
        }
    }
    Ok(())
}

fn read_json(archive_dir: &Path, name: &str) -> Result<Value, AnalyzeError> {
    let path = archive_dir.join(name);
    let metadata = fs::symlink_metadata(&path).map_err(|source| AnalyzeError::Io {
        path: path.clone(),
        source,
    })?;
    if !metadata.file_type().is_file() {
        return Err(AnalyzeError::Invalid(format!(
            "analysis input {name} is not a regular file"
        )));
    }
    if metadata.len() > MAX_ANALYSIS_JSON_BYTES {
        return Err(AnalyzeError::Invalid(format!(
            "analysis input {name} has {} bytes; maximum is {MAX_ANALYSIS_JSON_BYTES}",
            metadata.len()
        )));
    }
    let encoded = fs::read(&path).map_err(|source| AnalyzeError::Io {
        path: path.clone(),
        source,
    })?;
    serde_json::from_slice(&encoded).map_err(|source| AnalyzeError::Json { path, source })
}

fn required_object<'a>(
    value: &'a Value,
    label: &str,
) -> Result<&'a Map<String, Value>, AnalyzeError> {
    value
        .as_object()
        .ok_or_else(|| AnalyzeError::Invalid(format!("{label} must be a JSON object")))
}

fn required_field<'a>(
    value: &'a Value,
    field: &str,
    label: &str,
) -> Result<&'a Value, AnalyzeError> {
    required_object(value, label)?.get(field).ok_or_else(|| {
        AnalyzeError::Invalid(format!("{label} is missing required field {field:?}"))
    })
}

fn optional_field<'a>(
    value: &'a Value,
    field: &str,
    label: &str,
) -> Result<Option<&'a Value>, AnalyzeError> {
    Ok(required_object(value, label)?.get(field))
}

fn required_array<'a>(
    value: &'a Value,
    field: &str,
    label: &str,
) -> Result<&'a Vec<Value>, AnalyzeError> {
    required_field(value, field, label)?
        .as_array()
        .ok_or_else(|| AnalyzeError::Invalid(format!("{label} {field} must be an array")))
}

fn required_str<'a>(value: &'a Value, field: &str, label: &str) -> Result<&'a str, AnalyzeError> {
    required_field(value, field, label)?
        .as_str()
        .ok_or_else(|| AnalyzeError::Invalid(format!("{label} {field} must be a string")))
}

fn required_u64(value: &Value, field: &str, label: &str) -> Result<u64, AnalyzeError> {
    required_field(value, field, label)?
        .as_u64()
        .ok_or_else(|| {
            AnalyzeError::Invalid(format!("{label} {field} must be an unsigned integer"))
        })
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::Path;

    use db_core::{generate_experiment_trace, ExperimentGeneratorConfig, ExperimentProfile};
    use serde_json::{json, Value};
    use tempfile::tempdir;

    use super::{
        analyze, snapshot_archive, summarize_distribution, verify_source_matches_snapshot, Cli,
        DistributionSummary, SNAPSHOT_PROTOCOL,
    };

    const FAILURE_PROTOCOL_V2: &str = "ordered_comparison_failure_sidecar_v2";

    #[test]
    fn nearest_rank_distribution_is_deterministic() {
        let mut values = vec![40, 10, 30, 20];
        assert_eq!(
            summarize_distribution(&mut values),
            DistributionSummary {
                samples: 4,
                min_ns: Some(10),
                nearest_rank_p50_ns: Some(20),
                nearest_rank_p95_ns: Some(40),
                max_ns: Some(40),
            }
        );
    }

    #[test]
    fn complete_pair_timings_are_primary_and_order_stratified() {
        let directory = tempdir().expect("temporary directory");
        write_complete_v6(directory.path());
        let report = analyze(&args(directory.path())).expect("analyze complete v6");
        assert_eq!(report.snapshot_protocol, SNAPSHOT_PROTOCOL);
        assert_eq!(report.verification.included_pairs, 1);
        assert_eq!(
            report
                .primary_complete_pairs
                .combined
                .left
                .reopen
                .duration_ns
                .samples,
            2
        );
        assert_eq!(
            report
                .primary_complete_pairs
                .by_execution_order
                .left_then_right
                .left
                .reopen
                .duration_ns
                .nearest_rank_p50_ns,
            Some(10)
        );
        assert_eq!(
            report
                .primary_complete_pairs
                .by_execution_order
                .right_then_left
                .left
                .reopen
                .duration_ns
                .nearest_rank_p50_ns,
            Some(30)
        );
    }

    #[test]
    fn failed_pair_evidence_stays_out_of_primary_success_distribution() {
        let directory = tempdir().expect("temporary directory");
        write_failed_v10(directory.path());
        let report = analyze(&args(directory.path())).expect("analyze failed v10");
        assert_eq!(report.verification.failed_pairs, 1);
        assert_eq!(
            report
                .primary_complete_pairs
                .combined
                .left
                .reopen
                .duration_ns
                .samples,
            0
        );
        assert_eq!(
            report
                .retained_failed_pair_evidence
                .completed_repetitions
                .combined
                .left
                .reopen
                .duration_ns
                .samples,
            1
        );
        assert_eq!(
            report
                .retained_failed_pair_evidence
                .failing_repetition_prefix
                .combined
                .right
                .compaction_stall
                .duration_ns
                .samples,
            1
        );
        assert_eq!(
            report
                .retained_failed_pair_evidence
                .failed_operations
                .combined
                .right
                .compaction_stall
                .duration_ns
                .nearest_rank_p50_ns,
            Some(99)
        );
    }

    #[test]
    fn source_change_after_snapshot_is_detected_byte_for_byte() {
        let directory = tempdir().expect("temporary directory");
        write_complete_v6(directory.path());
        let snapshot = snapshot_archive(directory.path()).expect("snapshot archive");
        verify_source_matches_snapshot(directory.path(), snapshot.path())
            .expect("unchanged source must match snapshot");

        let batch_path = directory.path().join("batch.json");
        let mut batch: Value = serde_json::from_slice(&fs::read(&batch_path).expect("read batch"))
            .expect("parse batch");
        batch["attempts"][0]["report"]["first"]["comparison"]["left"]["operational_timing"]
            ["reopen_ns"][0] = json!(999);
        batch["attempts"][0]["report"]["first"]["comparison"]["left"]["operational_timing"]
            ["reopen_samples"][0]["duration_ns"] = json!(999);
        fs::write(
            &batch_path,
            serde_json::to_vec_pretty(&batch).expect("encode changed batch"),
        )
        .expect("write changed batch");

        assert!(
            verify_source_matches_snapshot(directory.path(), snapshot.path())
                .expect_err("timing-only mutation must be detected")
                .to_string()
                .contains("changed")
        );
    }

    fn args(path: &Path) -> Cli {
        Cli {
            archive_dir: path.to_path_buf(),
            expected_revision: Some("abc123".to_owned()),
            require_publication: false,
        }
    }

    fn write_complete_v6(path: &Path) {
        let trace = trace_value();
        let first = ordered_report("left_then_right", 10, 20);
        let second = ordered_report("right_then_left", 30, 40);
        let batch = json!({
            "trace": trace,
            "pair_seed": 0,
            "requested_pairs": 1,
            "included_pairs": 1,
            "failed_pairs": 0,
            "excluded_pairs": 0,
            "attempts": [{
                "context": {"pair_index": 0, "pair_order": "left_then_right_first"},
                "disposition": "included",
                "report": {
                    "pair_order": "left_then_right_first",
                    "first": first,
                    "second": second
                },
                "failure": null,
                "exclusion_reason": null
            }]
        });
        write_base_archive(path, 6, &batch, None);
    }

    fn write_failed_v10(path: &Path) {
        let trace = trace_value();
        let batch = json!({
            "trace": trace,
            "pair_seed": 0,
            "requested_pairs": 1,
            "included_pairs": 0,
            "failed_pairs": 1,
            "excluded_pairs": 0,
            "attempts": [{
                "context": {"pair_index": 0, "pair_order": "left_then_right_first"},
                "disposition": "failed",
                "report": null,
                "failure": {
                    "stage": "comparison",
                    "engine_role": null,
                    "repetition_index": null,
                    "class": "io",
                    "message": "synthetic failure"
                },
                "exclusion_reason": null
            }]
        });
        let mut right_failure_timing = timing(0, 50);
        right_failure_timing["compaction_stall_failure_samples"] = json!([{
            "measured_step_index": 0,
            "duration_ns": 99,
            "work": null,
            "error_class": "io"
        }]);
        let sidecars = json!([{
            "context": {"pair_index": 0, "pair_order": "left_then_right_first"},
            "failure": {
                "pair_order": "left_then_right_first",
                "repetition_index": 1,
                "completed_first": ordered_report("left_then_right", 11, 22),
                "ordered_failure": {
                    "execution_order": "right_then_left",
                    "error_class": "io",
                    "message": "synthetic failure",
                    "left_operational_timing": timing(33, 0),
                    "right_operational_timing": right_failure_timing
                }
            }
        }]);
        write_base_archive(path, 10, &batch, Some(&sidecars));
    }

    fn ordered_report(order: &str, left_reopen: u64, right_reopen: u64) -> Value {
        json!({
            "execution_order": order,
            "comparison": {
                "left": {"operational_timing": timing(left_reopen, 0)},
                "right": {"operational_timing": timing(right_reopen, 0)}
            }
        })
    }

    fn timing(reopen: u64, compaction: u64) -> Value {
        let reopen_ns = if reopen == 0 { vec![] } else { vec![reopen] };
        let compaction_ns = if compaction == 0 {
            vec![]
        } else {
            vec![compaction]
        };
        let reopen_samples = if reopen == 0 {
            json!([])
        } else {
            json!([{
                "measured_step_index": 0,
                "duration_ns": reopen,
                "work": {"unit": "btree_page_access", "units_examined": 1, "bytes_examined": 4096}
            }])
        };
        let compaction_samples = if compaction == 0 {
            json!([])
        } else {
            json!([{
                "measured_step_index": 0,
                "duration_ns": compaction,
                "work": {"unit": "lsm_sstable_record_version", "units_examined": 1, "bytes_examined": 16}
            }])
        };
        json!({
            "reopen_ns": reopen_ns,
            "compaction_stall_ns": compaction_ns,
            "reopen_samples": reopen_samples,
            "compaction_stall_samples": compaction_samples
        })
    }

    fn trace_value() -> Value {
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
        serde_json::to_value(trace).expect("encode trace")
    }

    fn write_base_archive(path: &Path, version: u16, batch: &Value, sidecars: Option<&Value>) {
        let mut environment = json!({
            "format_version": version,
            "repository_revision": "abc123",
            "execution_protocol": "fresh_counterbalanced_repeated_batch_v1",
            "attempt_protocol": "retain_all_requested_pairs_v1",
            "pair_seed": 0,
            "requested_pairs": 1,
            "engine_layout": "pair-{pair_index:06}/repetition-{repetition_index}/{btree.db|lsm}",
            "cache_state": "warm"
        });
        let mut files = vec!["trace.json", "batch.json", "environment.json"];
        let mut index = json!({
            "format_version": version,
            "repository_revision": "abc123",
            "execution_protocol": "fresh_counterbalanced_repeated_batch_v1",
            "attempt_protocol": "retain_all_requested_pairs_v1",
            "files": files
        });
        if let Some(sidecars) = sidecars {
            environment["comparison_failure_protocol"] = json!(FAILURE_PROTOCOL_V2);
            files.push("comparison-failures.json");
            index["comparison_failure_protocol"] = json!(FAILURE_PROTOCOL_V2);
            index["files"] = json!(files);
            write_json(path, "comparison-failures.json", sidecars);
        }
        write_json(path, "trace.json", batch.get("trace").expect("batch trace"));
        write_json(path, "batch.json", batch);
        write_json(path, "environment.json", &environment);
        write_json(path, "index.json", &index);
    }

    fn write_json(path: &Path, name: &str, value: &Value) {
        fs::write(
            path.join(name),
            serde_json::to_vec_pretty(value).expect("encode json"),
        )
        .expect("write json");
    }
}
