//! Shared logical semantics for the database laboratory.
//!
//! This crate deliberately contains only concepts already exercised by real engines. It is not a
//! framework for hypothetical future data models.

mod bytes;
mod captured_experiment;
mod counterbalanced_experiment;
mod counterbalanced_failure;
mod differential;
mod engine;
mod error;
mod experiment;
mod experiment_batch;
mod experiment_batch_failure;
mod operation;
mod ordered_experiment;
mod shrink;
mod workload;

pub use bytes::{ByteString, ByteStringError};
pub use captured_experiment::{
    compare_experiment_trace_ordered_captured, CapturedOrderedExperimentError,
    OrderedExperimentFailureEvidence,
};
pub use counterbalanced_experiment::{
    compare_experiment_trace_counterbalanced, CounterbalancedExperimentComparisonReport,
    CounterbalancedPairOrder,
};
pub use counterbalanced_failure::{
    compare_experiment_trace_counterbalanced_captured, CapturedCounterbalancedExperimentError,
    CounterbalancedComparisonFailureEvidence,
};
pub use differential::{compare_workload, DifferentialError, DifferentialReport};
pub use engine::{
    execute_step, execute_workload, validate_experiment_compatibility, validate_key,
    validate_key_value, validate_range_scan, AmplificationInstrumented, AmplificationRatio,
    AmplificationReport, ConcurrencyMode, CrashRecovery, DistributionMode, EngineCapabilities,
    KvEngine, LogicalModel, OperationalTimingFailureSample, OperationalTimingInstrumented,
    OperationalTimingReport, OperationalTimingSample, OperationalWork, OperationalWorkUnit,
    Persistence, ReadWorkUnit, StorageArchitecture, StructuralReadAmplification, MAX_KEY_BYTES,
    MAX_VALUE_BYTES,
};
pub use error::{DbError, ErrorClass, Result};
pub use experiment::{
    compare_experiment_trace, execute_experiment_step, generate_experiment_trace,
    run_experiment_trace, ExperimentComparisonReport, ExperimentEngineEvidence,
    ExperimentGeneratorConfig, ExperimentOutcome, ExperimentProfile, ExperimentRow,
    ExperimentRunReport, ExperimentStep, ExperimentTrace, EXPERIMENT_TRACE_FORMAT_VERSION,
    MAX_EXPERIMENT_OUTCOME_PAYLOAD_BYTES, MAX_EXPERIMENT_RANGE_LIMIT, MAX_EXPERIMENT_STEPS,
    MAX_EXPERIMENT_TRACE_PAYLOAD_BYTES,
};
pub use experiment_batch::{
    run_counterbalanced_experiment_batch, CounterbalancedExperimentBatchReport,
    ExperimentAttemptAdmission, ExperimentAttemptContext, ExperimentAttemptDisposition,
    ExperimentAttemptFailure, ExperimentAttemptFailureStage, ExperimentAttemptRecord,
    ExperimentEngineRole, ExperimentInstanceContext, MAX_EXPERIMENT_BATCH_PAIRS,
};
pub use experiment_batch_failure::{
    run_counterbalanced_experiment_batch_captured, CounterbalancedBatchComparisonFailureEvidence,
    CounterbalancedExperimentBatchCapturedReport,
};
pub use operation::{Outcome, WorkloadStep};
pub use ordered_experiment::{
    compare_experiment_trace_ordered, ExperimentExecutionOrder, OrderedExperimentComparisonReport,
};
pub use shrink::minimize_failing_workload;
pub use workload::{
    generate_workload, GeneratorConfig, Workload, MAX_WORKLOAD_STEPS, WORKLOAD_FORMAT_VERSION,
};
