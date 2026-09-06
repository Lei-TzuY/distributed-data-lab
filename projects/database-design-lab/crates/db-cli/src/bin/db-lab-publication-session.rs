use std::collections::BTreeSet;
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufReader, BufWriter, Read, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use db_cli::batch_archive::{verify_batch_archive, VerificationSummary, VerifyError};
use db_cli::host_preflight::{
    load_verified_host_preflight_snapshot, HostPreflightSnapshot, HostPreflightVerifyError,
    HOST_PREFLIGHT_PROTOCOL,
};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use thiserror::Error;

const V1_FORMAT_VERSION: u16 = 1;
const V2_FORMAT_VERSION: u16 = 2;
const V1_SESSION_PROTOCOL: &str = "controlled_publication_session_v1";
const V2_SESSION_PROTOCOL: &str = "controlled_publication_session_v2";
const PUBLICATION_PROTOCOL: &str = "publication_warm_v1";
const MAX_FILE_BYTES: u64 = 64 * 1024 * 1024;
const PREFLIGHT_FILE: &str = "host-preflight.json";
const POSTFLIGHT_FILE: &str = "host-postflight.json";
const EVIDENCE_DIR: &str = "evidence";
const INDEX_FILE: &str = "index.json";

#[derive(Debug, Parser)]
#[command(
    name = "db-lab-publication-session",
    version,
    about = "Create or verify a self-contained controlled publication-session artifact"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    /// Create a v2 session enclosed by passing pre-run and post-run host-control snapshots.
    Create {
        #[arg(long)]
        host_preflight: PathBuf,
        #[arg(long)]
        host_postflight: PathBuf,
        #[arg(long)]
        archive_dir: PathBuf,
        #[arg(long)]
        session_dir: PathBuf,
        #[arg(long)]
        expected_revision: Option<String>,
    },
    /// Verify a retained v1 or v2 session.
    Verify {
        #[arg(long)]
        session_dir: PathBuf,
        #[arg(long)]
        expected_revision: Option<String>,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct V1SessionIndex {
    format_version: u16,
    session_protocol: String,
    host_preflight_protocol: String,
    publication_admission_protocol: String,
    host_label: String,
    preflight_recorded_unix_seconds: u64,
    repository_revision: String,
    source_archive_format_version: u16,
    evidence_files: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct V2SessionIndex {
    format_version: u16,
    session_protocol: String,
    host_control_snapshot_protocol: String,
    publication_admission_protocol: String,
    host_label: String,
    preflight_recorded_unix_seconds: u64,
    archive_recorded_unix_seconds: u64,
    postflight_recorded_unix_seconds: u64,
    repository_revision: String,
    source_archive_format_version: u16,
    evidence_files: Vec<String>,
}

#[derive(Debug, Serialize)]
struct SessionSummary {
    valid: bool,
    session_format_version: u16,
    session_protocol: String,
    host_label: String,
    preflight_recorded_unix_seconds: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    archive_recorded_unix_seconds: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    postflight_recorded_unix_seconds: Option<u64>,
    repository_revision: String,
    source_archive_format_version: u16,
    evidence_files: usize,
}

#[derive(Debug, Error)]
enum SessionError {
    #[error(transparent)]
    Archive(#[from] VerifyError),
    #[error(transparent)]
    HostControl(#[from] HostPreflightVerifyError),
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
    #[error("invalid publication session: {0}")]
    Invalid(String),
}

fn main() -> ExitCode {
    let args = Cli::parse();
    let result = match args.command {
        Command::Create {
            host_preflight,
            host_postflight,
            archive_dir,
            session_dir,
            expected_revision,
        } => create_v2_session(
            &host_preflight,
            &host_postflight,
            &archive_dir,
            &session_dir,
            expected_revision.as_deref(),
        ),
        Command::Verify {
            session_dir,
            expected_revision,
        } => verify_session(&session_dir, expected_revision.as_deref()),
    };

    match result {
        Ok(summary) => match serde_json::to_string_pretty(&summary) {
            Ok(encoded) => {
                println!("{encoded}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                eprintln!("error: failed to encode session summary: {error}");
                ExitCode::from(1)
            }
        },
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(1)
        }
    }
}

fn create_v2_session(
    preflight_path: &Path,
    postflight_path: &Path,
    archive_path: &Path,
    session_path: &Path,
    expected_revision: Option<&str>,
) -> Result<SessionSummary, SessionError> {
    let source_preflight = canonical_regular_file(preflight_path, "host preflight")?;
    let source_postflight = canonical_regular_file(postflight_path, "host postflight")?;
    if source_preflight == source_postflight {
        return invalid("preflight and postflight must be distinct retained snapshot files");
    }
    let source_archive = canonical_directory(archive_path, "source archive")?;
    let target = fresh_target(session_path)?;
    reject_overlap(&source_archive, &target)?;

    let preflight = load_verified_host_preflight_snapshot(&source_preflight, None, true)?;
    let postflight = load_verified_host_preflight_snapshot(&source_postflight, None, true)?;
    let archive = verify_batch_archive(&source_archive, expected_revision, true)?;
    let (host_label, archive_recorded) = publication_binding(&source_archive, true)?;
    let archive_recorded = archive_recorded.expect("required publication recording time");
    match_host(&preflight, &host_label, "preflight")?;
    match_host(&postflight, &host_label, "postflight")?;
    validate_temporal_enclosure(
        preflight.recorded_unix_seconds,
        archive_recorded,
        postflight.recorded_unix_seconds,
    )?;

    fs::create_dir(&target).map_err(|source| io_error(&target, source))?;
    let result = create_v2_session_inner(
        &source_preflight,
        &source_postflight,
        &source_archive,
        &target,
        &preflight,
        &postflight,
        &archive,
        &host_label,
        archive_recorded,
    );
    if result.is_err() {
        let _ = fs::remove_dir_all(&target);
    }
    result
}

#[allow(clippy::too_many_arguments)]
fn create_v2_session_inner(
    source_preflight: &Path,
    source_postflight: &Path,
    source_archive: &Path,
    target: &Path,
    preflight: &HostPreflightSnapshot,
    postflight: &HostPreflightSnapshot,
    archive: &VerificationSummary,
    host_label: &str,
    archive_recorded: u64,
) -> Result<SessionSummary, SessionError> {
    let copied_preflight = target.join(PREFLIGHT_FILE);
    let copied_postflight = target.join(POSTFLIGHT_FILE);
    copy_new(source_preflight, &copied_preflight)?;
    copy_new(source_postflight, &copied_postflight)?;

    let evidence = target.join(EVIDENCE_DIR);
    fs::create_dir(&evidence).map_err(|source| io_error(&evidence, source))?;
    let source_files = entry_names(source_archive, "source archive")?;
    for name in &source_files {
        copy_new(&source_archive.join(name), &evidence.join(name))?;
    }

    let copied_preflight_value =
        load_verified_host_preflight_snapshot(&copied_preflight, Some(host_label), true)?;
    let copied_postflight_value =
        load_verified_host_preflight_snapshot(&copied_postflight, Some(host_label), true)?;
    if &copied_preflight_value != preflight {
        return invalid("preflight value changed while it was copied");
    }
    if &copied_postflight_value != postflight {
        return invalid("postflight value changed while it was copied");
    }
    let copied_archive = verify_batch_archive(&evidence, Some(&archive.repository_revision), true)?;
    if &copied_archive != archive {
        return invalid("archive verification summary changed while evidence was copied");
    }
    let (copied_host, copied_recorded) = publication_binding(&evidence, true)?;
    if copied_host != host_label || copied_recorded != Some(archive_recorded) {
        return invalid("publication host/time binding changed while evidence was copied");
    }
    validate_temporal_enclosure(
        copied_preflight_value.recorded_unix_seconds,
        archive_recorded,
        copied_postflight_value.recorded_unix_seconds,
    )?;

    let source_preflight_after =
        load_verified_host_preflight_snapshot(source_preflight, Some(host_label), true)?;
    let source_postflight_after =
        load_verified_host_preflight_snapshot(source_postflight, Some(host_label), true)?;
    if &source_preflight_after != preflight {
        return invalid("source preflight changed while the session was being created");
    }
    if &source_postflight_after != postflight {
        return invalid("source postflight changed while the session was being created");
    }
    let source_archive_after =
        verify_batch_archive(source_archive, Some(&archive.repository_revision), true)?;
    if &source_archive_after != archive {
        return invalid("source archive changed while the session was being created");
    }
    let (source_host_after, source_recorded_after) = publication_binding(source_archive, true)?;
    if source_host_after != host_label || source_recorded_after != Some(archive_recorded) {
        return invalid("source publication host/time binding changed during session creation");
    }
    validate_temporal_enclosure(
        source_preflight_after.recorded_unix_seconds,
        archive_recorded,
        source_postflight_after.recorded_unix_seconds,
    )?;

    compare_files(source_preflight, &copied_preflight, PREFLIGHT_FILE)?;
    compare_files(source_postflight, &copied_postflight, POSTFLIGHT_FILE)?;
    compare_directories(source_archive, &evidence)?;

    let index = V2SessionIndex {
        format_version: V2_FORMAT_VERSION,
        session_protocol: V2_SESSION_PROTOCOL.to_owned(),
        host_control_snapshot_protocol: HOST_PREFLIGHT_PROTOCOL.to_owned(),
        publication_admission_protocol: PUBLICATION_PROTOCOL.to_owned(),
        host_label: host_label.to_owned(),
        preflight_recorded_unix_seconds: preflight.recorded_unix_seconds,
        archive_recorded_unix_seconds: archive_recorded,
        postflight_recorded_unix_seconds: postflight.recorded_unix_seconds,
        repository_revision: archive.repository_revision.clone(),
        source_archive_format_version: archive.format_version,
        evidence_files: source_files.into_iter().collect(),
    };
    write_new_json(&target.join(INDEX_FILE), &index)?;
    verify_session(target, Some(&archive.repository_revision))
}

fn verify_session(
    session_path: &Path,
    expected_revision: Option<&str>,
) -> Result<SessionSummary, SessionError> {
    let session = canonical_directory(session_path, "publication session")?;
    let index_path = session.join(INDEX_FILE);
    let index_value = read_json(&index_path)?;
    let format_version = index_value
        .get("format_version")
        .and_then(Value::as_u64)
        .ok_or_else(|| {
            SessionError::Invalid("session index lacks integer format_version".to_owned())
        })?;

    match format_version {
        1 => {
            let index: V1SessionIndex =
                serde_json::from_value(index_value).map_err(|source| SessionError::Json {
                    path: index_path,
                    source,
                })?;
            verify_v1_session(&session, index, expected_revision)
        }
        2 => {
            let index: V2SessionIndex =
                serde_json::from_value(index_value).map_err(|source| SessionError::Json {
                    path: index_path,
                    source,
                })?;
            verify_v2_session(&session, index, expected_revision)
        }
        other => invalid(format!(
            "unsupported publication-session format {other}; supported formats are 1 and 2"
        )),
    }
}

fn verify_v1_session(
    session: &Path,
    index: V1SessionIndex,
    expected_revision: Option<&str>,
) -> Result<SessionSummary, SessionError> {
    require_session_layout(session, V1_FORMAT_VERSION)?;
    validate_v1_index(&index, expected_revision)?;

    let preflight = load_verified_host_preflight_snapshot(
        &session.join(PREFLIGHT_FILE),
        Some(&index.host_label),
        true,
    )?;
    if preflight.recorded_unix_seconds != index.preflight_recorded_unix_seconds {
        return invalid("host-preflight recording time differs from v1 session index");
    }

    let evidence = session.join(EVIDENCE_DIR);
    verify_evidence_file_set(&evidence, &index.evidence_files)?;
    let archive = verify_batch_archive(&evidence, Some(&index.repository_revision), true)?;
    if archive.format_version != index.source_archive_format_version {
        return invalid(format!(
            "source archive format differs from v1 index: expected v{}, verified v{}",
            index.source_archive_format_version, archive.format_version
        ));
    }
    let (archive_host, _) = publication_binding(&evidence, false)?;
    if archive_host != index.host_label {
        return invalid(format!(
            "publication archive host label {archive_host:?} differs from session host label {:?}",
            index.host_label
        ));
    }
    match_host(&preflight, &archive_host, "preflight")?;

    Ok(SessionSummary {
        valid: true,
        session_format_version: V1_FORMAT_VERSION,
        session_protocol: V1_SESSION_PROTOCOL.to_owned(),
        host_label: index.host_label,
        preflight_recorded_unix_seconds: index.preflight_recorded_unix_seconds,
        archive_recorded_unix_seconds: None,
        postflight_recorded_unix_seconds: None,
        repository_revision: archive.repository_revision,
        source_archive_format_version: archive.format_version,
        evidence_files: index.evidence_files.len(),
    })
}

fn verify_v2_session(
    session: &Path,
    index: V2SessionIndex,
    expected_revision: Option<&str>,
) -> Result<SessionSummary, SessionError> {
    require_session_layout(session, V2_FORMAT_VERSION)?;
    validate_v2_index(&index, expected_revision)?;

    let preflight = load_verified_host_preflight_snapshot(
        &session.join(PREFLIGHT_FILE),
        Some(&index.host_label),
        true,
    )?;
    let postflight = load_verified_host_preflight_snapshot(
        &session.join(POSTFLIGHT_FILE),
        Some(&index.host_label),
        true,
    )?;
    if preflight.recorded_unix_seconds != index.preflight_recorded_unix_seconds {
        return invalid("preflight recording time differs from v2 session index");
    }
    if postflight.recorded_unix_seconds != index.postflight_recorded_unix_seconds {
        return invalid("postflight recording time differs from v2 session index");
    }

    let evidence = session.join(EVIDENCE_DIR);
    verify_evidence_file_set(&evidence, &index.evidence_files)?;
    let archive = verify_batch_archive(&evidence, Some(&index.repository_revision), true)?;
    if archive.format_version != index.source_archive_format_version {
        return invalid(format!(
            "source archive format differs from v2 index: expected v{}, verified v{}",
            index.source_archive_format_version, archive.format_version
        ));
    }
    let (archive_host, archive_recorded) = publication_binding(&evidence, true)?;
    let archive_recorded = archive_recorded.expect("required publication recording time");
    if archive_host != index.host_label {
        return invalid(format!(
            "publication archive host label {archive_host:?} differs from session host label {:?}",
            index.host_label
        ));
    }
    if archive_recorded != index.archive_recorded_unix_seconds {
        return invalid("archive recording time differs from v2 session index");
    }
    match_host(&preflight, &archive_host, "preflight")?;
    match_host(&postflight, &archive_host, "postflight")?;
    validate_temporal_enclosure(
        preflight.recorded_unix_seconds,
        archive_recorded,
        postflight.recorded_unix_seconds,
    )?;

    Ok(SessionSummary {
        valid: true,
        session_format_version: V2_FORMAT_VERSION,
        session_protocol: V2_SESSION_PROTOCOL.to_owned(),
        host_label: index.host_label,
        preflight_recorded_unix_seconds: index.preflight_recorded_unix_seconds,
        archive_recorded_unix_seconds: Some(index.archive_recorded_unix_seconds),
        postflight_recorded_unix_seconds: Some(index.postflight_recorded_unix_seconds),
        repository_revision: archive.repository_revision,
        source_archive_format_version: archive.format_version,
        evidence_files: index.evidence_files.len(),
    })
}

fn validate_v1_index(
    index: &V1SessionIndex,
    expected_revision: Option<&str>,
) -> Result<(), SessionError> {
    if index.format_version != V1_FORMAT_VERSION || index.session_protocol != V1_SESSION_PROTOCOL {
        return invalid("v1 session index has an unsupported format/protocol pairing");
    }
    if index.host_preflight_protocol != HOST_PREFLIGHT_PROTOCOL {
        return invalid("v1 host-preflight protocol differs from the shared verifier");
    }
    validate_common_index(
        &index.publication_admission_protocol,
        &index.host_label,
        index.preflight_recorded_unix_seconds,
        &index.repository_revision,
        index.source_archive_format_version,
        &index.evidence_files,
        expected_revision,
    )
}

fn validate_v2_index(
    index: &V2SessionIndex,
    expected_revision: Option<&str>,
) -> Result<(), SessionError> {
    if index.format_version != V2_FORMAT_VERSION || index.session_protocol != V2_SESSION_PROTOCOL {
        return invalid("v2 session index has an unsupported format/protocol pairing");
    }
    if index.host_control_snapshot_protocol != HOST_PREFLIGHT_PROTOCOL {
        return invalid("v2 host-control snapshot protocol differs from the shared verifier");
    }
    validate_common_index(
        &index.publication_admission_protocol,
        &index.host_label,
        index.preflight_recorded_unix_seconds,
        &index.repository_revision,
        index.source_archive_format_version,
        &index.evidence_files,
        expected_revision,
    )?;
    if index.archive_recorded_unix_seconds == 0 || index.postflight_recorded_unix_seconds == 0 {
        return invalid("v2 archive/postflight recording times must be greater than zero");
    }
    validate_temporal_enclosure(
        index.preflight_recorded_unix_seconds,
        index.archive_recorded_unix_seconds,
        index.postflight_recorded_unix_seconds,
    )
}

#[allow(clippy::too_many_arguments)]
fn validate_common_index(
    publication_protocol: &str,
    host_label: &str,
    preflight_recorded: u64,
    repository_revision: &str,
    source_archive_format_version: u16,
    evidence_files: &[String],
    expected_revision: Option<&str>,
) -> Result<(), SessionError> {
    if publication_protocol != PUBLICATION_PROTOCOL {
        return invalid("publication admission protocol differs from publication_warm_v1");
    }
    if host_label.trim().is_empty() || host_label.trim() != host_label {
        return invalid("index host_label must be non-empty without surrounding whitespace");
    }
    if preflight_recorded == 0 {
        return invalid("index preflight recording time must be greater than zero");
    }
    if repository_revision.trim().is_empty() || repository_revision.trim() != repository_revision {
        return invalid(
            "index repository_revision must be non-empty without surrounding whitespace",
        );
    }
    if let Some(expected) = expected_revision {
        if repository_revision != expected {
            return invalid(format!(
                "session repository revision {repository_revision:?} differs from expected revision {expected:?}"
            ));
        }
    }
    if !matches!(source_archive_format_version, 7 | 11) {
        return invalid(format!(
            "publication session requires source archive format v7 or v11; found v{source_archive_format_version}"
        ));
    }
    validate_evidence_list(evidence_files)
}

fn validate_evidence_list(evidence_files: &[String]) -> Result<(), SessionError> {
    if evidence_files.is_empty() {
        return invalid("index evidence_files must not be empty");
    }
    let unique: BTreeSet<&str> = evidence_files.iter().map(String::as_str).collect();
    let sorted: Vec<&str> = unique.iter().copied().collect();
    let recorded: Vec<&str> = evidence_files.iter().map(String::as_str).collect();
    if unique.len() != recorded.len() || sorted != recorded {
        return invalid("index evidence_files must be sorted and duplicate-free");
    }
    Ok(())
}

fn validate_temporal_enclosure(
    preflight_recorded: u64,
    archive_recorded: u64,
    postflight_recorded: u64,
) -> Result<(), SessionError> {
    if preflight_recorded > archive_recorded {
        return invalid(format!(
            "preflight recording time {preflight_recorded} is after archive recording time {archive_recorded}"
        ));
    }
    if archive_recorded > postflight_recorded {
        return invalid(format!(
            "archive recording time {archive_recorded} is after postflight recording time {postflight_recorded}"
        ));
    }
    Ok(())
}

fn verify_evidence_file_set(evidence: &Path, expected: &[String]) -> Result<(), SessionError> {
    let actual: Vec<String> = entry_names(evidence, "publication evidence")?
        .into_iter()
        .collect();
    if actual != expected {
        return invalid(format!(
            "publication evidence file set differs from index: expected {expected:?}, found {actual:?}"
        ));
    }
    Ok(())
}

fn publication_binding(
    archive: &Path,
    require_recorded_time: bool,
) -> Result<(String, Option<u64>), SessionError> {
    let environment = read_json(&archive.join("environment.json"))?;
    let host_label = environment
        .get("publication_admission")
        .and_then(Value::as_object)
        .and_then(|admission| admission.get("host_label"))
        .and_then(Value::as_str)
        .map(str::to_owned)
        .ok_or_else(|| {
            SessionError::Invalid(
                "verified publication archive is missing publication_admission.host_label"
                    .to_owned(),
            )
        })?;
    let recorded = environment
        .get("recorded_unix_seconds")
        .and_then(Value::as_u64);
    if require_recorded_time && recorded.is_none_or(|value| value == 0) {
        return invalid("v2 publication session requires environment.recorded_unix_seconds > 0");
    }
    Ok((host_label, recorded))
}

fn match_host(
    snapshot: &HostPreflightSnapshot,
    archive_host: &str,
    role: &str,
) -> Result<(), SessionError> {
    if snapshot.host_label != archive_host {
        return invalid(format!(
            "{role} host label {:?} differs from publication archive host label {archive_host:?}",
            snapshot.host_label
        ));
    }
    Ok(())
}

fn canonical_regular_file(path: &Path, label: &str) -> Result<PathBuf, SessionError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_file() {
        return invalid(format!(
            "{label} must be a regular file rather than a symlink or non-file"
        ));
    }
    fs::canonicalize(path).map_err(|source| io_error(path, source))
}

fn canonical_directory(path: &Path, label: &str) -> Result<PathBuf, SessionError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_dir() {
        return invalid(format!(
            "{label} must be a real directory rather than a symlink or non-directory"
        ));
    }
    fs::canonicalize(path).map_err(|source| io_error(path, source))
}

fn fresh_target(path: &Path) -> Result<PathBuf, SessionError> {
    match fs::symlink_metadata(path) {
        Ok(_) => {
            return invalid(format!(
                "session destination already exists: {}",
                path.display()
            ))
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(source) => return Err(io_error(path, source)),
    }
    let name = path.file_name().ok_or_else(|| {
        SessionError::Invalid(format!(
            "session destination has no final path component: {}",
            path.display()
        ))
    })?;
    let parent = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let parent = fs::canonicalize(parent).map_err(|source| io_error(parent, source))?;
    Ok(parent.join(name))
}

fn reject_overlap(source: &Path, target: &Path) -> Result<(), SessionError> {
    if source == target || source.starts_with(target) || target.starts_with(source) {
        return invalid(
            "source archive and publication session must be distinct, non-nested paths",
        );
    }
    Ok(())
}

fn require_session_layout(session: &Path, format_version: u16) -> Result<(), SessionError> {
    let actual = entry_names(session, "publication session")?;
    let mut expected: BTreeSet<String> = [PREFLIGHT_FILE, EVIDENCE_DIR, INDEX_FILE]
        .into_iter()
        .map(str::to_owned)
        .collect();
    if format_version == V2_FORMAT_VERSION {
        expected.insert(POSTFLIGHT_FILE.to_owned());
    }
    if actual != expected {
        return invalid(format!(
            "publication-session entries must be exactly {expected:?}; found {actual:?}"
        ));
    }
    require_regular(&session.join(PREFLIGHT_FILE), PREFLIGHT_FILE)?;
    if format_version == V2_FORMAT_VERSION {
        require_regular(&session.join(POSTFLIGHT_FILE), POSTFLIGHT_FILE)?;
    }
    require_regular(&session.join(INDEX_FILE), INDEX_FILE)?;
    let evidence = session.join(EVIDENCE_DIR);
    let metadata = fs::symlink_metadata(&evidence).map_err(|source| io_error(&evidence, source))?;
    if !metadata.file_type().is_dir() {
        return invalid("publication-session evidence must be a real directory");
    }
    Ok(())
}

fn require_regular(path: &Path, label: &str) -> Result<(), SessionError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| io_error(path, source))?;
    if !metadata.file_type().is_file() {
        return invalid(format!("{label} must be a regular file"));
    }
    if metadata.len() > MAX_FILE_BYTES {
        return invalid(format!(
            "{label} has {} bytes; maximum is {MAX_FILE_BYTES}",
            metadata.len()
        ));
    }
    Ok(())
}

fn entry_names(path: &Path, label: &str) -> Result<BTreeSet<String>, SessionError> {
    let entries = fs::read_dir(path).map_err(|source| io_error(path, source))?;
    let mut names = BTreeSet::new();
    for entry in entries {
        let entry = entry.map_err(|source| io_error(path, source))?;
        let name = entry.file_name().into_string().map_err(|_| {
            SessionError::Invalid(format!("{label} contains a non-UTF-8 entry name"))
        })?;
        names.insert(name);
    }
    Ok(names)
}

fn copy_new(source: &Path, target: &Path) -> Result<(), SessionError> {
    let metadata = fs::symlink_metadata(source).map_err(|error| io_error(source, error))?;
    if !metadata.file_type().is_file() {
        return invalid(format!(
            "source entry {} is not a regular file",
            source.display()
        ));
    }
    if metadata.len() > MAX_FILE_BYTES {
        return invalid(format!(
            "source entry {} exceeds size limit",
            source.display()
        ));
    }

    let source_file = File::open(source).map_err(|error| io_error(source, error))?;
    let target_file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(target)
        .map_err(|error| io_error(target, error))?;
    let mut reader = BufReader::new(source_file);
    let mut writer = BufWriter::new(target_file);
    let copied = io::copy(&mut reader.by_ref().take(MAX_FILE_BYTES + 1), &mut writer)
        .map_err(|error| io_error(source, error))?;
    if copied > MAX_FILE_BYTES {
        return invalid(format!(
            "source entry {} grew beyond size limit",
            source.display()
        ));
    }
    writer.flush().map_err(|error| io_error(target, error))?;
    writer
        .get_ref()
        .sync_all()
        .map_err(|error| io_error(target, error))?;
    Ok(())
}

fn compare_directories(source: &Path, copy: &Path) -> Result<(), SessionError> {
    let source_names = entry_names(source, "source archive")?;
    let copy_names = entry_names(copy, "publication evidence")?;
    if source_names != copy_names {
        return invalid("source archive file set changed while the session was being created");
    }
    for name in source_names {
        compare_files(&source.join(&name), &copy.join(&name), &name)?;
    }
    Ok(())
}

fn compare_files(source: &Path, copy: &Path, label: &str) -> Result<(), SessionError> {
    let source_file = File::open(source).map_err(|error| io_error(source, error))?;
    let copy_file = File::open(copy).map_err(|error| io_error(copy, error))?;
    let source_metadata = source_file
        .metadata()
        .map_err(|error| io_error(source, error))?;
    let copy_metadata = copy_file
        .metadata()
        .map_err(|error| io_error(copy, error))?;
    if !source_metadata.is_file()
        || !copy_metadata.is_file()
        || source_metadata.len() != copy_metadata.len()
    {
        return invalid(format!("source/copy metadata differs for {label:?}"));
    }

    let mut left = BufReader::new(source_file);
    let mut right = BufReader::new(copy_file);
    let mut left_buffer = [0_u8; 64 * 1024];
    let mut right_buffer = [0_u8; 64 * 1024];
    loop {
        let left_len = left
            .read(&mut left_buffer)
            .map_err(|error| io_error(source, error))?;
        let right_len = right
            .read(&mut right_buffer)
            .map_err(|error| io_error(copy, error))?;
        if left_len != right_len || left_buffer[..left_len] != right_buffer[..right_len] {
            return invalid(format!("source/copy bytes differ for {label:?}"));
        }
        if left_len == 0 {
            return Ok(());
        }
    }
}

fn read_json(path: &Path) -> Result<Value, SessionError> {
    require_regular(path, &path.display().to_string())?;
    let file = File::open(path).map_err(|error| io_error(path, error))?;
    let mut encoded = Vec::new();
    file.take(MAX_FILE_BYTES + 1)
        .read_to_end(&mut encoded)
        .map_err(|error| io_error(path, error))?;
    if encoded.len() as u64 > MAX_FILE_BYTES {
        return invalid(format!("JSON file {} exceeds size limit", path.display()));
    }
    serde_json::from_slice(&encoded).map_err(|source| SessionError::Json {
        path: path.to_path_buf(),
        source,
    })
}

fn write_new_json(path: &Path, value: &impl Serialize) -> Result<(), SessionError> {
    let file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|error| io_error(path, error))?;
    let mut writer = BufWriter::new(file);
    serde_json::to_writer_pretty(&mut writer, value).map_err(|source| SessionError::Json {
        path: path.to_path_buf(),
        source,
    })?;
    writer
        .write_all(b"\n")
        .map_err(|error| io_error(path, error))?;
    writer.flush().map_err(|error| io_error(path, error))?;
    writer
        .get_ref()
        .sync_all()
        .map_err(|error| io_error(path, error))?;
    Ok(())
}

fn io_error(path: &Path, source: io::Error) -> SessionError {
    SessionError::Io {
        path: path.to_path_buf(),
        source,
    }
}

fn invalid<T>(message: impl Into<String>) -> Result<T, SessionError> {
    Err(SessionError::Invalid(message.into()))
}
