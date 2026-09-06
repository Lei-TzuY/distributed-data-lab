use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use db_cli::generation_cleanup_windows::cleanup_obsolete_generations_windows;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-cleanup-windows",
    version,
    about = "Durably retire obsolete append-log generation history from the Windows authoritative namespace"
)]
struct Cli {
    #[arg(long)]
    directory: PathBuf,
}

fn main() -> ExitCode {
    match cleanup_obsolete_generations_windows(&Cli::parse().directory) {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode Windows cleanup summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}
