use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use db_cli::host_preflight::verify_host_preflight_snapshot;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-host-preflight-verify",
    version,
    about = "Fail-closed verification for Linux controlled-host preflight snapshots"
)]
struct Cli {
    /// Existing host-preflight JSON snapshot to verify.
    #[arg(long)]
    snapshot: PathBuf,
    /// Optional exact host label expected by the caller.
    #[arg(long)]
    expected_host_label: Option<String>,
    /// Reject internally valid snapshots that record passed=false.
    #[arg(long)]
    require_passed: bool,
}

fn main() -> ExitCode {
    let args = Cli::parse();
    match verify_host_preflight_snapshot(
        &args.snapshot,
        args.expected_host_label.as_deref(),
        args.require_passed,
    ) {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
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
