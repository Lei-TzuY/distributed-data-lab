use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use db_cli::generation_orphan::{
    inspect_generation_orphan, retire_generation_orphan, GenerationFileFingerprint,
    GenerationOrphanError,
};

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-log-generation-orphan",
    version,
    about = "Inspect or explicitly retire reserved abandoned append-log generation candidates"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    /// Read one reserved higher uncommitted generation and report exact retirement fingerprints.
    Inspect {
        #[arg(long)]
        directory: PathBuf,
        #[arg(long)]
        generation: u64,
    },
    /// Remove one confirmed-abandoned candidate/staging pair while retaining its durable reservation.
    Retire {
        #[arg(long)]
        directory: PathBuf,
        #[arg(long)]
        generation: u64,
        /// Exact authoritative generation returned by `inspect`.
        #[arg(long)]
        expected_authority: u64,
        /// Exact orphan-log byte length returned by `inspect`.
        #[arg(long)]
        expected_orphan_bytes: u64,
        /// Exact decimal orphan-log CRC-32/IEEE returned by `inspect`.
        #[arg(long)]
        expected_orphan_crc32: u32,
        /// Exact staging-marker byte length returned by `inspect`, when staging exists.
        #[arg(long)]
        expected_staging_bytes: Option<u64>,
        /// Exact decimal staging-marker CRC-32/IEEE returned by `inspect`, when staging exists.
        #[arg(long)]
        expected_staging_crc32: Option<u32>,
        /// Required operator attestation; the tool never infers candidate-builder liveness.
        #[arg(long)]
        confirm_generation_builder_stopped: bool,
    },
}

fn main() -> ExitCode {
    let result = match Cli::parse().command {
        Command::Inspect {
            directory,
            generation,
        } => inspect_generation_orphan(&directory, generation)
            .and_then(|summary| encode_json(&summary)),
        Command::Retire {
            directory,
            generation,
            expected_authority,
            expected_orphan_bytes,
            expected_orphan_crc32,
            expected_staging_bytes,
            expected_staging_crc32,
            confirm_generation_builder_stopped,
        } => match staging_fingerprint(expected_staging_bytes, expected_staging_crc32) {
            Ok(expected_staging_fingerprint) => retire_generation_orphan(
                &directory,
                generation,
                expected_authority,
                GenerationFileFingerprint {
                    bytes: expected_orphan_bytes,
                    crc32: expected_orphan_crc32,
                },
                expected_staging_fingerprint,
                confirm_generation_builder_stopped,
            )
            .and_then(|summary| encode_json(&summary)),
            Err(error) => Err(error),
        },
    };

    match result {
        Ok(encoded) => {
            println!("{encoded}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}

fn staging_fingerprint(
    bytes: Option<u64>,
    crc32: Option<u32>,
) -> Result<Option<GenerationFileFingerprint>, GenerationOrphanError> {
    match (bytes, crc32) {
        (None, None) => Ok(None),
        (Some(bytes), Some(crc32)) => Ok(Some(GenerationFileFingerprint { bytes, crc32 })),
        _ => Err(GenerationOrphanError::Invalid(
            "--expected-staging-bytes and --expected-staging-crc32 must be supplied together"
                .to_owned(),
        )),
    }
}

fn encode_json<T: serde::Serialize>(value: &T) -> Result<String, GenerationOrphanError> {
    serde_json::to_string_pretty(value).map_err(|error| {
        GenerationOrphanError::Invalid(format!(
            "failed to encode generation orphan summary: {error}"
        ))
    })
}
