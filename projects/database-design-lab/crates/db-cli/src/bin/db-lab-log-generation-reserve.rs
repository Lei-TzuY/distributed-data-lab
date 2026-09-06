use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use db_cli::generation_reservation::reserve_next_generation;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-reserve",
    version,
    about = "Durably reserve the next append-log generation id on supported hosts"
)]
struct Cli {
    /// Existing verified generation directory.
    #[arg(long)]
    directory: PathBuf,
}

fn main() -> ExitCode {
    match reserve_next_generation(&Cli::parse().directory) {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode generation reservation summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}
