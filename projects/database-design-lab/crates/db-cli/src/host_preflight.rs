use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use thiserror::Error;

pub const HOST_PREFLIGHT_PROTOCOL: &str = "linux_controlled_host_preflight_v1";
pub const HOST_PREFLIGHT_EXPECTED_GOVERNOR: &str = "performance";
pub const HOST_PREFLIGHT_MAX_CPUS: usize = 4096;
pub const HOST_PREFLIGHT_MAX_CPU_ID: u32 = 1_048_575;
pub const HOST_PREFLIGHT_MAX_TEXT_BYTES: usize = 4096;
pub const HOST_PREFLIGHT_MAX_SNAPSHOT_BYTES: usize = 2 * 1024 * 1024;
pub const HOST_PREFLIGHT_LIMITATIONS: [&str; 3] = [
    "operator attestations are recorded statements, not independently verified facts",
    "thermal equilibrium and storage/controller cache state are not portable kernel observations in this protocol",
    "a passing snapshot is a prerequisite for controlled collection, not a performance result or regression threshold",
];

const INTEL_NO_TURBO_PATH: &str = "/sys/devices/system/cpu/intel_pstate/no_turbo";
const GENERIC_BOOST_PATH: &str = "/sys/devices/system/cpu/cpufreq/boost";
const MAX_DESCRIPTIVE_TEXT_BYTES: usize = 64 * 1024;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HostPreflightSnapshot {
    pub protocol: String,
    pub recorded_unix_seconds: u64,
    pub host_label: String,
    pub passed: bool,
    pub expected: HostPreflightExpectedControls,
    pub observation: HostPreflightObservation,
    pub operator_attestations: HostPreflightOperatorAttestations,
    pub violations: Vec<String>,
    pub limitations: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HostPreflightExpectedControls {
    pub process_cpu_affinity: Vec<u32>,
    pub scaling_governor: String,
    pub turbo_disabled: bool,
    pub max_load_per_cpu: f64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HostPreflightObservation {
    pub target_os: String,
    pub target_arch: String,
    pub kernel_release: Option<String>,
    pub cpu_model: Option<String>,
    pub process_allowed_cpus: Option<Vec<u32>>,
    pub online_cpus: Option<Vec<u32>>,
    pub governors: BTreeMap<u32, Option<String>>,
    pub turbo: HostPreflightTurboObservation,
    pub load_one: Option<f64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HostPreflightTurboObservation {
    pub interface: Option<String>,
    pub raw_value: Option<String>,
    pub disabled: Option<bool>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HostPreflightOperatorAttestations {
    pub thermal_control: String,
    pub background_load_control: String,
    pub storage_cache_control: String,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct HostPreflightVerificationSummary {
    pub valid: bool,
    pub protocol: String,
    pub passed: bool,
    pub recorded_unix_seconds: u64,
    pub host_label: String,
    pub process_cpu_affinity: Vec<u32>,
    pub max_load_per_cpu: f64,
    pub observed_load_one: Option<f64>,
    pub violations: usize,
}

#[derive(Debug, Error)]
pub enum HostPreflightVerifyError {
    #[error("I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("invalid host-preflight JSON at {path}: {source}")]
    Json {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("invalid host-preflight snapshot: {0}")]
    Invalid(String),
}

pub fn verify_host_preflight_snapshot(
    path: &Path,
    expected_host_label: Option<&str>,
    require_passed: bool,
) -> Result<HostPreflightVerificationSummary, HostPreflightVerifyError> {
    let snapshot =
        load_verified_host_preflight_snapshot(path, expected_host_label, require_passed)?;
    Ok(HostPreflightVerificationSummary {
        valid: true,
        protocol: snapshot.protocol.clone(),
        passed: snapshot.passed,
        recorded_unix_seconds: snapshot.recorded_unix_seconds,
        host_label: snapshot.host_label.clone(),
        process_cpu_affinity: snapshot.expected.process_cpu_affinity.clone(),
        max_load_per_cpu: snapshot.expected.max_load_per_cpu,
        observed_load_one: snapshot.observation.load_one,
        violations: snapshot.violations.len(),
    })
}

pub fn load_verified_host_preflight_snapshot(
    path: &Path,
    expected_host_label: Option<&str>,
    require_passed: bool,
) -> Result<HostPreflightSnapshot, HostPreflightVerifyError> {
    let snapshot = read_snapshot(path)?;
    validate_host_preflight_snapshot(&snapshot, expected_host_label, require_passed)?;
    Ok(snapshot)
}

pub fn validate_host_preflight_snapshot(
    snapshot: &HostPreflightSnapshot,
    expected_host_label: Option<&str>,
    require_passed: bool,
) -> Result<(), HostPreflightVerifyError> {
    if snapshot.protocol != HOST_PREFLIGHT_PROTOCOL {
        return Err(invalid(format!(
            "unsupported protocol {:?}; expected {HOST_PREFLIGHT_PROTOCOL:?}",
            snapshot.protocol
        )));
    }
    if snapshot.recorded_unix_seconds == 0 {
        return Err(invalid("recorded_unix_seconds must be greater than zero"));
    }
    validate_trimmed_text(
        "host_label",
        &snapshot.host_label,
        HOST_PREFLIGHT_MAX_TEXT_BYTES,
    )?;
    if let Some(expected) = expected_host_label {
        if snapshot.host_label != expected {
            return Err(invalid(format!(
                "host label {:?} differs from expected label {:?}",
                snapshot.host_label, expected
            )));
        }
    }

    let expected_cpus = validate_cpu_vector(
        "expected.process_cpu_affinity",
        &snapshot.expected.process_cpu_affinity,
    )?;
    if snapshot.expected.scaling_governor != HOST_PREFLIGHT_EXPECTED_GOVERNOR {
        return Err(invalid(format!(
            "expected.scaling_governor must be {HOST_PREFLIGHT_EXPECTED_GOVERNOR:?}; found {:?}",
            snapshot.expected.scaling_governor
        )));
    }
    if !snapshot.expected.turbo_disabled {
        return Err(invalid(
            "expected.turbo_disabled must be true for protocol v1",
        ));
    }
    if !snapshot.expected.max_load_per_cpu.is_finite() || snapshot.expected.max_load_per_cpu < 0.0 {
        return Err(invalid(
            "expected.max_load_per_cpu must be finite and non-negative",
        ));
    }

    if snapshot.observation.target_os != "linux" {
        return Err(invalid(format!(
            "observation.target_os must be \"linux\"; found {:?}",
            snapshot.observation.target_os
        )));
    }
    validate_trimmed_text(
        "observation.target_arch",
        &snapshot.observation.target_arch,
        128,
    )?;
    validate_optional_trimmed_text(
        "observation.kernel_release",
        snapshot.observation.kernel_release.as_deref(),
        MAX_DESCRIPTIVE_TEXT_BYTES,
    )?;
    validate_optional_trimmed_text(
        "observation.cpu_model",
        snapshot.observation.cpu_model.as_deref(),
        MAX_DESCRIPTIVE_TEXT_BYTES,
    )?;

    if let Some(actual) = snapshot.observation.process_allowed_cpus.as_ref() {
        validate_cpu_vector("observation.process_allowed_cpus", actual)?;
    }
    if let Some(online) = snapshot.observation.online_cpus.as_ref() {
        validate_cpu_vector("observation.online_cpus", online)?;
    }

    let governor_keys: BTreeSet<u32> = snapshot.observation.governors.keys().copied().collect();
    if governor_keys != expected_cpus {
        return Err(invalid(format!(
            "observation.governors keys {governor_keys:?} differ from expected CPU set {expected_cpus:?}"
        )));
    }
    for (cpu, governor) in &snapshot.observation.governors {
        if let Some(governor) = governor {
            validate_trimmed_text(&format!("observation.governors[{cpu}]"), governor, 128)?;
        }
    }

    validate_turbo_observation(&snapshot.observation.turbo)?;
    if let Some(load_one) = snapshot.observation.load_one {
        if !load_one.is_finite() || load_one < 0.0 {
            return Err(invalid(
                "observation.load_one must be finite and non-negative when present",
            ));
        }
    }

    validate_trimmed_text(
        "operator_attestations.thermal_control",
        &snapshot.operator_attestations.thermal_control,
        HOST_PREFLIGHT_MAX_TEXT_BYTES,
    )?;
    validate_trimmed_text(
        "operator_attestations.background_load_control",
        &snapshot.operator_attestations.background_load_control,
        HOST_PREFLIGHT_MAX_TEXT_BYTES,
    )?;
    validate_trimmed_text(
        "operator_attestations.storage_cache_control",
        &snapshot.operator_attestations.storage_cache_control,
        HOST_PREFLIGHT_MAX_TEXT_BYTES,
    )?;

    let limitations_match = snapshot
        .limitations
        .iter()
        .map(String::as_str)
        .eq(HOST_PREFLIGHT_LIMITATIONS);
    if !limitations_match {
        return Err(invalid(format!(
            "limitations differ from the frozen v1 protocol: found {:?}",
            snapshot.limitations
        )));
    }

    for (index, violation) in snapshot.violations.iter().enumerate() {
        validate_trimmed_text(
            &format!("violations[{index}]"),
            violation,
            HOST_PREFLIGHT_MAX_TEXT_BYTES,
        )?;
    }
    let recomputed = recompute_violations(snapshot);
    if snapshot.violations != recomputed {
        return Err(invalid(format!(
            "violations do not match recomputed hard-control failures: stored {:?}, recomputed {recomputed:?}",
            snapshot.violations
        )));
    }
    let recomputed_passed = recomputed.is_empty();
    if snapshot.passed != recomputed_passed {
        return Err(invalid(format!(
            "passed={} disagrees with recomputed hard-control result {recomputed_passed}",
            snapshot.passed
        )));
    }
    if require_passed && !snapshot.passed {
        return Err(invalid(
            "snapshot is internally valid but records passed=false",
        ));
    }
    Ok(())
}

fn invalid(message: impl Into<String>) -> HostPreflightVerifyError {
    HostPreflightVerifyError::Invalid(message.into())
}

fn read_snapshot(path: &Path) -> Result<HostPreflightSnapshot, HostPreflightVerifyError> {
    let metadata = fs::symlink_metadata(path).map_err(|source| HostPreflightVerifyError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if !metadata.file_type().is_file() {
        return Err(invalid(format!(
            "snapshot path must be a regular file rather than a symlink or non-file: {}",
            path.display()
        )));
    }
    if metadata.len() > HOST_PREFLIGHT_MAX_SNAPSHOT_BYTES as u64 {
        return Err(invalid(format!(
            "snapshot has {} bytes; maximum is {HOST_PREFLIGHT_MAX_SNAPSHOT_BYTES}",
            metadata.len()
        )));
    }

    let file = File::open(path).map_err(|source| HostPreflightVerifyError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    let mut encoded = Vec::new();
    file.take(HOST_PREFLIGHT_MAX_SNAPSHOT_BYTES as u64 + 1)
        .read_to_end(&mut encoded)
        .map_err(|source| HostPreflightVerifyError::Io {
            path: path.to_path_buf(),
            source,
        })?;
    if encoded.len() > HOST_PREFLIGHT_MAX_SNAPSHOT_BYTES {
        return Err(invalid(format!(
            "snapshot exceeds maximum {HOST_PREFLIGHT_MAX_SNAPSHOT_BYTES} bytes"
        )));
    }
    serde_json::from_slice(&encoded).map_err(|source| HostPreflightVerifyError::Json {
        path: path.to_path_buf(),
        source,
    })
}

fn validate_cpu_vector(
    label: &str,
    cpus: &[u32],
) -> Result<BTreeSet<u32>, HostPreflightVerifyError> {
    if cpus.is_empty() {
        return Err(invalid(format!("{label} must not be empty")));
    }
    if cpus.len() > HOST_PREFLIGHT_MAX_CPUS {
        return Err(invalid(format!(
            "{label} contains {} CPUs; maximum is {HOST_PREFLIGHT_MAX_CPUS}",
            cpus.len()
        )));
    }

    let mut previous = None;
    let mut set = BTreeSet::new();
    for &cpu in cpus {
        if cpu > HOST_PREFLIGHT_MAX_CPU_ID {
            return Err(invalid(format!(
                "{label} contains CPU id {cpu} above maximum {HOST_PREFLIGHT_MAX_CPU_ID}"
            )));
        }
        if previous.is_some_and(|previous| cpu <= previous) {
            return Err(invalid(format!(
                "{label} must be strictly increasing and duplicate-free"
            )));
        }
        set.insert(cpu);
        previous = Some(cpu);
    }
    Ok(set)
}

fn validate_turbo_observation(
    turbo: &HostPreflightTurboObservation,
) -> Result<(), HostPreflightVerifyError> {
    match (&turbo.interface, &turbo.raw_value, turbo.disabled) {
        (None, None, None) => Ok(()),
        (Some(interface), Some(raw), Some(disabled)) => {
            validate_trimmed_text("observation.turbo.raw_value", raw, 128)?;
            let expected_disabled = match interface.as_str() {
                INTEL_NO_TURBO_PATH => raw == "1",
                GENERIC_BOOST_PATH => raw == "0",
                other => {
                    return Err(invalid(format!(
                        "unsupported turbo observation interface {other:?}"
                    )))
                }
            };
            if disabled != expected_disabled {
                return Err(invalid(format!(
                    "turbo disabled={disabled} disagrees with interface {interface:?} raw value {raw:?}"
                )));
            }
            Ok(())
        }
        _ => Err(invalid(
            "turbo interface, raw_value, and disabled must either all be present or all be absent",
        )),
    }
}

fn validate_trimmed_text(
    label: &str,
    value: &str,
    maximum_bytes: usize,
) -> Result<(), HostPreflightVerifyError> {
    if value.is_empty() || value.len() > maximum_bytes || value.trim() != value {
        return Err(invalid(format!(
            "{label} must contain 1..={maximum_bytes} UTF-8 bytes without surrounding whitespace"
        )));
    }
    Ok(())
}

fn validate_optional_trimmed_text(
    label: &str,
    value: Option<&str>,
    maximum_bytes: usize,
) -> Result<(), HostPreflightVerifyError> {
    if let Some(value) = value {
        validate_trimmed_text(label, value, maximum_bytes)?;
    }
    Ok(())
}

fn recompute_violations(snapshot: &HostPreflightSnapshot) -> Vec<String> {
    let mut violations = Vec::new();
    let expected = &snapshot.expected.process_cpu_affinity;
    let expected_set: BTreeSet<u32> = expected.iter().copied().collect();

    match snapshot.observation.process_allowed_cpus.as_ref() {
        Some(actual) if actual == expected => {}
        Some(actual) => violations.push(format!(
            "process CPU affinity is {actual:?}; expected exactly {expected:?}"
        )),
        None => violations.push("process CPU affinity could not be observed".to_owned()),
    }

    match snapshot.observation.online_cpus.as_ref() {
        Some(actual) => {
            let actual: BTreeSet<u32> = actual.iter().copied().collect();
            let missing: Vec<u32> = expected_set.difference(&actual).copied().collect();
            if !missing.is_empty() {
                violations.push(format!("expected CPUs are offline or absent: {missing:?}"));
            }
        }
        None => violations.push("online CPU set could not be observed".to_owned()),
    }

    for cpu in expected {
        match snapshot
            .observation
            .governors
            .get(cpu)
            .and_then(Option::as_deref)
        {
            Some(HOST_PREFLIGHT_EXPECTED_GOVERNOR) => {}
            Some(actual) => violations.push(format!(
                "CPU {cpu} scaling governor is {actual:?}; expected {HOST_PREFLIGHT_EXPECTED_GOVERNOR:?}"
            )),
            None => violations.push(format!("CPU {cpu} scaling governor could not be observed")),
        }
    }

    match snapshot.observation.turbo.disabled {
        Some(true) => {}
        Some(false) => violations.push(format!(
            "turbo/boost is enabled according to {:?} value {:?}",
            snapshot.observation.turbo.interface, snapshot.observation.turbo.raw_value
        )),
        None => violations.push(
            "turbo/boost disabled state could not be established from supported Linux interfaces"
                .to_owned(),
        ),
    }

    match snapshot.observation.load_one {
        Some(load_one) if load_one.is_finite() => {
            let load_per_cpu = load_one / expected.len() as f64;
            if load_per_cpu > snapshot.expected.max_load_per_cpu {
                violations.push(format!(
                    "one-minute load per pinned CPU is {load_per_cpu:.6}; budget is {:.6}",
                    snapshot.expected.max_load_per_cpu
                ));
            }
        }
        _ => violations.push("one-minute system load could not be observed".to_owned()),
    }

    violations
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::fs;

    use serde_json::Value;
    use tempfile::tempdir;

    use super::{
        recompute_violations, validate_host_preflight_snapshot, verify_host_preflight_snapshot,
        HostPreflightExpectedControls, HostPreflightObservation, HostPreflightOperatorAttestations,
        HostPreflightSnapshot, HostPreflightTurboObservation, HOST_PREFLIGHT_LIMITATIONS,
        HOST_PREFLIGHT_PROTOCOL,
    };

    #[test]
    fn passing_snapshot_verifies_and_round_trips_from_file() {
        let snapshot = passing_snapshot();
        validate_host_preflight_snapshot(&snapshot, Some("perf-host-01"), true)
            .expect("validate passing snapshot");

        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("preflight.json");
        fs::write(
            &path,
            serde_json::to_vec_pretty(&snapshot).expect("encode snapshot"),
        )
        .expect("write snapshot");
        let summary = verify_host_preflight_snapshot(&path, Some("perf-host-01"), true)
            .expect("verify snapshot file");
        assert!(summary.valid);
        assert!(summary.passed);
        assert_eq!(summary.process_cpu_affinity, vec![2, 3]);
        assert_eq!(summary.violations, 0);
    }

    #[test]
    fn internally_consistent_failed_snapshot_is_auditable_but_not_admitted() {
        let mut snapshot = passing_snapshot();
        snapshot.observation.process_allowed_cpus = Some(vec![2, 3, 4]);
        snapshot.violations = recompute_violations(&snapshot);
        snapshot.passed = snapshot.violations.is_empty();
        validate_host_preflight_snapshot(&snapshot, None, false)
            .expect("failed snapshot remains structurally auditable");
        assert!(validate_host_preflight_snapshot(&snapshot, None, true).is_err());
    }

    #[test]
    fn tampered_violation_ledger_is_rejected() {
        let mut snapshot = passing_snapshot();
        snapshot.observation.process_allowed_cpus = Some(vec![2, 3, 4]);
        snapshot.passed = false;
        snapshot.violations.clear();
        assert!(validate_host_preflight_snapshot(&snapshot, None, false)
            .expect_err("tampered violation ledger must fail")
            .to_string()
            .contains("violations do not match"));
    }

    #[test]
    fn turbo_raw_value_and_derived_state_must_agree() {
        let mut snapshot = passing_snapshot();
        snapshot.observation.turbo.raw_value = Some("0".to_owned());
        assert!(validate_host_preflight_snapshot(&snapshot, None, false)
            .expect_err("inconsistent turbo state must fail")
            .to_string()
            .contains("turbo disabled"));
    }

    #[test]
    fn expected_host_label_is_enforced() {
        let snapshot = passing_snapshot();
        assert!(
            validate_host_preflight_snapshot(&snapshot, Some("different-host"), true)
                .expect_err("host label mismatch must fail")
                .to_string()
                .contains("differs from expected")
        );
    }

    #[test]
    fn unknown_json_fields_fail_closed() {
        let snapshot = passing_snapshot();
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("preflight.json");
        let mut value = serde_json::to_value(snapshot).expect("encode snapshot value");
        let Value::Object(object) = &mut value else {
            panic!("snapshot must encode as object");
        };
        object.insert("future_unversioned_field".to_owned(), Value::Bool(true));
        fs::write(
            &path,
            serde_json::to_vec_pretty(&value).expect("encode tampered snapshot"),
        )
        .expect("write snapshot");
        assert!(verify_host_preflight_snapshot(&path, None, false)
            .expect_err("unknown fields must fail")
            .to_string()
            .contains("unknown field"));
    }

    fn passing_snapshot() -> HostPreflightSnapshot {
        HostPreflightSnapshot {
            protocol: HOST_PREFLIGHT_PROTOCOL.to_owned(),
            recorded_unix_seconds: 1_788_000_000,
            host_label: "perf-host-01".to_owned(),
            passed: true,
            expected: HostPreflightExpectedControls {
                process_cpu_affinity: vec![2, 3],
                scaling_governor: "performance".to_owned(),
                turbo_disabled: true,
                max_load_per_cpu: 0.10,
            },
            observation: HostPreflightObservation {
                target_os: "linux".to_owned(),
                target_arch: "x86_64".to_owned(),
                kernel_release: Some("example-kernel".to_owned()),
                cpu_model: Some("Example CPU".to_owned()),
                process_allowed_cpus: Some(vec![2, 3]),
                online_cpus: Some(vec![0, 1, 2, 3]),
                governors: BTreeMap::from([
                    (2, Some("performance".to_owned())),
                    (3, Some("performance".to_owned())),
                ]),
                turbo: HostPreflightTurboObservation {
                    interface: Some("/sys/devices/system/cpu/intel_pstate/no_turbo".to_owned()),
                    raw_value: Some("1".to_owned()),
                    disabled: Some(true),
                },
                load_one: Some(0.10),
            },
            operator_attestations: HostPreflightOperatorAttestations {
                thermal_control: "steady-state thermal protocol".to_owned(),
                background_load_control: "benchmark services only".to_owned(),
                storage_cache_control: "trace-induced warm policy".to_owned(),
            },
            violations: Vec::new(),
            limitations: HOST_PREFLIGHT_LIMITATIONS
                .iter()
                .map(|value| (*value).to_owned())
                .collect(),
        }
    }
}
