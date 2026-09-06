use std::collections::BTreeSet;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use db_core::{DbError, ExperimentTrace, MAX_EXPERIMENT_BATCH_PAIRS};
use serde::Serialize;
use serde_json::{Map, Value};
use thiserror::Error;

const MAX_ARCHIVE_JSON_BYTES: u64 = 64 * 1024 * 1024;
const EXECUTION_PROTOCOL: &str = "fresh_counterbalanced_repeated_batch_v1";
const ATTEMPT_PROTOCOL: &str = "retain_all_requested_pairs_v1";
const FAILURE_PROTOCOL_V2: &str = "ordered_comparison_failure_sidecar_v2";
const PUBLICATION_PROTOCOL: &str = "publication_warm_v1";
const PUBLICATION_CACHE_POLICY: &str = "trace_induced_warm";
const PUBLICATION_DURABILITY_MODE: &str = "synced_single_operation";
const PUBLICATION_PAIR_ORDER_POLICY: &str = "pair_seed_low_bit_then_alternate";
const ENGINE_LAYOUT: &str = "pair-{pair_index:06}/repetition-{repetition_index}/{btree.db|lsm}";

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct VerificationSummary {
    pub valid: bool,
    pub format_version: u16,
    pub publication_admitted: bool,
    pub repository_revision: String,
    pub requested_pairs: u32,
    pub included_pairs: u32,
    pub failed_pairs: u32,
    pub excluded_pairs: u32,
    pub comparison_failure_sidecars: usize,
}

#[derive(Debug, Error)]
pub enum VerifyError {
    #[error("invalid batch archive: {0}")]
    Invalid(String),
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
    #[error("invalid experiment trace: {0}")]
    Trace(#[source] DbError),
}

pub fn verify_batch_archive(
    archive_dir: &Path,
    expected_revision: Option<&str>,
    require_publication: bool,
) -> Result<VerificationSummary, VerifyError> {
    let index = read_json(archive_dir, "index.json")?;
    let format_version = required_u16(&index, "format_version", "index.json")?;
    if matches!(format_version, 8 | 9) {
        return invalid(format!(
            "format v{format_version} is a frozen legacy failure-sidecar format without pair_index; it cannot be joined unambiguously to repeated batch ledger rows. Regenerate evidence as v10/v11"
        ));
    }
    if !matches!(format_version, 6 | 7 | 10 | 11) {
        return invalid(format!(
            "unsupported repeated batch archive format v{format_version}; supported verifiable formats are v6, v7, v10, and v11"
        ));
    }

    let publication = matches!(format_version, 7 | 11);
    let contextual_failures = matches!(format_version, 10 | 11);
    if require_publication && !publication {
        return invalid(format!(
            "caller requires publication evidence but archive format v{format_version} is exploratory"
        ));
    }

    let expected_files: &[&str] = if contextual_failures {
        &[
            "trace.json",
            "batch.json",
            "environment.json",
            "comparison-failures.json",
        ]
    } else {
        &["trace.json", "batch.json", "environment.json"]
    };
    verify_index(
        &index,
        format_version,
        publication,
        contextual_failures,
        expected_files,
    )?;
    verify_directory_entries(archive_dir, expected_files)?;

    let trace_json = read_json(archive_dir, "trace.json")?;
    let trace: ExperimentTrace =
        serde_json::from_value(trace_json.clone()).map_err(|source| VerifyError::Json {
            path: archive_dir.join("trace.json"),
            source,
        })?;
    trace.validate().map_err(VerifyError::Trace)?;

    let batch = read_json(archive_dir, "batch.json")?;
    let environment = read_json(archive_dir, "environment.json")?;
    let repository_revision = verify_environment(
        &environment,
        &index,
        &batch,
        format_version,
        publication,
        contextual_failures,
        expected_revision,
    )?;
    verify_trace_identity(&trace_json, &batch)?;
    let counts = verify_batch_ledger(&batch)?;

    let sidecar_count = if contextual_failures {
        let sidecars = read_json(archive_dir, "comparison-failures.json")?;
        verify_contextual_sidecars(&sidecars, &batch, counts.requested_pairs)?
    } else {
        0
    };

    if contextual_failures && sidecar_count == 0 {
        return invalid(format!(
            "format v{format_version} requires at least one contextual comparison failure sidecar"
        ));
    }

    Ok(VerificationSummary {
        valid: true,
        format_version,
        publication_admitted: publication,
        repository_revision,
        requested_pairs: counts.requested_pairs,
        included_pairs: counts.included_pairs,
        failed_pairs: counts.failed_pairs,
        excluded_pairs: counts.excluded_pairs,
        comparison_failure_sidecars: sidecar_count,
    })
}

fn verify_index(
    index: &Value,
    format_version: u16,
    publication: bool,
    contextual_failures: bool,
    expected_files: &[&str],
) -> Result<(), VerifyError> {
    require_equal_str(
        index,
        "execution_protocol",
        EXECUTION_PROTOCOL,
        "index.json",
    )?;
    require_equal_str(index, "attempt_protocol", ATTEMPT_PROTOCOL, "index.json")?;
    let files = required_array(index, "files", "index.json")?;
    if files.len() != expected_files.len() {
        return invalid(format!(
            "index.json files has {} entries; format v{format_version} requires {}",
            files.len(),
            expected_files.len()
        ));
    }
    for (position, (value, expected)) in files.iter().zip(expected_files).enumerate() {
        if value.as_str() != Some(expected) {
            return invalid(format!(
                "index.json files[{position}] must be {expected:?}; found {value}"
            ));
        }
    }

    let index_object = required_object(index, "index.json")?;
    if contextual_failures {
        require_equal_str(
            index,
            "comparison_failure_protocol",
            FAILURE_PROTOCOL_V2,
            "index.json",
        )?;
    } else if index_object.contains_key("comparison_failure_protocol") {
        return invalid(format!(
            "format v{format_version} index.json must not declare comparison_failure_protocol"
        ));
    }

    if publication {
        require_equal_str(
            index,
            "admission_protocol",
            PUBLICATION_PROTOCOL,
            "index.json",
        )?;
    } else if index_object.contains_key("admission_protocol") {
        return invalid(format!(
            "exploratory format v{format_version} index.json must not declare admission_protocol"
        ));
    }
    Ok(())
}

fn verify_directory_entries(archive_dir: &Path, indexed_files: &[&str]) -> Result<(), VerifyError> {
    let mut expected = BTreeSet::from(["index.json".to_owned()]);
    expected.extend(indexed_files.iter().map(|name| (*name).to_owned()));
    let read_dir = fs::read_dir(archive_dir).map_err(|source| VerifyError::Io {
        path: archive_dir.to_path_buf(),
        source,
    })?;
    let mut actual = BTreeSet::new();
    for entry in read_dir {
        let entry = entry.map_err(|source| VerifyError::Io {
            path: archive_dir.to_path_buf(),
            source,
        })?;
        let name = entry.file_name().into_string().map_err(|_| {
            VerifyError::Invalid("archive contains a non-UTF-8 file name".to_owned())
        })?;
        let metadata = fs::symlink_metadata(entry.path()).map_err(|source| VerifyError::Io {
            path: entry.path(),
            source,
        })?;
        if !metadata.file_type().is_file() {
            return invalid(format!(
                "archive entry {name:?} is not a regular file; symlinks/directories are not admitted"
            ));
        }
        actual.insert(name);
    }
    if actual != expected {
        return invalid(format!(
            "archive directory entries do not match immutable index contract: expected {expected:?}, found {actual:?}"
        ));
    }
    Ok(())
}

fn verify_environment(
    environment: &Value,
    index: &Value,
    batch: &Value,
    format_version: u16,
    publication: bool,
    contextual_failures: bool,
    expected_revision: Option<&str>,
) -> Result<String, VerifyError> {
    if required_u16(environment, "format_version", "environment.json")? != format_version {
        return invalid("environment.json format_version differs from index.json".to_owned());
    }
    require_equal_str(
        environment,
        "execution_protocol",
        EXECUTION_PROTOCOL,
        "environment.json",
    )?;
    require_equal_str(
        environment,
        "attempt_protocol",
        ATTEMPT_PROTOCOL,
        "environment.json",
    )?;
    require_equal_str(
        environment,
        "engine_layout",
        ENGINE_LAYOUT,
        "environment.json",
    )?;

    let revision = required_str(environment, "repository_revision", "environment.json")?;
    let index_revision = required_str(index, "repository_revision", "index.json")?;
    if revision != index_revision {
        return invalid(
            "repository_revision differs between environment.json and index.json".to_owned(),
        );
    }
    if let Some(expected) = expected_revision {
        if revision != expected {
            return invalid(format!(
                "repository_revision is {revision:?}; caller expected {expected:?}"
            ));
        }
    }

    let pair_seed = required_u64(environment, "pair_seed", "environment.json")?;
    let requested_pairs = required_u32(environment, "requested_pairs", "environment.json")?;
    if pair_seed != required_u64(batch, "pair_seed", "batch.json")? {
        return invalid("pair_seed differs between environment.json and batch.json".to_owned());
    }
    if requested_pairs != required_u32(batch, "requested_pairs", "batch.json")? {
        return invalid(
            "requested_pairs differs between environment.json and batch.json".to_owned(),
        );
    }

    let env_object = required_object(environment, "environment.json")?;
    if contextual_failures {
        require_equal_str(
            environment,
            "comparison_failure_protocol",
            FAILURE_PROTOCOL_V2,
            "environment.json",
        )?;
    } else if env_object.contains_key("comparison_failure_protocol") {
        return invalid(format!(
            "format v{format_version} environment.json must not declare comparison_failure_protocol"
        ));
    }

    if publication {
        let admission = required_field(environment, "publication_admission", "environment.json")?;
        verify_publication_admission(admission, requested_pairs)?;
        require_equal_str(environment, "cache_state", "warm", "environment.json")?;
    } else if env_object.contains_key("publication_admission") {
        return invalid(format!(
            "exploratory format v{format_version} environment.json must not contain publication_admission"
        ));
    }

    Ok(revision.to_owned())
}

fn verify_publication_admission(
    admission: &Value,
    requested_pairs: u32,
) -> Result<(), VerifyError> {
    let label = "environment.json publication_admission";
    require_equal_str(admission, "admission_protocol", PUBLICATION_PROTOCOL, label)?;
    require_equal_str(admission, "cache_policy", PUBLICATION_CACHE_POLICY, label)?;
    require_equal_str(admission, "cache_state", "warm", label)?;
    require_equal_str(
        admission,
        "durability_mode",
        PUBLICATION_DURABILITY_MODE,
        label,
    )?;
    require_equal_str(
        admission,
        "pair_order_policy",
        PUBLICATION_PAIR_ORDER_POLICY,
        label,
    )?;
    if required_u32(admission, "requested_pairs", label)? != requested_pairs {
        return invalid("publication_admission requested_pairs differs from batch".to_owned());
    }
    if required_u64(admission, "ordered_comparisons_per_included_pair", label)? != 2 {
        return invalid(
            "publication_admission ordered_comparisons_per_included_pair must be 2".to_owned(),
        );
    }
    for field in [
        "rust_target_triple",
        "host_label",
        "host_cpu",
        "host_memory",
        "storage_device",
        "filesystem",
        "mount_options",
        "optimization_flags",
        "analysis_script_version",
        "noise_budget",
    ] {
        if required_str(admission, field, label)?.trim().is_empty() {
            return invalid(format!("{label} {field} must be non-empty"));
        }
    }
    Ok(())
}

fn verify_trace_identity(trace: &Value, batch: &Value) -> Result<(), VerifyError> {
    let batch_trace = required_field(batch, "trace", "batch.json")?;
    if batch_trace != trace {
        return invalid("batch.json trace does not exactly match trace.json JSON value".to_owned());
    }
    Ok(())
}

#[derive(Debug, Clone, Copy)]
struct BatchCounts {
    requested_pairs: u32,
    included_pairs: u32,
    failed_pairs: u32,
    excluded_pairs: u32,
}

fn verify_batch_ledger(batch: &Value) -> Result<BatchCounts, VerifyError> {
    let requested_pairs = required_u32(batch, "requested_pairs", "batch.json")?;
    if requested_pairs == 0 || requested_pairs > MAX_EXPERIMENT_BATCH_PAIRS {
        return invalid(format!(
            "batch.json requested_pairs is {requested_pairs}; expected 1..={MAX_EXPERIMENT_BATCH_PAIRS}"
        ));
    }
    let pair_seed = required_u64(batch, "pair_seed", "batch.json")?;
    let declared = BatchCounts {
        requested_pairs,
        included_pairs: required_u32(batch, "included_pairs", "batch.json")?,
        failed_pairs: required_u32(batch, "failed_pairs", "batch.json")?,
        excluded_pairs: required_u32(batch, "excluded_pairs", "batch.json")?,
    };
    if declared
        .included_pairs
        .checked_add(declared.failed_pairs)
        .and_then(|value| value.checked_add(declared.excluded_pairs))
        != Some(requested_pairs)
    {
        return invalid(
            "batch.json declared disposition counts do not sum to requested_pairs".to_owned(),
        );
    }

    let attempts = required_array(batch, "attempts", "batch.json")?;
    if attempts.len() != requested_pairs as usize {
        return invalid(format!(
            "batch.json attempts has {} rows; requested_pairs is {requested_pairs}",
            attempts.len()
        ));
    }

    let mut actual_included = 0_u32;
    let mut actual_failed = 0_u32;
    let mut actual_excluded = 0_u32;
    for (index, attempt) in attempts.iter().enumerate() {
        verify_attempt(attempt, index as u32, pair_seed)?;
        match required_str(attempt, "disposition", "batch.json attempt")? {
            "included" => actual_included += 1,
            "failed" => actual_failed += 1,
            "excluded" => actual_excluded += 1,
            other => return invalid(format!("unknown batch attempt disposition {other:?}")),
        }
    }
    if (actual_included, actual_failed, actual_excluded)
        != (
            declared.included_pairs,
            declared.failed_pairs,
            declared.excluded_pairs,
        )
    {
        return invalid(format!(
            "batch.json declared counts ({}, {}, {}) differ from attempt rows ({actual_included}, {actual_failed}, {actual_excluded})",
            declared.included_pairs, declared.failed_pairs, declared.excluded_pairs
        ));
    }
    Ok(declared)
}

fn verify_attempt(attempt: &Value, pair_index: u32, pair_seed: u64) -> Result<(), VerifyError> {
    let context = required_field(attempt, "context", "batch.json attempt")?;
    if required_u32(context, "pair_index", "batch.json attempt context")? != pair_index {
        return invalid(format!(
            "batch attempt row {pair_index} does not carry matching context.pair_index"
        ));
    }
    let expected_order = expected_pair_order(pair_seed, pair_index);
    require_equal_str(
        context,
        "pair_order",
        expected_order,
        "batch.json attempt context",
    )?;

    let disposition = required_str(attempt, "disposition", "batch.json attempt")?;
    let report = required_field(attempt, "report", "batch.json attempt")?;
    let failure = required_field(attempt, "failure", "batch.json attempt")?;
    let exclusion_reason = required_field(attempt, "exclusion_reason", "batch.json attempt")?;
    match disposition {
        "included" => {
            if !report.is_object() || !failure.is_null() || !exclusion_reason.is_null() {
                return invalid(format!("included pair {pair_index} must have report only"));
            }
            require_equal_str(report, "pair_order", expected_order, "included pair report")?;
            verify_ordered_report(
                required_field(report, "first", "included pair report")?,
                first_execution_order(expected_order),
                "included pair first report",
            )?;
            verify_ordered_report(
                required_field(report, "second", "included pair report")?,
                second_execution_order(expected_order),
                "included pair second report",
            )?;
        }
        "failed" => {
            if !report.is_null() || !failure.is_object() || !exclusion_reason.is_null() {
                return invalid(format!("failed pair {pair_index} must have failure only"));
            }
            let stage = required_str(failure, "stage", "failed pair failure")?;
            if !matches!(stage, "engine_factory" | "comparison") {
                return invalid(format!(
                    "failed pair {pair_index} has unknown failure stage {stage:?}"
                ));
            }
            require_nonempty_str(failure, "message", "failed pair failure")?;
            require_error_class(failure, "class", "failed pair failure")?;
            if stage == "engine_factory" {
                match required_str(failure, "engine_role", "engine factory failure")? {
                    "left" | "right" => {}
                    other => return invalid(format!("invalid engine factory role {other:?}")),
                }
                let repetition =
                    required_u64(failure, "repetition_index", "engine factory failure")?;
                if repetition > 1 {
                    return invalid(format!(
                        "engine factory failure repetition_index {repetition} is outside 0..=1"
                    ));
                }
            }
        }
        "excluded" => {
            if !report.is_null() || !failure.is_null() {
                return invalid(format!(
                    "excluded pair {pair_index} must not contain report/failure"
                ));
            }
            if exclusion_reason
                .as_str()
                .is_none_or(|reason| reason.trim().is_empty())
            {
                return invalid(format!(
                    "excluded pair {pair_index} must retain a non-empty exclusion_reason"
                ));
            }
        }
        other => return invalid(format!("unknown batch attempt disposition {other:?}")),
    }
    Ok(())
}

fn verify_ordered_report(
    report: &Value,
    expected_order: &str,
    label: &str,
) -> Result<(), VerifyError> {
    require_equal_str(report, "execution_order", expected_order, label)
}

fn verify_contextual_sidecars(
    sidecars: &Value,
    batch: &Value,
    requested_pairs: u32,
) -> Result<usize, VerifyError> {
    let sidecars = sidecars.as_array().ok_or_else(|| {
        VerifyError::Invalid("comparison-failures.json must be an array".to_owned())
    })?;
    let attempts = required_array(batch, "attempts", "batch.json")?;
    let mut seen = BTreeSet::new();
    for (position, sidecar) in sidecars.iter().enumerate() {
        let context = required_field(sidecar, "context", "comparison failure sidecar")?;
        let failure = required_field(sidecar, "failure", "comparison failure sidecar")?;
        let pair_index = required_u32(context, "pair_index", "comparison failure context")?;
        if pair_index >= requested_pairs {
            return invalid(format!(
                "comparison sidecar {position} pair_index {pair_index} is outside requested range"
            ));
        }
        if !seen.insert(pair_index) {
            return invalid(format!(
                "comparison-failures.json contains more than one sidecar for pair {pair_index}"
            ));
        }
        let attempt = &attempts[pair_index as usize];
        if required_field(attempt, "context", "batch attempt")? != context {
            return invalid(format!(
                "comparison sidecar for pair {pair_index} does not exactly match batch attempt context"
            ));
        }
        if required_str(attempt, "disposition", "batch attempt")? != "failed" {
            return invalid(format!(
                "comparison sidecar pair {pair_index} does not reference a failed batch attempt"
            ));
        }
        let attempt_failure = required_field(attempt, "failure", "batch attempt")?;
        require_equal_str(
            attempt_failure,
            "stage",
            "comparison",
            "batch attempt failure",
        )?;

        let pair_order = required_str(context, "pair_order", "comparison failure context")?;
        if required_str(failure, "pair_order", "comparison failure")? != pair_order {
            return invalid(format!(
                "comparison sidecar pair {pair_index} nested pair_order differs from context"
            ));
        }
        let repetition = required_u64(failure, "repetition_index", "comparison failure")?;
        if repetition > 1 {
            return invalid(format!(
                "comparison sidecar pair {pair_index} repetition_index {repetition} is outside 0..=1"
            ));
        }
        let failure_object = required_object(failure, "comparison failure")?;
        match repetition {
            0 if failure_object.contains_key("completed_first") => {
                return invalid(format!(
                    "comparison sidecar pair {pair_index} repetition 0 must not contain completed_first"
                ));
            }
            1 if !failure_object
                .get("completed_first")
                .is_some_and(Value::is_object) =>
            {
                return invalid(format!(
                    "comparison sidecar pair {pair_index} repetition 1 must retain completed_first"
                ));
            }
            _ => {}
        }

        let ordered = required_field(failure, "ordered_failure", "comparison failure")?;
        let expected_execution = if repetition == 0 {
            first_execution_order(pair_order)
        } else {
            second_execution_order(pair_order)
        };
        require_equal_str(
            ordered,
            "execution_order",
            expected_execution,
            "ordered failure",
        )?;
        let error_class = require_error_class(ordered, "error_class", "ordered failure")?;
        let message = require_nonempty_str(ordered, "message", "ordered failure")?;
        if required_str(attempt_failure, "class", "batch attempt failure")? != error_class
            || required_str(attempt_failure, "message", "batch attempt failure")? != message
        {
            return invalid(format!(
                "comparison sidecar pair {pair_index} error identity differs from batch failure"
            ));
        }
        for field in ["left_operational_timing", "right_operational_timing"] {
            if !required_field(ordered, field, "ordered failure")?.is_object() {
                return invalid(format!(
                    "comparison sidecar pair {pair_index} {field} must be an object"
                ));
            }
        }
    }
    Ok(sidecars.len())
}

fn expected_pair_order(pair_seed: u64, pair_index: u32) -> &'static str {
    if ((pair_seed & 1) ^ u64::from(pair_index & 1)) == 0 {
        "left_then_right_first"
    } else {
        "right_then_left_first"
    }
}

fn first_execution_order(pair_order: &str) -> &'static str {
    if pair_order == "left_then_right_first" {
        "left_then_right"
    } else {
        "right_then_left"
    }
}

fn second_execution_order(pair_order: &str) -> &'static str {
    if pair_order == "left_then_right_first" {
        "right_then_left"
    } else {
        "left_then_right"
    }
}

fn read_json(archive_dir: &Path, name: &str) -> Result<Value, VerifyError> {
    let path = archive_dir.join(name);
    let metadata = fs::symlink_metadata(&path).map_err(|source| VerifyError::Io {
        path: path.clone(),
        source,
    })?;
    if !metadata.file_type().is_file() {
        return invalid(format!("{name} is not a regular file"));
    }
    if metadata.len() > MAX_ARCHIVE_JSON_BYTES {
        return invalid(format!(
            "{name} has {} bytes; maximum verified JSON size is {MAX_ARCHIVE_JSON_BYTES}",
            metadata.len()
        ));
    }
    let encoded = fs::read(&path).map_err(|source| VerifyError::Io {
        path: path.clone(),
        source,
    })?;
    serde_json::from_slice(&encoded).map_err(|source| VerifyError::Json { path, source })
}

fn required_object<'a>(
    value: &'a Value,
    label: &str,
) -> Result<&'a Map<String, Value>, VerifyError> {
    value
        .as_object()
        .ok_or_else(|| VerifyError::Invalid(format!("{label} must be a JSON object")))
}

fn required_field<'a>(
    value: &'a Value,
    field: &str,
    label: &str,
) -> Result<&'a Value, VerifyError> {
    required_object(value, label)?
        .get(field)
        .ok_or_else(|| VerifyError::Invalid(format!("{label} is missing required field {field:?}")))
}

fn required_array<'a>(
    value: &'a Value,
    field: &str,
    label: &str,
) -> Result<&'a Vec<Value>, VerifyError> {
    required_field(value, field, label)?
        .as_array()
        .ok_or_else(|| VerifyError::Invalid(format!("{label} {field} must be an array")))
}

fn required_str<'a>(value: &'a Value, field: &str, label: &str) -> Result<&'a str, VerifyError> {
    required_field(value, field, label)?
        .as_str()
        .ok_or_else(|| VerifyError::Invalid(format!("{label} {field} must be a string")))
}

fn require_nonempty_str<'a>(
    value: &'a Value,
    field: &str,
    label: &str,
) -> Result<&'a str, VerifyError> {
    let result = required_str(value, field, label)?;
    if result.trim().is_empty() {
        return invalid(format!("{label} {field} must be non-empty"));
    }
    Ok(result)
}

fn required_u64(value: &Value, field: &str, label: &str) -> Result<u64, VerifyError> {
    required_field(value, field, label)?
        .as_u64()
        .ok_or_else(|| VerifyError::Invalid(format!("{label} {field} must be an unsigned integer")))
}

fn required_u32(value: &Value, field: &str, label: &str) -> Result<u32, VerifyError> {
    let integer = required_u64(value, field, label)?;
    u32::try_from(integer)
        .map_err(|_| VerifyError::Invalid(format!("{label} {field} value {integer} exceeds u32")))
}

fn required_u16(value: &Value, field: &str, label: &str) -> Result<u16, VerifyError> {
    let integer = required_u64(value, field, label)?;
    u16::try_from(integer)
        .map_err(|_| VerifyError::Invalid(format!("{label} {field} value {integer} exceeds u16")))
}

fn require_equal_str(
    value: &Value,
    field: &str,
    expected: &str,
    label: &str,
) -> Result<(), VerifyError> {
    let found = required_str(value, field, label)?;
    if found != expected {
        return invalid(format!(
            "{label} {field} must be {expected:?}; found {found:?}"
        ));
    }
    Ok(())
}

fn require_error_class<'a>(
    value: &'a Value,
    field: &str,
    label: &str,
) -> Result<&'a str, VerifyError> {
    let class = required_str(value, field, label)?;
    if !matches!(
        class,
        "invalid_input" | "io" | "corruption" | "unsupported_version" | "poisoned"
    ) {
        return invalid(format!("{label} {field} has unknown error class {class:?}"));
    }
    Ok(class)
}

fn invalid<T>(message: String) -> Result<T, VerifyError> {
    Err(VerifyError::Invalid(message))
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::Path;

    use db_core::{generate_experiment_trace, ExperimentGeneratorConfig, ExperimentProfile};
    use serde_json::{json, Value};
    use tempfile::tempdir;

    use super::{verify_batch_archive, FAILURE_PROTOCOL_V2};

    #[test]
    fn verifies_v6_and_rejects_unindexed_extra_file() {
        let directory = tempdir().expect("temporary directory");
        write_archive(directory.path(), 6, false);
        let summary =
            verify_batch_archive(directory.path(), Some("abc123"), false).expect("verify v6");
        assert_eq!(summary.format_version, 6);
        assert_eq!(summary.excluded_pairs, 1);
        fs::write(directory.path().join("extra.txt"), b"unexpected").expect("write extra");
        assert!(
            verify_batch_archive(directory.path(), Some("abc123"), false)
                .expect_err("extra file must fail")
                .to_string()
                .contains("directory entries")
        );
    }

    #[test]
    fn verifies_v10_pair_join_and_rejects_tampered_pair_index() {
        let directory = tempdir().expect("temporary directory");
        write_archive(directory.path(), 10, true);
        let summary =
            verify_batch_archive(directory.path(), Some("abc123"), false).expect("verify v10");
        assert_eq!(summary.comparison_failure_sidecars, 1);

        let sidecar_path = directory.path().join("comparison-failures.json");
        let mut sidecars: Value =
            serde_json::from_slice(&fs::read(&sidecar_path).expect("read sidecar"))
                .expect("parse sidecar");
        sidecars[0]["context"]["pair_index"] = json!(1);
        fs::write(
            &sidecar_path,
            serde_json::to_vec_pretty(&sidecars).expect("encode tampered sidecar"),
        )
        .expect("write tampered sidecar");
        assert!(
            verify_batch_archive(directory.path(), Some("abc123"), false)
                .expect_err("tampered pair index must fail")
                .to_string()
                .contains("outside requested range")
        );
    }

    #[test]
    fn frozen_v8_is_explicitly_not_strongly_verifiable() {
        let directory = tempdir().expect("temporary directory");
        let index = json!({
            "format_version": 8,
            "repository_revision": "abc123",
            "execution_protocol": "fresh_counterbalanced_repeated_batch_v1",
            "attempt_protocol": "retain_all_requested_pairs_v1",
            "comparison_failure_protocol": "ordered_comparison_failure_sidecar_v1",
            "files": ["trace.json", "batch.json", "environment.json", "comparison-failures.json"]
        });
        write_json(directory.path(), "index.json", &index);
        let error = verify_batch_archive(directory.path(), Some("abc123"), false)
            .expect_err("v8 must fail closed");
        assert!(error.to_string().contains("without pair_index"));
    }

    fn write_archive(path: &Path, format_version: u16, with_failure: bool) {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 7,
            profile: ExperimentProfile::RandomWrite,
            operations: 1,
            key_space: 4,
            value_bytes: 4,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");
        let trace = serde_json::to_value(trace).expect("encode trace value");
        let (disposition, failure, exclusion_reason) = if with_failure {
            (
                "failed",
                json!({
                    "stage": "comparison",
                    "engine_role": null,
                    "repetition_index": null,
                    "class": "io",
                    "message": "synthetic compaction fault"
                }),
                Value::Null,
            )
        } else {
            ("excluded", Value::Null, json!("scheduled cooldown"))
        };
        let batch = json!({
            "trace": trace,
            "pair_seed": 0,
            "requested_pairs": 1,
            "included_pairs": 0,
            "failed_pairs": if with_failure { 1 } else { 0 },
            "excluded_pairs": if with_failure { 0 } else { 1 },
            "attempts": [{
                "context": {"pair_index": 0, "pair_order": "left_then_right_first"},
                "disposition": disposition,
                "report": null,
                "failure": failure,
                "exclusion_reason": exclusion_reason
            }]
        });
        let mut environment = json!({
            "format_version": format_version,
            "repository_revision": "abc123",
            "execution_protocol": "fresh_counterbalanced_repeated_batch_v1",
            "attempt_protocol": "retain_all_requested_pairs_v1",
            "pair_seed": 0,
            "requested_pairs": 1,
            "engine_layout": "pair-{pair_index:06}/repetition-{repetition_index}/{btree.db|lsm}",
            "cache_state": "warm"
        });
        let mut index = json!({
            "format_version": format_version,
            "repository_revision": "abc123",
            "execution_protocol": "fresh_counterbalanced_repeated_batch_v1",
            "attempt_protocol": "retain_all_requested_pairs_v1",
            "files": ["trace.json", "batch.json", "environment.json"]
        });
        if with_failure {
            environment["comparison_failure_protocol"] = json!(FAILURE_PROTOCOL_V2);
            index["comparison_failure_protocol"] = json!(FAILURE_PROTOCOL_V2);
            index["files"] = json!([
                "trace.json",
                "batch.json",
                "environment.json",
                "comparison-failures.json"
            ]);
            let sidecars = json!([{
                "context": {"pair_index": 0, "pair_order": "left_then_right_first"},
                "failure": {
                    "pair_order": "left_then_right_first",
                    "repetition_index": 0,
                    "ordered_failure": {
                        "execution_order": "left_then_right",
                        "error_class": "io",
                        "message": "synthetic compaction fault",
                        "left_operational_timing": {},
                        "right_operational_timing": {}
                    }
                }
            }]);
            write_json(path, "comparison-failures.json", &sidecars);
        }
        write_json(path, "trace.json", batch.get("trace").expect("batch trace"));
        write_json(path, "batch.json", &batch);
        write_json(path, "environment.json", &environment);
        write_json(path, "index.json", &index);
    }

    fn write_json(path: &Path, name: &str, value: &Value) {
        fs::write(
            path.join(name),
            serde_json::to_vec_pretty(value).expect("encode json"),
        )
        .expect("write json");
    }
}
