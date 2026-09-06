use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
#[cfg(not(windows))]
use db_cli::generation_cutover::cutover_migrated_legacy_append_log;
#[cfg(windows)]
use db_cli::generation_cutover_windows::cutover_migrated_legacy_append_log_windows;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-cutover",
    version,
    about = "Retire a migrated legacy append-log pathname behind a durable sentinel"
)]
struct Cli {
    /// Legacy one-file append-log pathname to retire. Raw-path writers must be quiesced and closed.
    #[arg(long)]
    legacy_source: PathBuf,

    /// Fresh generation directory produced by migration and not yet mutated after import.
    #[arg(long)]
    target_directory: PathBuf,
}

fn main() -> ExitCode {
    let args = Cli::parse();
    #[cfg(windows)]
    let result =
        cutover_migrated_legacy_append_log_windows(&args.legacy_source, &args.target_directory);
    #[cfg(not(windows))]
    let result = cutover_migrated_legacy_append_log(&args.legacy_source, &args.target_directory);

    match result {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode cutover summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}
