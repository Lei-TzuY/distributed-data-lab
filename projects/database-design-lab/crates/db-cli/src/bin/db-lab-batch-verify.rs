use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use db_cli::batch_archive::verify_batch_archive;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-batch-verify",
    version,
    about = "Fail-closed verifier for immutable repeated Phase 4 evidence archives"
)]
struct Cli {
    /// Existing repeated-batch archive directory to verify without modifying it.
    #[arg(long)]
    archive_dir: PathBuf,
    /// Optional exact repository revision expected by the caller.
    #[arg(long)]
    expected_revision: Option<String>,
    /// Reject exploratory evidence even when it is otherwise internally consistent.
    #[arg(long)]
    require_publication: bool,
}

fn main() -> ExitCode {
    let args = Cli::parse();
    match verify_batch_archive(
        &args.archive_dir,
        args.expected_revision.as_deref(),
        args.require_publication,
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
