use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use db_cli::generation_orphan::GenerationFileFingerprint;
use db_cli::generation_orphan_windows::retire_generation_orphan_windows;

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-orphan-retire-windows",
    version,
    about = "Durably retire a reserved abandoned append-log generation from the Windows authoritative namespace"
)]
struct Cli {
    #[arg(long)]
    directory: PathBuf,
    #[arg(long)]
    generation: u64,
    #[arg(long)]
    expected_authority: u64,
    #[arg(long)]
    expected_orphan_bytes: u64,
    #[arg(long)]
    expected_orphan_crc32: u32,
    #[arg(long)]
    expected_staging_bytes: Option<u64>,
    #[arg(long)]
    expected_staging_crc32: Option<u32>,
    #[arg(long)]
    confirm_generation_builder_stopped: bool,
}

fn main() -> ExitCode {
    let args = Cli::parse();
    let staging = match (args.expected_staging_bytes, args.expected_staging_crc32) {
        (None, None) => None,
        (Some(bytes), Some(crc32)) => Some(GenerationFileFingerprint { bytes, crc32 }),
        _ => {
            eprintln!(
                "error: expected staging bytes and CRC32 must either both be supplied or both omitted"
            );
            return ExitCode::from(1);
        }
    };

    match retire_generation_orphan_windows(
        &args.directory,
        args.generation,
        args.expected_authority,
        GenerationFileFingerprint {
            bytes: args.expected_orphan_bytes,
            crc32: args.expected_orphan_crc32,
        },
        staging,
        args.confirm_generation_builder_stopped,
    ) {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode Windows orphan retirement summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}
