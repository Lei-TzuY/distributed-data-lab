use serde::Serialize;

use crate::{ByteString, DbError, ErrorClass, Outcome, Result, Workload, WorkloadStep};

/// Maximum key size in the first common KV semantics.
pub const MAX_KEY_BYTES: usize = 4 * 1024;
/// Maximum value size in the first common KV semantics.
pub const MAX_VALUE_BYTES: usize = 1024 * 1024;

/// Logical contract exposed by an engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum LogicalModel {
    /// Opaque binary key to opaque binary value.
    KeyValue,
}

/// Physical architecture actually implemented by an engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum StorageArchitecture {
    /// Deterministic volatile oracle, not a persistent storage candidate.
    InMemoryReference,
    /// Versioned checksummed mutation log plus replay index.
    AppendLog,
    /// Checksummed page file with a copy-on-write B+ tree and mirrored metadata.
    BPlusTree,
    /// Write-ahead log plus ordered mutable and immutable MemTables.
    LsmTree,
}

/// Concurrency contract actually enforced by the current engine boundary.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ConcurrencyMode {
    /// The caller serializes access; no cross-process exclusion is provided.
    CallerSerialized,
}

/// Whether acknowledged state survives process restart.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Persistence {
    /// State exists only in memory.
    Volatile,
    /// State is represented in a versioned on-disk format.
    Persistent,
}

/// Crash-recovery behavior currently exposed by an engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CrashRecovery {
    /// No persistent crash recovery is applicable.
    None,
    /// Valid records replay and a structurally valid incomplete final append is discarded.
    TruncatedFinalAppend,
    /// Durable COW pages are published by alternating checksummed root metadata copies.
    MirroredCopyOnWritePages,
    /// A versioned checksummed write-ahead log reconstructs MemTables during reopen.
    WriteAheadLogReplay,
}

/// Distribution behavior currently exposed by an engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum DistributionMode {
    /// One local process owns the engine; no replication protocol exists.
    Standalone,
}

/// Explicit capabilities used to prevent accidental incomparable experiments.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct EngineCapabilities {
    /// Stable engine identifier.
    pub name: &'static str,
    /// Logical model implemented by the trait contract.
    pub logical_model: LogicalModel,
    /// Physical storage architecture.
    pub storage_architecture: StorageArchitecture,
    /// Concurrency contract.
    pub concurrency: ConcurrencyMode,
    /// Persistence behavior.
    pub persistence: Persistence,
    /// Crash-recovery behavior.
    pub crash_recovery: CrashRecovery,
    /// Distribution behavior.
    pub distribution: DistributionMode,
    /// Whether the public common semantics currently include ordered range scans.
    pub ordered_range_scan: bool,
    /// Maximum accepted key bytes.
    pub max_key_bytes: usize,
    /// Maximum accepted value bytes.
    pub max_value_bytes: usize,
}

/// Exact integer numerator/denominator pair used by reproducible amplification evidence.
///
/// Zero denominators are preserved instead of being converted into floating-point infinities or NaN.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize)]
pub struct AmplificationRatio {
    /// Raw physical/structural work numerator.
    pub numerator: u64,
    /// Raw logical baseline denominator.
    pub denominator: u64,
}

/// Architecture-specific structural unit used by a read-amplification numerator.
///
/// These units deliberately prevent a page access from being silently compared as though it were the
/// same event as consulting an SSTable or decoding one SSTable version. Device-level read bytes and
/// cache misses require a separate controlled benchmark layer.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ReadWorkUnit {
    /// One logical access to a validated fixed-size B+ tree page, including cache hits.
    BtreePageAccess,
    /// One LSM SSTable considered by a point lookup before a hit/miss decision.
    LsmSstableConsult,
    /// One physical SSTable record version decoded while resolving an ordered range.
    LsmSstableVersionDecoded,
}

/// One structural read-amplification ratio plus the unit carried by its numerator.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct StructuralReadAmplification {
    /// Exact structural-work numerator over the logical-operation/result denominator.
    pub ratio: AmplificationRatio,
    /// Architecture-specific unit represented by `ratio.numerator`.
    pub unit: ReadWorkUnit,
}

/// Common reporting shape for hand-computable storage-engine amplification evidence.
///
/// Point/range read numerators remain explicitly architecture-specific through `ReadWorkUnit`.
/// Data-write and primary-structure ratios use bytes, but callers must still keep each engine's
/// documented accounting boundary fixed when comparing experiments.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct AmplificationReport {
    /// Structural work per successful explicit point GET.
    pub point_read: StructuralReadAmplification,
    /// Structural work per logical record returned by successful range scans.
    pub range_read: StructuralReadAmplification,
    /// Data-path bytes written per acknowledged logical mutation byte.
    pub data_write_bytes_per_logical_byte: AmplificationRatio,
    /// Current primary data-structure bytes retained per live logical key+value byte.
    pub primary_structure_bytes_per_live_byte: AmplificationRatio,
}

/// Architecture-specific unit paired with one synchronous operational timing sample.
///
/// Like `ReadWorkUnit`, these are deterministic engine-level work units rather than device I/O events.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum OperationalWorkUnit {
    /// One logical validated B+ tree data-page access during reopen validation/reuse discovery.
    BtreePageAccess,
    /// One LSM persisted record version examined while reopening WAL/SSTable state.
    LsmRecordVersion,
    /// One authoritative SSTable record version consumed by a full-set compaction.
    LsmSstableRecordVersion,
}

/// Deterministic data-path work associated with one operational timing sample.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct OperationalWork {
    /// Architecture-specific logical work unit.
    pub unit: OperationalWorkUnit,
    /// Number of logical units examined by the operation.
    pub units_examined: u64,
    /// Data-path bytes represented by those units under the engine's documented accounting boundary.
    pub bytes_examined: u64,
}

/// One successful synchronous operation sample associated with an experiment step when available.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct OperationalTimingSample {
    /// Zero-based measured experiment step that triggered this sample, or `None` outside a measured runner.
    pub measured_step_index: Option<u64>,
    /// Wall-clock duration measured with `std::time::Instant`.
    pub duration_ns: u64,
    /// Deterministic data-path work completed by the timed operation.
    pub work: OperationalWork,
}

/// One failed synchronous operation sample associated with an experiment step when available.
///
/// `work` is optional by design. A failed operation may not expose enough validated state to reconstruct
/// deterministic work without guessing; callers must preserve `None` rather than substituting planned or
/// partially observed work under a successful-sample accounting unit.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct OperationalTimingFailureSample {
    /// Zero-based measured experiment step that triggered this sample, or `None` outside a measured runner.
    pub measured_step_index: Option<u64>,
    /// Wall-clock time elapsed before the operation returned an error.
    pub duration_ns: u64,
    /// Deterministic completed work when the failing path can prove it under the normal accounting boundary.
    pub work: Option<OperationalWork>,
    /// Stable coarse class of the returned operation error.
    pub error_class: ErrorClass,
}

/// Raw process-local recovery and compaction-stall samples.
///
/// Successful samples preserve the original nanosecond projections plus deterministic work. Failed
/// samples are kept separately so they cannot enter a success-only latency distribution by accident.
/// Duration/work evidence is not a performance claim: attempt retention, execution-order
/// counterbalancing, cache/filesystem protocol, host pinning, and scheduler/device controls remain
/// required before durations are compared across engines or revisions.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
pub struct OperationalTimingReport {
    /// Backward-compatible projection of successful same-handle `REOPEN` durations in nanoseconds.
    pub reopen_ns: Vec<u64>,
    /// Backward-compatible projection of successful synchronous compaction durations in nanoseconds.
    pub compaction_stall_ns: Vec<u64>,
    /// Successful same-handle `REOPEN` samples with deterministic work and measured-step association.
    pub reopen_samples: Vec<OperationalTimingSample>,
    /// Successful synchronous compaction samples with deterministic work and measured-step association.
    pub compaction_stall_samples: Vec<OperationalTimingSample>,
    /// Failed same-handle `REOPEN` attempts. Omitted from serialized reports when empty for schema compatibility.
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub reopen_failure_samples: Vec<OperationalTimingFailureSample>,
    /// Failed synchronous compactions. Omitted from serialized reports when empty for schema compatibility.
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub compaction_stall_failure_samples: Vec<OperationalTimingFailureSample>,
}

/// Reset/context/report surface for operational samples collected during an experiment window.
pub trait OperationalTimingInstrumented {
    /// Clears process-local operational samples without changing database state.
    fn reset_operational_timing(&mut self);

    /// Associates subsequently emitted operational samples with one measured experiment step.
    ///
    /// The experiment runner sets this immediately before a measured action and clears it immediately after.
    fn set_operational_step_index(&mut self, step_index: Option<u64>);

    /// Returns a clone of the raw operational samples accumulated in the current window.
    fn operational_timing_report(&self) -> OperationalTimingReport;
}

/// Common reset/report surface implemented by storage engines admitted to amplification experiments.
pub trait AmplificationInstrumented {
    /// Clears process-local measurement counters without changing database state.
    fn reset_amplification(&mut self);

    /// Returns an exact report for the current process-local measurement window and durable state.
    fn amplification_report(&mut self) -> Result<AmplificationReport>;
}

/// Rejects two engine configurations when common experiment semantics are not comparable.
///
/// Architecture and crash-recovery *mechanism* are intentionally allowed to differ: those are the
/// independent variables. Logical model, caller/concurrency contract, persistence class, distribution
/// mode, ordered-range capability, and size limits must match. `require_ordered_range` additionally
/// refuses a pair that does not expose the common half-open range API.
pub fn validate_experiment_compatibility(
    left: EngineCapabilities,
    right: EngineCapabilities,
    require_ordered_range: bool,
) -> Result<()> {
    let mut mismatches = Vec::new();
    if left.logical_model != right.logical_model {
        mismatches.push("logical_model");
    }
    if left.concurrency != right.concurrency {
        mismatches.push("concurrency");
    }
    if left.persistence != right.persistence {
        mismatches.push("persistence");
    }
    if left.distribution != right.distribution {
        mismatches.push("distribution");
    }
    if left.ordered_range_scan != right.ordered_range_scan {
        mismatches.push("ordered_range_scan");
    }
    if left.max_key_bytes != right.max_key_bytes {
        mismatches.push("max_key_bytes");
    }
    if left.max_value_bytes != right.max_value_bytes {
        mismatches.push("max_value_bytes");
    }
    if require_ordered_range && (!left.ordered_range_scan || !right.ordered_range_scan) {
        mismatches.push("required_ordered_range_scan");
    }
    if mismatches.is_empty() {
        return Ok(());
    }
    Err(DbError::InvalidInput(format!(
        "engine capabilities are not experiment-compatible: {} vs {} differ in {}",
        left.name,
        right.name,
        mismatches.join(", ")
    )))
}

/// Minimal engine contract proven by the current reference and persistent implementations.
pub trait KvEngine {
    /// Returns explicit capabilities for experiment validation.
    fn capabilities(&self) -> EngineCapabilities;

    /// Sets a key and returns the previous value, if any.
    fn put(&mut self, key: &[u8], value: &[u8]) -> Result<Option<Vec<u8>>>;

    /// Reads a key.
    fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>>;

    /// Deletes a key and returns the previous value, if any.
    fn delete(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>>;

    /// Returns up to `limit` key/value pairs in ascending bytewise key order from `[start, end)`.
    ///
    /// `end = None` means no upper bound. Engines whose capability advertises
    /// `ordered_range_scan = false` may reject this operation.
    fn range_scan(
        &mut self,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        let _ = (start, end, limit);
        Err(DbError::InvalidInput(format!(
            "engine {} does not expose ordered range scans",
            self.capabilities().name
        )))
    }

    /// Closes and reopens engine state, replaying persistent state where applicable.
    fn reopen(&mut self) -> Result<()>;
}

/// Validates a key against the common semantics.
pub fn validate_key(key: &[u8]) -> Result<()> {
    if key.len() > MAX_KEY_BYTES {
        return Err(DbError::InvalidInput(format!(
            "key has {} bytes; maximum is {MAX_KEY_BYTES}",
            key.len()
        )));
    }
    Ok(())
}

/// Validates half-open ordered range bounds against the common key semantics.
pub fn validate_range_scan(start: &[u8], end: Option<&[u8]>) -> Result<()> {
    validate_key(start)?;
    if let Some(end) = end {
        validate_key(end)?;
        if end < start {
            return Err(DbError::InvalidInput(
                "ordered range end must not sort before start".to_owned(),
            ));
        }
    }
    Ok(())
}

/// Validates a key/value pair against the common semantics.
pub fn validate_key_value(key: &[u8], value: &[u8]) -> Result<()> {
    validate_key(key)?;
    if value.len() > MAX_VALUE_BYTES {
        return Err(DbError::InvalidInput(format!(
            "value has {} bytes; maximum is {MAX_VALUE_BYTES}",
            value.len()
        )));
    }
    Ok(())
}

/// Executes one step using the common observable semantics.
pub fn execute_step<E: KvEngine>(engine: &mut E, step: &WorkloadStep) -> Result<Outcome> {
    match step {
        WorkloadStep::Put { key, value } => {
            engine
                .put(key.as_slice(), value.as_slice())
                .map(|previous| Outcome::Put {
                    previous: previous.map(ByteString::from),
                })
        }
        WorkloadStep::Get { key } => engine.get(key.as_slice()).map(|value| Outcome::Get {
            value: value.map(ByteString::from),
        }),
        WorkloadStep::Delete { key } => {
            engine
                .delete(key.as_slice())
                .map(|previous| Outcome::Delete {
                    previous: previous.map(ByteString::from),
                })
        }
        WorkloadStep::Reopen => {
            engine.reopen()?;
            Ok(Outcome::Reopened)
        }
    }
}

/// Executes a validated workload and returns every observable result.
pub fn execute_workload<E: KvEngine>(engine: &mut E, workload: &Workload) -> Result<Vec<Outcome>> {
    workload.validate()?;
    workload
        .steps
        .iter()
        .map(|step| execute_step(engine, step))
        .collect()
}
