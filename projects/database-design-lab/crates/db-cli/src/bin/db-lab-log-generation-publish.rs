use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
#[cfg(unix)]
use db_cli::generation_lock::acquire_generation_writer_lease;
use db_cli::generation_publication::{publish_generation_marker, GenerationPublicationSummary};

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-publish",
    version,
    about = "Durably publish an append-log generation commit marker on supported hosts"
)]
struct Cli {
    /// Existing generation directory.
    #[arg(long)]
    directory: PathBuf,

    /// Generation id whose clean append-log image should become committed.
    #[arg(long)]
    generation: u64,
}

fn main() -> ExitCode {
    let args = Cli::parse();
    match run_publication(&args) {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode publication summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}

#[cfg(unix)]
fn run_publication(args: &Cli) -> Result<GenerationPublicationSummary, String> {
    let lease =
        acquire_generation_writer_lease(&args.directory).map_err(|error| error.to_string())?;
    publish_generation_marker(lease.directory(), args.generation).map_err(|error| error.to_string())
}

#[cfg(not(unix))]
fn run_publication(args: &Cli) -> Result<GenerationPublicationSummary, String> {
    publish_generation_marker(&args.directory, args.generation).map_err(|error| error.to_string())
}
