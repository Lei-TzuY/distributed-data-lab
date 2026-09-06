use std::fs::{self, OpenOptions};
use std::io::{self, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, ValueEnum};
use db_core::{
    compare_workload, minimize_failing_workload, DbError, DifferentialError, ErrorClass, KvEngine,
    Workload,
};
use db_storage_btree::{BPlusTree, BtreeError};
use db_storage_log::LogEngine;
use db_storage_lsm::LsmEngine;
use db_storage_memory::MemoryEngine;
use tempfile::tempdir;
use thiserror::Error;

const MAX_JSON_INPUT_BYTES: u64 = 64 * 1024 * 1024;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-shrink",
    version,
    about = "Minimize a reproducible differential failure to a 1-minimal workload"
)]
struct Cli {
    /// Persistent candidate engine whose failure is reproduced against the in-memory oracle.
    #[arg(long, value_enum)]
    engine: CandidateKind,
    /// B+ tree validated-page cache capacity when `--engine btree` is selected.
    #[arg(long, default_value_t = 64)]
    btree_cache_pages: usize,
    /// Versioned workload JSON that currently reproduces a differential failure.
    workload: PathBuf,
    /// New minimized workload JSON file; existing files are never overwritten.
    #[arg(long)]
    output: PathBuf,
}

#[derive(Debug, Clone, Copy, ValueEnum)]
enum CandidateKind {
    Log,
    Btree,
    Lsm,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FailureSignature {
    InvalidWorkload(ErrorClass),
    IncompatibleCapabilities,
    LeftEngine(ErrorClass),
    RightEngine(ErrorClass),
    Mismatch,
}

#[derive(Debug, Error)]
enum CliError {
    #[error(transparent)]
    Io(#[from] io::Error),
    #[error(transparent)]
    Json(#[from] serde_json::Error),
    #[error(transparent)]
    Db(#[from] DbError),
    #[error(transparent)]
    Btree(#[from] BtreeError),
    #[error("{0}")]
    Usage(String),
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run(cli: Cli) -> Result<(), CliError> {
    if matches!(cli.engine, CandidateKind::Btree) && cli.btree_cache_pages == 0 {
        return Err(CliError::Usage(
            "--btree-cache-pages must be greater than zero".to_owned(),
        ));
    }
    let workload = load_workload(&cli.workload)?;
    let target =
        replay_signature(cli.engine, cli.btree_cache_pages, &workload)?.ok_or_else(|| {
            CliError::Usage(
                "the input workload does not currently reproduce a differential failure".to_owned(),
            )
        })?;

    let minimized = minimize_failing_workload(&workload, |candidate| {
        Ok::<_, CliError>(
            replay_signature(cli.engine, cli.btree_cache_pages, candidate)? == Some(target),
        )
    })?;

    write_new_json(&cli.output, &minimized)?;
    println!(
        "minimized differential failure from {} to {} steps ({target:?})",
        workload.steps.len(),
        minimized.steps.len()
    );
    Ok(())
}

fn replay_signature(
    engine: CandidateKind,
    btree_cache_pages: usize,
    workload: &Workload,
) -> Result<Option<FailureSignature>, CliError> {
    let directory = tempdir()?;
    match engine {
        CandidateKind::Log => {
            let candidate = LogEngine::create_new(directory.path().join("candidate.log"))?;
            Ok(compare_candidate(candidate, workload))
        }
        CandidateKind::Btree => {
            let candidate =
                BPlusTree::create_new(directory.path().join("candidate.btree"), btree_cache_pages)?;
            Ok(compare_candidate(candidate, workload))
        }
        CandidateKind::Lsm => {
            let candidate = LsmEngine::create_new(directory.path().join("candidate-lsm"))?;
            Ok(compare_candidate(candidate, workload))
        }
    }
}

fn compare_candidate<E: KvEngine>(
    mut candidate: E,
    workload: &Workload,
) -> Option<FailureSignature> {
    let mut reference = MemoryEngine::new();
    compare_workload(&mut reference, &mut candidate, workload)
        .err()
        .map(|error| failure_signature(&error))
}

fn failure_signature(error: &DifferentialError) -> FailureSignature {
    match error {
        DifferentialError::InvalidWorkload(source) => {
            FailureSignature::InvalidWorkload(source.class())
        }
        DifferentialError::IncompatibleCapabilities(_) => {
            FailureSignature::IncompatibleCapabilities
        }
        DifferentialError::LeftEngine { source, .. } => {
            FailureSignature::LeftEngine(source.class())
        }
        DifferentialError::RightEngine { source, .. } => {
            FailureSignature::RightEngine(source.class())
        }
        DifferentialError::Mismatch { .. } => FailureSignature::Mismatch,
    }
}

fn load_workload(path: &Path) -> Result<Workload, CliError> {
    let metadata = fs::metadata(path)?;
    if metadata.len() > MAX_JSON_INPUT_BYTES {
        return Err(CliError::Usage(format!(
            "workload JSON has {} bytes; maximum is {MAX_JSON_INPUT_BYTES}",
            metadata.len()
        )));
    }
    let bytes = fs::read(path)?;
    let workload: Workload = serde_json::from_slice(&bytes)?;
    workload.validate()?;
    Ok(workload)
}

fn write_new_json(path: &Path, value: &Workload) -> Result<(), CliError> {
    let file = OpenOptions::new().write(true).create_new(true).open(path)?;
    let mut writer = BufWriter::new(file);
    serde_json::to_writer_pretty(&mut writer, value)?;
    writer.write_all(b"\n")?;
    writer.flush()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use clap::Parser;
    use db_core::{DbError, DifferentialError, ErrorClass};

    use super::{failure_signature, CandidateKind, Cli, FailureSignature};

    #[test]
    fn parser_accepts_all_persistent_candidates() {
        for engine in ["log", "btree", "lsm"] {
            let cli = Cli::try_parse_from([
                "db-lab-shrink",
                "--engine",
                engine,
                "failure.json",
                "--output",
                "minimal.json",
            ])
            .expect("parse shrink command");
            assert!(matches!(
                (engine, cli.engine),
                ("log", CandidateKind::Log)
                    | ("btree", CandidateKind::Btree)
                    | ("lsm", CandidateKind::Lsm)
            ));
        }
    }

    #[test]
    fn failure_signature_preserves_engine_side_and_error_class() {
        let right = DifferentialError::RightEngine {
            step_index: 7,
            source: DbError::Poisoned,
        };
        assert_eq!(
            failure_signature(&right),
            FailureSignature::RightEngine(ErrorClass::Poisoned)
        );

        let incompatible =
            DifferentialError::IncompatibleCapabilities("different limits".to_owned());
        assert_eq!(
            failure_signature(&incompatible),
            FailureSignature::IncompatibleCapabilities
        );
    }
}
