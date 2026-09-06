use std::collections::BTreeSet;
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufReader, BufWriter, Read, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use db_cli::batch_archive::{verify_batch_archive, VerificationSummary, VerifyError};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use thiserror::Error;

const BUNDLE_FORMAT_VERSION: u16 = 1;
const BUNDLE_PROTOCOL: &str = "verified_operational_timing_analysis_bundle_v1";
const MAX_BUNDLE_JSON_BYTES: u64 = 64 * 1024 * 1024;
const ANALYSIS_FILE: &str = "analysis.json";
const EVIDENCE_DIRECTORY: &str = "evidence";
const INDEX_FILE: &str = "index.json";

#[allow(dead_code)]
mod analyzer_impl {
    pub fn analyze_value(
        archive_dir: &std::path::Path,
        expected_revision: Option<&str>,
        require_publication: bool,
    ) -> Result<serde_json::Value, String> {
        let report = analyze(&Cli {
            archive_dir: archive_dir.to_path_buf(),
            expected_revision: expected_revision.map(str::to_owned),
            require_publication,
        })
        .map_err(|error| error.to_string())?;
        serde_json::to_value(report).map_err(|error| error.to_string())
    }

    pub const fn analysis_protocol() -> &'static str {
        ANALYSIS_PROTOCOL
    }

    pub const fn snapshot_protocol() -> &'static str {
        SNAPSHOT_PROTOCOL
    }

    include!("db-lab-batch-analyze.rs");
}

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-batch-analysis-bundle",
    version,
    about = "Create or verify a self-contained immutable timing-analysis bundle"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    /// Copy one verified batch archive, analyze the copy, and seal the report beside it.
    Create {
        /// Existing immutable repeated-batch archive directory.
        #[arg(long)]
        archive_dir: PathBuf,
        /// Fresh destination directory for the self-contained bundle.
        #[arg(long)]
        bundle_dir: PathBuf,
        /// Optional exact repository revision expected by the caller.
        #[arg(long)]
        expected_revision: Option<String>,
        /// Reject exploratory evidence before creating the bundle.
        #[arg(long)]
        require_publication: bool,
    },
    /// Re-run archive verification and descriptive analysis from bundled evidence.
    Verify {
        /// Existing analysis bundle directory.
        #[arg(long)]
        bundle_dir: PathBuf,
        /// Optional exact repository revision expected by the caller.
        #[arg(long)]
        expected_revision: Option<String>,
        /// Require publication-admitted bundled evidence.
        #[arg(long)]
        require_publication: bool,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
struct AnalysisBundleIndex {
    format_version: u16,
    bundle_protocol: String,
    analysis_protocol: String,
    snapshot_protocol: String,
    repository_revision: String,
    source_archive_format_version: u16,
    publication_admitted: bool,
    evidence_files: Vec<String>,
}

#[derive(Debug, Serialize)]
struct BundleVerificationSummary {
    valid: bool,
    bundle_format_version: u16,
    bundle_protocol: &'static str,
    repository_revision: String,
    source_archive_format_version: u16,
    publication_admitted: bool,
    evidence_files: usize,
}

#[derive(Debug, Error)]
enum BundleError {
    #[error(transparent)]
    Verify(#[from] VerifyError),
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("invalid JSON at {path}: {source}")]
    Json {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("analysis failed: {0}")]
    Analysis(String),
    #[error("invalid analysis bundle: {0}")]
    Invalid(String),
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let result = match cli.command {
        Command::Create {
            archive_dir,
            bundle_dir,
            expected_revision,
            require_publication,
        } => create_bundle(
            &archive_dir,
            &bundle_dir,
            expected_revision.as_deref(),
            require_publication,
        ),
        Command::Verify {
            bundle_dir,
            expected_revision,
            require_publication,
        } => verify_bundle(
            &bundle_dir,
            expected_revision.as_deref(),
            require_publication,
        ),
    };

    match result {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode bundle verification summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}

fn create_bundle(
    archive_dir: &Path,
    bundle_dir: &Path,
    expected_revision: Option<&str>,
    require_publication: bool,
) -> Result<BundleVerificationSummary, BundleError> {
    let source_dir = canonical_existing_real_directory(archive_dir, "source archive")?;
    let target_dir = canonical_fresh_target(bundle_dir)?;
    reject_nested_paths(&source_dir, &target_dir)?;

    let source_verification =
        verify_batch_archive(&source_dir, expected_revision, require_publication)?;

    fs::create_dir(&target_dir).map_err(|source| BundleError::Io {
        path: target_dir.clone(),
        source,
    })?;

    let result = (|| {
        let evidence_dir = target_dir.join(EVIDENCE_DIRECTORY);
        fs::create_dir(&evidence_dir).map_err(|source| BundleError::Io {
            path: evidence_dir.clone(),
            source,
        })?;

        let source_files = directory_entry_names(&source_dir, "source archive")?;
        for name in &source_files {
            copy_regular_file(&source_dir.join(name), &evidence_dir.join(name))?;
        }

        let evidence_verification =
            verify_batch_archive(&evidence_dir, expected_revision, require_publication)?;
        if evidence_verification != source_verification {
            return Err(BundleError::Invalid(
                "source verification summary changed while evidence was copied".to_owned(),
            ));
        }

        let analysis =
            analyzer_impl::analyze_value(&evidence_dir, expected_revision, require_publication)
                .map_err(BundleError::Analysis)?;

        let evidence_verification_after =
            verify_batch_archive(&evidence_dir, expected_revision, require_publication)?;
        if evidence_verification_after != evidence_verification {
            return Err(BundleError::Invalid(
                "bundled evidence changed while analysis was being computed".to_owned(),
            ));
        }
        let source_verification_after =
            verify_batch_archive(&source_dir, expected_revision, require_publication)?;
        if source_verification_after != source_verification {
            return Err(BundleError::Invalid(
                "source verification summary changed while the bundle was being created".to_owned(),
            ));
        }
        verify_directories_match(&source_dir, &evidence_dir)?;

        write_new_json(&target_dir.join(ANALYSIS_FILE), &analysis)?;
        let evidence_files: Vec<String> = source_files.into_iter().collect();
        let index = AnalysisBundleIndex {
            format_version: BUNDLE_FORMAT_VERSION,
            bundle_protocol: BUNDLE_PROTOCOL.to_owned(),
            analysis_protocol: analyzer_impl::analysis_protocol().to_owned(),
            snapshot_protocol: analyzer_impl::snapshot_protocol().to_owned(),
            repository_revision: evidence_verification.repository_revision.clone(),
            source_archive_format_version: evidence_verification.format_version,
            publication_admitted: evidence_verification.publication_admitted,
            evidence_files,
        };
        write_new_json(&target_dir.join(INDEX_FILE), &index)?;

        verify_bundle(
            &target_dir,
            Some(&source_verification.repository_revision),
            require_publication,
        )
    })();

    if result.is_err() {
        let _ = fs::remove_dir_all(&target_dir);
    }
    result
}

fn verify_bundle(
    bundle_dir: &Path,
    expected_revision: Option<&str>,
    require_publication: bool,
) -> Result<BundleVerificationSummary, BundleError> {
    let bundle_dir = canonical_existing_real_directory(bundle_dir, "analysis bundle")?;
    require_exact_bundle_entries(&bundle_dir)?;

    let index_path = bundle_dir.join(INDEX_FILE);
    let index: AnalysisBundleIndex =
        serde_json::from_value(read_json(&index_path)?).map_err(|source| BundleError::Json {
            path: index_path.clone(),
            source,
        })?;
    validate_index(&index, expected_revision)?;

    let evidence_dir = bundle_dir.join(EVIDENCE_DIRECTORY);
    let actual_evidence_files: Vec<String> =
        directory_entry_names(&evidence_dir, "bundled evidence")?
            .into_iter()
            .collect();
    if actual_evidence_files != index.evidence_files {
        return Err(BundleError::Invalid(format!(
            "bundled evidence file set differs from index.json: index {:?}, actual {:?}",
            index.evidence_files, actual_evidence_files
        )));
    }

    let effective_require_publication = require_publication || index.publication_admitted;
    let verification = verify_batch_archive(
        &evidence_dir,
        Some(&index.repository_revision),
        effective_require_publication,
    )?;
    if verification.format_version != index.source_archive_format_version {
        return Err(BundleError::Invalid(format!(
            "source archive format differs from index.json: index {}, verified {}",
            index.source_archive_format_version, verification.format_version
        )));
    }
    if verification.publication_admitted != index.publication_admitted {
        return Err(BundleError::Invalid(format!(
            "publication admission differs from index.json: index {}, verified {}",
            index.publication_admitted, verification.publication_admitted
        )));
    }

    let recomputed_analysis = analyzer_impl::analyze_value(
        &evidence_dir,
        Some(&index.repository_revision),
        effective_require_publication,
    )
    .map_err(BundleError::Analysis)?;
    let stored_analysis = read_json(&bundle_dir.join(ANALYSIS_FILE))?;
    if stored_analysis != recomputed_analysis {
        return Err(BundleError::Invalid(
            "analysis.json does not match analysis recomputed from bundled evidence".to_owned(),
        ));
    }

    Ok(summary_from_verification(
        &verification,
        index.evidence_files.len(),
    ))
}

fn validate_index(
    index: &AnalysisBundleIndex,
    expected_revision: Option<&str>,
) -> Result<(), BundleError> {
    if index.format_version != BUNDLE_FORMAT_VERSION {
        return Err(BundleError::Invalid(format!(
            "unsupported bundle format version {}; supported version is {BUNDLE_FORMAT_VERSION}",
            index.format_version
        )));
    }
    if index.bundle_protocol != BUNDLE_PROTOCOL {
        return Err(BundleError::Invalid(format!(
            "unsupported bundle protocol {:?}",
            index.bundle_protocol
        )));
    }
    if index.analysis_protocol != analyzer_impl::analysis_protocol() {
        return Err(BundleError::Invalid(format!(
            "analysis protocol differs from this analyzer: index {:?}, expected {:?}",
            index.analysis_protocol,
            analyzer_impl::analysis_protocol()
        )));
    }
    if index.snapshot_protocol != analyzer_impl::snapshot_protocol() {
        return Err(BundleError::Invalid(format!(
            "snapshot protocol differs from this analyzer: index {:?}, expected {:?}",
            index.snapshot_protocol,
            analyzer_impl::snapshot_protocol()
        )));
    }
    if let Some(expected_revision) = expected_revision {
        if index.repository_revision != expected_revision {
            return Err(BundleError::Invalid(format!(
                "bundle repository revision {:?} differs from expected revision {:?}",
                index.repository_revision, expected_revision
            )));
        }
    }
    if index.evidence_files.is_empty() {
        return Err(BundleError::Invalid(
            "index.json evidence_files must not be empty".to_owned(),
        ));
    }
    let unique: BTreeSet<&str> = index.evidence_files.iter().map(String::as_str).collect();
    if unique.len() != index.evidence_files.len() {
        return Err(BundleError::Invalid(
            "index.json evidence_files contains duplicate names".to_owned(),
        ));
    }
    Ok(())
}

fn summary_from_verification(
    verification: &VerificationSummary,
    evidence_files: usize,
) -> BundleVerificationSummary {
    BundleVerificationSummary {
        valid: true,
        bundle_format_version: BUNDLE_FORMAT_VERSION,
        bundle_protocol: BUNDLE_PROTOCOL,
        repository_revision: verification.repository_revision.clone(),
        source_archive_format_version: verification.format_version,
        publication_admitted: verification.publication_admitted,
        evidence_files,
    }
}

fn canonical_existing_real_directory(path: &Path, label: &str) -> Result<PathBuf, BundleError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| BundleError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if !metadata.file_type().is_dir() {
        return Err(BundleError::Invalid(format!(
            "{label} must be a real directory rather than a symlink or non-directory"
        )));
    }
    fs::canonicalize(path).map_err(|source| BundleError::Io {
        path: path.to_path_buf(),
        source,
    })
}

fn canonical_fresh_target(path: &Path) -> Result<PathBuf, BundleError> {
    if path.exists() {
        return Err(BundleError::Invalid(format!(
            "analysis bundle destination already exists: {}",
            path.display()
        )));
    }
    let name = path.file_name().ok_or_else(|| {
        BundleError::Invalid(format!(
            "analysis bundle destination has no final path component: {}",
            path.display()
        ))
    })?;
    let parent = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let parent = fs::canonicalize(parent).map_err(|source| BundleError::Io {
        path: parent.to_path_buf(),
        source,
    })?;
    Ok(parent.join(name))
}

fn reject_nested_paths(source_dir: &Path, target_dir: &Path) -> Result<(), BundleError> {
    if source_dir == target_dir
        || source_dir.starts_with(target_dir)
        || target_dir.starts_with(source_dir)
    {
        return Err(BundleError::Invalid(
            "source archive and analysis bundle must be distinct, non-nested paths".to_owned(),
        ));
    }
    Ok(())
}

fn require_exact_bundle_entries(bundle_dir: &Path) -> Result<(), BundleError> {
    let actual = directory_entry_names(bundle_dir, "analysis bundle")?;
    let expected: BTreeSet<String> = [ANALYSIS_FILE, EVIDENCE_DIRECTORY, INDEX_FILE]
        .into_iter()
        .map(str::to_owned)
        .collect();
    if actual != expected {
        return Err(BundleError::Invalid(format!(
            "analysis bundle entries must be exactly {expected:?}; found {actual:?}"
        )));
    }
    require_regular_file(&bundle_dir.join(ANALYSIS_FILE), "analysis.json")?;
    require_regular_file(&bundle_dir.join(INDEX_FILE), "index.json")?;
    let evidence = fs::symlink_metadata(bundle_dir.join(EVIDENCE_DIRECTORY)).map_err(|source| {
        BundleError::Io {
            path: bundle_dir.join(EVIDENCE_DIRECTORY),
            source,
        }
    })?;
    if !evidence.file_type().is_dir() {
        return Err(BundleError::Invalid(
            "bundle evidence must be a real directory rather than a symlink or non-directory"
                .to_owned(),
        ));
    }
    Ok(())
}

fn require_regular_file(path: &Path, label: &str) -> Result<(), BundleError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| BundleError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if !metadata.file_type().is_file() {
        return Err(BundleError::Invalid(format!(
            "{label} must be a regular file rather than a symlink or non-file"
        )));
    }
    if metadata.len() > MAX_BUNDLE_JSON_BYTES {
        return Err(BundleError::Invalid(format!(
            "{label} has {} bytes; maximum is {MAX_BUNDLE_JSON_BYTES}",
            metadata.len()
        )));
    }
    Ok(())
}

fn directory_entry_names(path: &Path, label: &str) -> Result<BTreeSet<String>, BundleError> {
    let read_dir = fs::read_dir(path).map_err(|source| BundleError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    let mut names = BTreeSet::new();
    for entry in read_dir {
        let entry = entry.map_err(|source| BundleError::Io {
            path: path.to_path_buf(),
            source,
        })?;
        let name = entry.file_name().into_string().map_err(|_| {
            BundleError::Invalid(format!("{label} contains a non-UTF-8 entry name"))
        })?;
        names.insert(name);
    }
    Ok(names)
}

fn copy_regular_file(source_path: &Path, target_path: &Path) -> Result<(), BundleError> {
    let metadata = fs::symlink_metadata(source_path).map_err(|source| BundleError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    if !metadata.file_type().is_file() {
        return Err(BundleError::Invalid(format!(
            "source archive entry {} is not a regular file",
            source_path.display()
        )));
    }
    if metadata.len() > MAX_BUNDLE_JSON_BYTES {
        return Err(BundleError::Invalid(format!(
            "source archive entry {} has {} bytes; maximum bundle file size is {MAX_BUNDLE_JSON_BYTES}",
            source_path.display(),
            metadata.len()
        )));
    }

    let source_file = File::open(source_path).map_err(|source| BundleError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    let target_file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(target_path)
        .map_err(|source| BundleError::Io {
            path: target_path.to_path_buf(),
            source,
        })?;
    let mut reader = BufReader::new(source_file);
    let mut writer = BufWriter::new(target_file);
    io::copy(&mut reader, &mut writer).map_err(|source| BundleError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    writer.flush().map_err(|source| BundleError::Io {
        path: target_path.to_path_buf(),
        source,
    })?;
    writer
        .get_ref()
        .sync_all()
        .map_err(|source| BundleError::Io {
            path: target_path.to_path_buf(),
            source,
        })?;
    Ok(())
}

fn verify_directories_match(source_dir: &Path, evidence_dir: &Path) -> Result<(), BundleError> {
    let source_entries = directory_entry_names(source_dir, "source archive")?;
    let evidence_entries = directory_entry_names(evidence_dir, "bundled evidence")?;
    if source_entries != evidence_entries {
        return Err(BundleError::Invalid(format!(
            "source archive entries changed during bundle creation: source {source_entries:?}, bundled {evidence_entries:?}"
        )));
    }
    for name in source_entries {
        compare_regular_files(&source_dir.join(&name), &evidence_dir.join(&name), &name)?;
    }
    Ok(())
}

fn compare_regular_files(
    source_path: &Path,
    evidence_path: &Path,
    name: &str,
) -> Result<(), BundleError> {
    let source_file = File::open(source_path).map_err(|source| BundleError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    let evidence_file = File::open(evidence_path).map_err(|source| BundleError::Io {
        path: evidence_path.to_path_buf(),
        source,
    })?;
    let source_metadata = source_file.metadata().map_err(|source| BundleError::Io {
        path: source_path.to_path_buf(),
        source,
    })?;
    let evidence_metadata = evidence_file.metadata().map_err(|source| BundleError::Io {
        path: evidence_path.to_path_buf(),
        source,
    })?;
    if !source_metadata.is_file() || !evidence_metadata.is_file() {
        return Err(BundleError::Invalid(format!(
            "archive entry {name:?} ceased to be a regular file during bundle creation"
        )));
    }
    if source_metadata.len() != evidence_metadata.len() {
        return Err(BundleError::Invalid(format!(
            "source archive entry {name:?} changed size during bundle creation"
        )));
    }

    let mut source_reader = BufReader::new(source_file);
    let mut evidence_reader = BufReader::new(evidence_file);
    let mut source_buffer = [0_u8; 64 * 1024];
    let mut evidence_buffer = [0_u8; 64 * 1024];
    loop {
        let source_read =
            source_reader
                .read(&mut source_buffer)
                .map_err(|source| BundleError::Io {
                    path: source_path.to_path_buf(),
                    source,
                })?;
        let evidence_read = evidence_reader
            .read(&mut evidence_buffer)
            .map_err(|source| BundleError::Io {
                path: evidence_path.to_path_buf(),
                source,
            })?;
        if source_read != evidence_read
            || source_buffer[..source_read] != evidence_buffer[..evidence_read]
        {
            return Err(BundleError::Invalid(format!(
                "source archive entry {name:?} changed content during bundle creation"
            )));
        }
        if source_read == 0 {
            break;
        }
    }
    Ok(())
}

fn read_json(path: &Path) -> Result<Value, BundleError> {
    require_regular_file(path, &path.display().to_string())?;
    let encoded = fs::read(path).map_err(|source| BundleError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    serde_json::from_slice(&encoded).map_err(|source| BundleError::Json {
        path: path.to_path_buf(),
        source,
    })
}

fn write_new_json(path: &Path, value: &impl Serialize) -> Result<(), BundleError> {
    let file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|source| BundleError::Io {
            path: path.to_path_buf(),
            source,
        })?;
    let mut writer = BufWriter::new(file);
    serde_json::to_writer_pretty(&mut writer, value).map_err(|source| BundleError::Json {
        path: path.to_path_buf(),
        source,
    })?;
    writer.write_all(b"\n").map_err(|source| BundleError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    writer.flush().map_err(|source| BundleError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    writer
        .get_ref()
        .sync_all()
        .map_err(|source| BundleError::Io {
            path: path.to_path_buf(),
            source,
        })?;
    Ok(())
}
