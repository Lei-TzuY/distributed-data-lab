use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use db_cli::generation_cleanup::cleanup_obsolete_generations;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-cleanup",
    version,
    about = "Safely remove obsolete append-log generations on supported hosts"
)]
struct Cli {
    /// Existing generation directory whose obsolete retained history should be removed.
    #[arg(long)]
    directory: PathBuf,
}

fn main() -> ExitCode {
    let args = Cli::parse();
    match cleanup_obsolete_generations(&args.directory) {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode generation cleanup summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}
