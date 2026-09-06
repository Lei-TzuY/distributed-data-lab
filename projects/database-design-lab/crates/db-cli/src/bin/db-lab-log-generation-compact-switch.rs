use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::Parser;
#[cfg(not(windows))]
use db_cli::generation_compaction::compact_switch_generation_offline;
use db_cli::generation_compaction::{
    OfflineGenerationCompactSwitchError, OfflineGenerationCompactSwitchSummary,
};
#[cfg(windows)]
use db_cli::generation_compaction_windows::compact_switch_generation_offline_windows;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-compact-switch",
    version,
    about = "Offline compact and durably switch an append-log generation directory"
)]
struct Cli {
    /// Existing verified generation directory. All writers must remain quiesced for this command.
    #[arg(long)]
    directory: PathBuf,
}

fn main() -> ExitCode {
    match run_switch(&Cli::parse().directory) {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode compact-switch summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}

#[cfg(windows)]
fn run_switch(
    directory: &Path,
) -> Result<OfflineGenerationCompactSwitchSummary, OfflineGenerationCompactSwitchError> {
    compact_switch_generation_offline_windows(directory)
}

#[cfg(not(windows))]
fn run_switch(
    directory: &Path,
) -> Result<OfflineGenerationCompactSwitchSummary, OfflineGenerationCompactSwitchError> {
    compact_switch_generation_offline(directory)
}
