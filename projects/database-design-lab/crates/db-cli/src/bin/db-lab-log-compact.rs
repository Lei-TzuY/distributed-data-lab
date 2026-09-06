use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use db_cli::log_compaction::compact_log_to_fresh_file;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-compact",
    version,
    about = "Publish a non-destructive compact copy of a clean append-log file"
)]
struct Cli {
    /// Existing clean append-log source. The source is opened read-only and is never repaired.
    #[arg(long)]
    source: PathBuf,
    /// Fresh compacted v1 append-log file. Existing paths are never overwritten.
    #[arg(long)]
    output: PathBuf,
}

fn main() -> ExitCode {
    let args = Cli::parse();
    match compact_log_to_fresh_file(&args.source, &args.output) {
        Ok(report) => match serde_json::to_string_pretty(&report) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode compaction report: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}
