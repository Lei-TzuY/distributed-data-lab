use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::Parser;
use db_cli::generation_engine::GenerationLogEngine;
use db_core::{execute_workload, DbError, KvEngine, Outcome, Workload};
use serde::Serialize;
use thiserror::Error;

const MAX_JSON_INPUT_BYTES: u64 = 64 * 1024 * 1024;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-run",
    version,
    about = "Execute a workload through the generation-aware append-log ownership contract"
)]
struct Cli {
    /// Canonical generation directory. Mutations are routed through GenerationLogEngine.
    #[arg(long)]
    directory: PathBuf,

    /// Versioned workload JSON file.
    #[arg(long)]
    workload: PathBuf,
}

#[derive(Debug, Serialize)]
struct RunReport {
    engine: &'static str,
    workload_format_version: u16,
    seed: Option<u64>,
    steps_executed: usize,
    authoritative_generation: u64,
    authoritative_log: String,
    outcomes: Vec<Outcome>,
}

#[derive(Debug, Error)]
enum RunError {
    #[error("invalid generation workload run: {0}")]
    Invalid(String),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(transparent)]
    Json(#[from] serde_json::Error),
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

fn run(cli: Cli) -> Result<(), RunError> {
    let workload = read_workload(&cli.workload)?;
    let mut engine = GenerationLogEngine::open(&cli.directory)?;
    let engine_name = engine.capabilities().name;
    let outcomes = execute_workload(&mut engine, &workload)?;
    let authoritative_generation = engine.authoritative_generation();
    let authoritative_log = engine.authoritative_log_path().display().to_string();

    let report = RunReport {
        engine: engine_name,
        workload_format_version: workload.format_version,
        seed: workload.seed,
        steps_executed: outcomes.len(),
        authoritative_generation,
        authoritative_log,
        outcomes,
    };
    println!("{}", serde_json::to_string_pretty(&report)?);
    Ok(())
}

fn read_workload(path: &Path) -> Result<Workload, RunError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_file() {
        return invalid(format!(
            "workload must be a real regular file rather than a symlink or non-file: {}",
            path.display()
        ));
    }
    if metadata.len() > MAX_JSON_INPUT_BYTES {
        return invalid(format!(
            "workload JSON exceeds {MAX_JSON_INPUT_BYTES} bytes: {}",
            path.display()
        ));
    }
    let encoded = fs::read(path).map_err(|source| io_error(path, source))?;
    if encoded.len() as u64 > MAX_JSON_INPUT_BYTES {
        return invalid(format!(
            "workload JSON grew beyond {MAX_JSON_INPUT_BYTES} bytes while reading: {}",
            path.display()
        ));
    }
    let workload: Workload = serde_json::from_slice(&encoded)?;
    workload.validate()?;
    Ok(workload)
}

fn io_error(path: &Path, source: io::Error) -> RunError {
    RunError::Io {
        path: path.to_path_buf(),
        source,
    }
}

fn invalid<T>(message: impl Into<String>) -> Result<T, RunError> {
    Err(RunError::Invalid(message.into()))
}
