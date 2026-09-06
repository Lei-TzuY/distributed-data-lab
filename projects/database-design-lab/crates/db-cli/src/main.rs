use std::fs::{self, OpenOptions};
use std::io::{self, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::{SystemTime, UNIX_EPOCH};

use clap::{Parser, Subcommand, ValueEnum};
use db_cli::generation_directory::parse_canonical_generation_name;
use db_core::{
    compare_experiment_trace, compare_workload, execute_workload, generate_experiment_trace,
    generate_workload, DbError, DifferentialError, ExperimentComparisonReport,
    ExperimentGeneratorConfig, ExperimentProfile, ExperimentTrace, GeneratorConfig, KvEngine,
    Outcome, Workload,
};
use db_storage_btree::{BPlusTree, BtreeError};
use db_storage_log::{InspectionReport, LogEngine, VerificationReport};
use db_storage_lsm::LsmEngine;
use db_storage_memory::MemoryEngine;
use serde::Serialize;
use thiserror::Error;

mod counterbalanced_archive;

use counterbalanced_archive::CounterbalancedArchiveArgs;

const MAX_JSON_INPUT_BYTES: u64 = 64 * 1024 * 1024;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab",
    version,
    about = "Deterministic correctness laboratory for database architecture experiments"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    /// Generate a versioned deterministic workload JSON file.
    Generate {
        /// Recorded SplitMix64 seed.
        #[arg(long)]
        seed: u64,
        /// Number of logical KV actions.
        #[arg(long, default_value_t = 1_000)]
        operations: u32,
        /// Number of reusable keys.
        #[arg(long, default_value_t = 128)]
        key_space: u32,
        /// Inclusive maximum generated value size.
        #[arg(long, default_value_t = 256)]
        max_value_bytes: u32,
        /// Insert an engine reopen after this many logical actions.
        #[arg(long)]
        reopen_every: Option<u32>,
        /// New JSON file to create; existing files are never overwritten.
        #[arg(long)]
        output: PathBuf,
    },
    /// Execute a workload against one engine and print all observable outcomes as JSON.
    Run {
        /// Engine implementation.
        #[arg(long, value_enum)]
        engine: EngineKind,
        /// Required for persistent engines; forbidden for `memory`.
        #[arg(long)]
        path: Option<PathBuf>,
        /// Versioned workload JSON file.
        workload: PathBuf,
    },
    /// Compare the reference engine and a fresh persistent candidate step by step.
    Differential {
        /// Persistent candidate engine.
        #[arg(long, value_enum, default_value_t = PersistentEngineKind::Log)]
        engine: PersistentEngineKind,
        /// New candidate file or directory to create; existing paths are rejected.
        #[arg(long)]
        path: PathBuf,
        /// Versioned workload JSON file.
        workload: PathBuf,
    },
    /// Generate a versioned Phase 4 experiment trace with setup and measured windows.
    ExperimentGenerate {
        /// Stable experiment family.
        #[arg(long, value_enum)]
        profile: ExperimentProfileKind,
        /// Recorded SplitMix64 seed.
        #[arg(long)]
        seed: u64,
        /// Number of measured logical operations, excluding inserted reopen steps.
        #[arg(long, default_value_t = 1_000)]
        operations: u32,
        /// Number of reusable logical key ids.
        #[arg(long, default_value_t = 4_096)]
        key_space: u32,
        /// Fixed generated value size.
        #[arg(long, default_value_t = 128)]
        value_bytes: u32,
        /// Maximum rows requested by generated range scans.
        #[arg(long, default_value_t = 16)]
        range_limit: u32,
        /// Insert a measured reopen after this many logical operations.
        #[arg(long)]
        reopen_every: Option<u32>,
        /// New trace JSON file to create; existing files are never overwritten.
        #[arg(long)]
        output: PathBuf,
    },
    /// Run one shared trace against fresh B+ tree and LSM candidates and archive exact evidence.
    ExperimentCompare {
        /// Versioned experiment trace JSON file.
        #[arg(long)]
        trace: PathBuf,
        /// New B+ tree page file to create.
        #[arg(long)]
        btree_path: PathBuf,
        /// New LSM directory to create.
        #[arg(long)]
        lsm_path: PathBuf,
        /// B+ tree validated-page cache capacity, recorded in the output wrapper.
        #[arg(long, default_value_t = 64)]
        btree_cache_pages: usize,
        /// New self-contained comparison report JSON file.
        #[arg(long)]
        output: PathBuf,
    },
    /// Run a shared comparison and create a new immutable evidence archive directory.
    ExperimentArchive {
        /// Versioned experiment trace JSON file.
        #[arg(long)]
        trace: PathBuf,
        /// New B+ tree page file to create.
        #[arg(long)]
        btree_path: PathBuf,
        /// New LSM directory to create.
        #[arg(long)]
        lsm_path: PathBuf,
        /// B+ tree validated-page cache capacity.
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
        /// Filesystem under test, when known (for example, `ntfs`, `ext4`, `apfs`).
        #[arg(long)]
        filesystem: Option<String>,
        /// Storage device/model label, when known. Do not place credentials or serial numbers here.
        #[arg(long)]
        storage_device: Option<String>,
        /// Declared cache preparation state for this run.
        #[arg(long, value_enum, default_value_t = CacheStateKind::Unspecified)]
        cache_state: CacheStateKind,
        /// Optional free-form experiment note. Do not include secrets.
        #[arg(long)]
        notes: Option<String>,
    },
    /// Run a fresh two-repetition AB/BA pair and archive exact evidence plus order provenance.
    ExperimentArchiveCounterbalanced {
        #[command(flatten)]
        args: CounterbalancedArchiveArgs,
    },
    /// Validate an append-log file without modifying it.
    Verify {
        /// Append-log file.
        path: PathBuf,
    },
    /// Replay and show live entries without modifying the append-log file.
    Inspect {
        /// Append-log file.
        path: PathBuf,
        /// Include values as hexadecimal. By default only key and value size are printed.
        #[arg(long)]
        show_values: bool,
    },
}

#[derive(Debug, Clone, Copy, ValueEnum)]
enum EngineKind {
    Memory,
    Log,
    Lsm,
}

#[derive(Debug, Clone, Copy, ValueEnum)]
enum PersistentEngineKind {
    Log,
    Lsm,
}

#[derive(Debug, Clone, Copy, ValueEnum)]
enum ExperimentProfileKind {
    PointRead,
    RangeScan,
    SequentialWrite,
    RandomWrite,
    Mixed,
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

impl From<ExperimentProfileKind> for ExperimentProfile {
    fn from(value: ExperimentProfileKind) -> Self {
        match value {
            ExperimentProfileKind::PointRead => Self::PointRead,
            ExperimentProfileKind::RangeScan => Self::RangeScan,
            ExperimentProfileKind::SequentialWrite => Self::SequentialWrite,
            ExperimentProfileKind::RandomWrite => Self::RandomWrite,
            ExperimentProfileKind::Mixed => Self::Mixed,
        }
    }
}

#[derive(Debug, Serialize)]
struct RunReport {
    engine: &'static str,
    workload_format_version: u16,
    seed: Option<u64>,
    steps_executed: usize,
    outcomes: Vec<Outcome>,
}

#[derive(Debug, Serialize)]
struct DifferentialCliReport {
    reference_engine: &'static str,
    candidate_engine: &'static str,
    workload_format_version: u16,
    seed: Option<u64>,
    steps_checked: usize,
}

#[derive(Debug, Serialize)]
struct ExperimentCliReport {
    btree_cache_pages: usize,
    comparison: ExperimentComparisonReport,
}

const EVIDENCE_ARCHIVE_FORMAT_VERSION: u16 = 1;

#[derive(Debug, Serialize)]
struct EvidenceArchiveEnvironment {
    format_version: u16,
    repository_revision: String,
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
    notes: Option<String>,
}

#[derive(Debug, Serialize)]
struct EvidenceArchiveIndex {
    format_version: u16,
    repository_revision: String,
    files: [&'static str; 3],
}

#[derive(Debug, Error)]
enum CliError {
    #[error("{0}")]
    Usage(String),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error(transparent)]
    Btree(#[from] BtreeError),
    #[error(transparent)]
    Differential(#[from] DifferentialError),
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
    #[error("invalid JSON: {0}")]
    Json(#[from] serde_json::Error),
}

fn main() -> ExitCode {
    match run_cli(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}

fn run_cli(cli: Cli) -> Result<(), CliError> {
    match cli.command {
        Command::Generate {
            seed,
            operations,
            key_space,
            max_value_bytes,
            reopen_every,
            output,
        } => {
            let workload = generate_workload(GeneratorConfig {
                seed,
                operations,
                key_space,
                max_value_bytes,
                reopen_every,
            })?;
            write_new_json(&output, &workload)
        }
        Command::Run {
            engine,
            path,
            workload,
        } => {
            let workload = read_workload(&workload)?;
            match (engine, path) {
                (EngineKind::Memory, None) => {
                    let mut engine = MemoryEngine::new();
                    print_run_report(&mut engine, &workload)
                }
                (EngineKind::Memory, Some(_)) => Err(CliError::Usage(
                    "--path is not valid for the in-memory engine".to_owned(),
                )),
                (EngineKind::Log, Some(path)) => {
                    reject_canonical_generation_raw_mutation(&path)?;
                    let mut engine = LogEngine::open(path)?;
                    print_run_report(&mut engine, &workload)
                }
                (EngineKind::Log, None) => Err(CliError::Usage(
                    "--path is required for the append-log engine".to_owned(),
                )),
                (EngineKind::Lsm, Some(path)) => {
                    let mut engine = LsmEngine::open(path)?;
                    print_run_report(&mut engine, &workload)
                }
                (EngineKind::Lsm, None) => Err(CliError::Usage(
                    "--path is required for the LSM engine".to_owned(),
                )),
            }
        }
        Command::Differential {
            engine,
            path,
            workload,
        } => {
            let workload = read_workload(&workload)?;
            let mut reference = MemoryEngine::new();
            match engine {
                PersistentEngineKind::Log => {
                    reject_canonical_generation_raw_mutation(&path)?;
                    let mut candidate = LogEngine::create_new(path)?;
                    print_differential_report(&mut reference, &mut candidate, &workload)
                }
                PersistentEngineKind::Lsm => {
                    let mut candidate = LsmEngine::create_new(path)?;
                    print_differential_report(&mut reference, &mut candidate, &workload)
                }
            }
        }
        Command::ExperimentGenerate {
            profile,
            seed,
            operations,
            key_space,
            value_bytes,
            range_limit,
            reopen_every,
            output,
        } => {
            let trace = generate_experiment_trace(ExperimentGeneratorConfig {
                seed,
                profile: profile.into(),
                operations,
                key_space,
                value_bytes,
                range_limit,
                reopen_every,
            })?;
            write_new_json(&output, &trace)
        }
        Command::ExperimentCompare {
            trace,
            btree_path,
            lsm_path,
            btree_cache_pages,
            output,
        } => {
            let trace = read_experiment_trace(&trace)?;
            ensure_fresh_experiment_targets(&btree_path, &lsm_path, &output)?;
            let mut btree = BPlusTree::create_new(&btree_path, btree_cache_pages)?;
            let mut lsm = LsmEngine::create_new(&lsm_path)?;
            let comparison = compare_experiment_trace(&mut btree, &mut lsm, &trace)?;
            write_new_json(
                &output,
                &ExperimentCliReport {
                    btree_cache_pages,
                    comparison,
                },
            )
        }
        Command::ExperimentArchive {
            trace,
            btree_path,
            lsm_path,
            btree_cache_pages,
            revision,
            archive_dir,
            host_label,
            filesystem,
            storage_device,
            cache_state,
            notes,
        } => {
            validate_revision(&revision)?;
            let trace = read_experiment_trace(&trace)?;
            ensure_fresh_archive_targets(&btree_path, &lsm_path, &archive_dir)?;
            let mut btree = BPlusTree::create_new(&btree_path, btree_cache_pages)?;
            let mut lsm = LsmEngine::create_new(&lsm_path)?;
            let comparison = compare_experiment_trace(&mut btree, &mut lsm, &trace)?;
            let environment = EvidenceArchiveEnvironment {
                format_version: EVIDENCE_ARCHIVE_FORMAT_VERSION,
                repository_revision: revision.clone(),
                db_lab_version: env!("CARGO_PKG_VERSION"),
                target_os: std::env::consts::OS,
                target_arch: std::env::consts::ARCH,
                build_profile: if cfg!(debug_assertions) {
                    "debug"
                } else {
                    "release"
                },
                rustc_version: rustc_version(),
                host_label,
                filesystem,
                storage_device,
                cache_state: cache_state.as_str(),
                btree_cache_pages,
                recorded_unix_seconds: SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .map_err(|error| {
                        CliError::Usage(format!("system clock precedes Unix epoch: {error}"))
                    })?
                    .as_secs(),
                notes,
            };
            write_evidence_archive(&archive_dir, &revision, &trace, &comparison, &environment)
        }
        Command::ExperimentArchiveCounterbalanced { args } => counterbalanced_archive::run(args),
        Command::Verify { path } => {
            let report: VerificationReport = LogEngine::verify(path)?;
            write_stdout_json(&report)
        }
        Command::Inspect { path, show_values } => {
            let report: InspectionReport = LogEngine::inspect(path, show_values)?;
            write_stdout_json(&report)
        }
    }
}

fn reject_canonical_generation_raw_mutation(path: &Path) -> Result<(), CliError> {
    let Some(name) = path.file_name().and_then(|name| name.to_str()) else {
        return Ok(());
    };
    if matches!(parse_canonical_generation_name(name), Ok(Some(_))) {
        return Err(CliError::Usage(format!(
            "raw append-log mutation refuses canonical generation path {}; use db-lab-log-generation-run --directory <generation-directory> --workload <workload> instead",
            path.display()
        )));
    }
    Ok(())
}

fn print_run_report<E: KvEngine>(engine: &mut E, workload: &Workload) -> Result<(), CliError> {
    let engine_name = engine.capabilities().name;
    let outcomes = execute_workload(engine, workload)?;
    write_stdout_json(&RunReport {
        engine: engine_name,
        workload_format_version: workload.format_version,
        seed: workload.seed,
        steps_executed: outcomes.len(),
        outcomes,
    })
}

fn print_differential_report<E: KvEngine>(
    reference: &mut MemoryEngine,
    candidate: &mut E,
    workload: &Workload,
) -> Result<(), CliError> {
    let reference_name = reference.capabilities().name;
    let candidate_name = candidate.capabilities().name;
    let report = compare_workload(reference, candidate, workload)?;
    write_stdout_json(&DifferentialCliReport {
        reference_engine: reference_name,
        candidate_engine: candidate_name,
        workload_format_version: workload.format_version,
        seed: workload.seed,
        steps_checked: report.steps_checked,
    })
}

fn read_workload(path: &Path) -> Result<Workload, CliError> {
    let encoded = read_bounded_json(path, "workload")?;
    let workload: Workload = serde_json::from_slice(&encoded)?;
    workload.validate()?;
    Ok(workload)
}

fn read_experiment_trace(path: &Path) -> Result<ExperimentTrace, CliError> {
    let encoded = read_bounded_json(path, "experiment trace")?;
    let trace: ExperimentTrace = serde_json::from_slice(&encoded)?;
    trace.validate()?;
    Ok(trace)
}

fn read_bounded_json(path: &Path, kind: &str) -> Result<Vec<u8>, CliError> {
    let metadata = fs::metadata(path)?;
    if metadata.len() > MAX_JSON_INPUT_BYTES {
        return Err(CliError::Usage(format!(
            "{kind} file has {} bytes; maximum is {MAX_JSON_INPUT_BYTES}",
            metadata.len()
        )));
    }
    Ok(fs::read(path)?)
}

fn ensure_fresh_experiment_targets(
    btree_path: &Path,
    lsm_path: &Path,
    output: &Path,
) -> Result<(), CliError> {
    if btree_path == lsm_path || btree_path == output || lsm_path == output {
        return Err(CliError::Usage(
            "experiment B+ tree, LSM, and output paths must be distinct".to_owned(),
        ));
    }
    for (label, path) in [
        ("B+ tree", btree_path),
        ("LSM", lsm_path),
        ("output", output),
    ] {
        if path.exists() {
            return Err(CliError::Usage(format!(
                "experiment {label} path already exists: {}",
                path.display()
            )));
        }
    }
    Ok(())
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

fn ensure_fresh_archive_targets(
    btree_path: &Path,
    lsm_path: &Path,
    archive_dir: &Path,
) -> Result<(), CliError> {
    if btree_path == lsm_path || btree_path == archive_dir || lsm_path == archive_dir {
        return Err(CliError::Usage(
            "archive B+ tree, LSM, and archive paths must be distinct".to_owned(),
        ));
    }
    for (label, path) in [
        ("B+ tree", btree_path),
        ("LSM", lsm_path),
        ("archive", archive_dir),
    ] {
        if path.exists() {
            return Err(CliError::Usage(format!(
                "experiment {label} path already exists: {}",
                path.display()
            )));
        }
    }
    Ok(())
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

fn write_evidence_archive(
    archive_dir: &Path,
    revision: &str,
    trace: &ExperimentTrace,
    comparison: &ExperimentComparisonReport,
    environment: &EvidenceArchiveEnvironment,
) -> Result<(), CliError> {
    fs::create_dir(archive_dir)?;
    let result = (|| {
        write_new_json(&archive_dir.join("trace.json"), trace)?;
        write_new_json(&archive_dir.join("comparison.json"), comparison)?;
        write_new_json(&archive_dir.join("environment.json"), environment)?;
        write_new_json(
            &archive_dir.join("index.json"),
            &EvidenceArchiveIndex {
                format_version: EVIDENCE_ARCHIVE_FORMAT_VERSION,
                repository_revision: revision.to_owned(),
                files: ["trace.json", "comparison.json", "environment.json"],
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

fn write_stdout_json(value: &impl Serialize) -> Result<(), CliError> {
    let stdout = io::stdout();
    let mut writer = BufWriter::new(stdout.lock());
    serde_json::to_writer_pretty(&mut writer, value)?;
    writer.write_all(b"\n")?;
    writer.flush()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use clap::Parser;
    use db_core::{
        compare_experiment_trace, generate_experiment_trace, ExperimentGeneratorConfig,
        ExperimentProfile, ReadWorkUnit, Workload,
    };
    use db_storage_btree::BPlusTree;
    use db_storage_lsm::LsmEngine;
    use tempfile::tempdir;

    use super::{
        validate_revision, CacheStateKind, Cli, Command, EngineKind, ExperimentProfileKind,
        PersistentEngineKind,
    };

    #[test]
    fn suggested_run_shape_parses() {
        let cli = Cli::try_parse_from([
            "db-lab",
            "run",
            "--engine",
            "log",
            "--path",
            "lab.db",
            "workload.json",
        ])
        .expect("parse run command");
        assert!(matches!(
            cli.command,
            Command::Run {
                engine: EngineKind::Log,
                ..
            }
        ));
    }

    #[test]
    fn committed_semantics_fixture_is_valid() {
        let encoded = include_str!("../../../fixtures/workloads/semantics-v1.json");
        let workload: Workload = serde_json::from_str(encoded).expect("parse semantics fixture");
        workload.validate().expect("validate semantics fixture");
        assert_eq!(workload.steps.len(), 11);
    }

    #[test]
    fn lsm_run_and_differential_shapes_parse() {
        let run = Cli::try_parse_from([
            "db-lab",
            "run",
            "--engine",
            "lsm",
            "--path",
            "engine-dir",
            "workload.json",
        ])
        .expect("parse LSM run");
        assert!(matches!(
            run.command,
            Command::Run {
                engine: EngineKind::Lsm,
                ..
            }
        ));

        let differential = Cli::try_parse_from([
            "db-lab",
            "differential",
            "--engine",
            "lsm",
            "--path",
            "fresh-dir",
            "workload.json",
        ])
        .expect("parse LSM differential");
        assert!(matches!(
            differential.command,
            Command::Differential {
                engine: PersistentEngineKind::Lsm,
                ..
            }
        ));
    }

    #[test]
    fn experiment_command_shapes_parse() {
        let generate = Cli::try_parse_from([
            "db-lab",
            "experiment-generate",
            "--profile",
            "mixed",
            "--seed",
            "42",
            "--output",
            "trace.json",
        ])
        .expect("parse experiment generation");
        assert!(matches!(
            generate.command,
            Command::ExperimentGenerate {
                profile: ExperimentProfileKind::Mixed,
                ..
            }
        ));

        let compare = Cli::try_parse_from([
            "db-lab",
            "experiment-compare",
            "--trace",
            "trace.json",
            "--btree-path",
            "tree.db",
            "--lsm-path",
            "lsm-dir",
            "--output",
            "report.json",
        ])
        .expect("parse experiment comparison");
        assert!(matches!(compare.command, Command::ExperimentCompare { .. }));
    }

    #[test]
    fn experiment_archive_shape_parses_and_revision_validation_is_strict() {
        let archive = Cli::try_parse_from([
            "db-lab",
            "experiment-archive",
            "--trace",
            "trace.json",
            "--btree-path",
            "tree.db",
            "--lsm-path",
            "lsm-dir",
            "--revision",
            "e1fb48a61d2a3ec6fafe4e9a4d001d5c6ce0231f",
            "--archive-dir",
            "evidence/run-001",
            "--cache-state",
            "warm",
        ])
        .expect("parse experiment archive");
        assert!(matches!(
            archive.command,
            Command::ExperimentArchive {
                cache_state: CacheStateKind::Warm,
                ..
            }
        ));

        let counterbalanced = Cli::try_parse_from([
            "db-lab",
            "experiment-archive-counterbalanced",
            "--trace",
            "trace.json",
            "--first-btree-path",
            "tree-a.db",
            "--first-lsm-path",
            "lsm-a",
            "--second-btree-path",
            "tree-b.db",
            "--second-lsm-path",
            "lsm-b",
            "--pair-order",
            "right-then-left-first",
            "--revision",
            "e1fb48a61d2a3ec6fafe4e9a4d001d5c6ce0231f",
            "--archive-dir",
            "evidence/run-002",
            "--cache-state",
            "warm",
        ])
        .expect("parse counterbalanced experiment archive");
        assert!(matches!(
            counterbalanced.command,
            Command::ExperimentArchiveCounterbalanced { .. }
        ));

        validate_revision("abc123").expect("simple revision");
        assert!(validate_revision("").is_err());
        assert!(validate_revision("bad revision with spaces").is_err());
    }

    #[test]
    fn real_btree_and_lsm_consume_the_exact_same_mixed_trace() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 0x2026_0829,
            profile: ExperimentProfile::Mixed,
            operations: 48,
            key_space: 24,
            value_bytes: 64,
            range_limit: 4,
            reopen_every: Some(13),
        })
        .expect("generate shared trace");
        let directory = tempdir().expect("temporary directory");
        let mut btree =
            BPlusTree::create_new(directory.path().join("tree.db"), 8).expect("create B+ tree");
        let mut lsm =
            LsmEngine::create_new(directory.path().join("lsm")).expect("create LSM engine");
        let report =
            compare_experiment_trace(&mut btree, &mut lsm, &trace).expect("compare engines");
        assert_eq!(report.outcomes.len(), trace.measured_steps.len());
        assert_eq!(
            report.left.amplification.point_read.unit,
            ReadWorkUnit::BtreePageAccess
        );
        assert_eq!(
            report.right.amplification.point_read.unit,
            ReadWorkUnit::LsmSstableConsult
        );
    }
}
