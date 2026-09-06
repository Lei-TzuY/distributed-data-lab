use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use db_cli::generation_directory::verify_generation_directory;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-verify",
    version,
    about = "Read-only verification of an append-log generation directory"
)]
struct Cli {
    /// Generation directory to inspect without modifying it.
    #[arg(long)]
    directory: PathBuf,
}

fn main() -> ExitCode {
    match verify_generation_directory(&Cli::parse().directory) {
        Ok(verified) => match serde_json::to_string_pretty(verified.summary()) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode verification summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}
