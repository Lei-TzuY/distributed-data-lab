use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use db_cli::generation_cutover_verify::verify_fresh_legacy_cutover;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-cutover-verify",
    version,
    about = "Verify fresh legacy append-log cutover evidence without modifying it"
)]
struct Cli {
    /// Legacy pathname that should now contain the cutover sentinel.
    #[arg(long)]
    legacy_source: PathBuf,

    /// Fresh generation directory that the sentinel must bind.
    #[arg(long)]
    target_directory: PathBuf,
}

fn main() -> ExitCode {
    let args = Cli::parse();
    match verify_fresh_legacy_cutover(&args.legacy_source, &args.target_directory) {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode cutover verification summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}
