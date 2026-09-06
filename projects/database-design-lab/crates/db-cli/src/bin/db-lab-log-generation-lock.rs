use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use db_cli::generation_lock::{clear_stale_generation_writer_lock, inspect_generation_writer_lock};

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-lock",
    version,
    about = "Inspect or explicitly clear stale append-log generation writer locks"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    /// Read retained lock evidence without changing it.
    Inspect {
        #[arg(long)]
        directory: PathBuf,
    },
    /// Remove exactly the lock evidence previously inspected, after external liveness confirmation.
    ClearStale {
        #[arg(long)]
        directory: PathBuf,
        /// Exact record_hex returned by `inspect`.
        #[arg(long)]
        expected_record_hex: String,
        /// Required operator attestation; the tool never infers writer liveness from PID or age.
        #[arg(long)]
        confirm_no_live_writer: bool,
    },
}

fn main() -> ExitCode {
    let result = match Cli::parse().command {
        Command::Inspect { directory } => {
            inspect_generation_writer_lock(&directory).and_then(|summary| encode_json(&summary))
        }
        Command::ClearStale {
            directory,
            expected_record_hex,
            confirm_no_live_writer,
        } => clear_stale_generation_writer_lock(
            &directory,
            &expected_record_hex,
            confirm_no_live_writer,
        )
        .and_then(|summary| encode_json(&summary)),
    };

    match result {
        Ok(encoded) => {
            println!("{encoded}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}

fn encode_json<T: serde::Serialize>(
    value: &T,
) -> Result<String, db_cli::generation_lock::GenerationWriterLockError> {
    serde_json::to_string_pretty(value).map_err(|error| {
        db_cli::generation_lock::GenerationWriterLockError::Invalid(format!(
            "failed to encode generation writer lock summary: {error}"
        ))
    })
}
