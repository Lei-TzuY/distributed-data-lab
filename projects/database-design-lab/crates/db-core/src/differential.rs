use serde::Serialize;
use thiserror::Error;

use crate::{execute_step, DbError, KvEngine, Outcome, Workload, WorkloadStep};

/// Successful differential comparison summary.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct DifferentialReport {
    /// Number of steps whose observable outcomes matched.
    pub steps_checked: usize,
}

/// A differential run failed or found a semantic mismatch.
#[derive(Debug, Error)]
pub enum DifferentialError {
    /// The workload itself is invalid.
    #[error("invalid workload: {0}")]
    InvalidWorkload(#[source] DbError),
    /// Engine capability declarations cannot support one common point-operation contract.
    #[error("incompatible engine capabilities: {0}")]
    IncompatibleCapabilities(String),
    /// The left engine failed while executing a valid step.
    #[error("left engine failed at step {step_index}: {source}")]
    LeftEngine {
        /// Zero-based failing step.
        step_index: usize,
        /// Engine error.
        #[source]
        source: DbError,
    },
    /// The right engine failed while executing a valid step.
    #[error("right engine failed at step {step_index}: {source}")]
    RightEngine {
        /// Zero-based failing step.
        step_index: usize,
        /// Engine error.
        #[source]
        source: DbError,
    },
    /// Both operations succeeded but returned different logical outcomes.
    #[error("semantic mismatch at step {step_index} ({step:?}): left={left:?}, right={right:?}")]
    Mismatch {
        /// Zero-based mismatching step.
        step_index: usize,
        /// Input step.
        step: WorkloadStep,
        /// Left result.
        left: Outcome,
        /// Right result.
        right: Outcome,
    },
}

/// Executes every step against two engines and requires identical observable outcomes.
pub fn compare_workload<L: KvEngine, R: KvEngine>(
    left: &mut L,
    right: &mut R,
    workload: &Workload,
) -> Result<DifferentialReport, DifferentialError> {
    workload
        .validate()
        .map_err(DifferentialError::InvalidWorkload)?;

    let left_capabilities = left.capabilities();
    let right_capabilities = right.capabilities();
    if left_capabilities.logical_model != right_capabilities.logical_model {
        return Err(DifferentialError::IncompatibleCapabilities(format!(
            "logical models differ: left={:?}, right={:?}",
            left_capabilities.logical_model, right_capabilities.logical_model
        )));
    }
    if left_capabilities.concurrency != right_capabilities.concurrency
        || left_capabilities.distribution != right_capabilities.distribution
    {
        return Err(DifferentialError::IncompatibleCapabilities(format!(
            "execution contracts differ: left=({:?}, {:?}), right=({:?}, {:?})",
            left_capabilities.concurrency,
            left_capabilities.distribution,
            right_capabilities.concurrency,
            right_capabilities.distribution
        )));
    }
    if left_capabilities.max_key_bytes != right_capabilities.max_key_bytes
        || left_capabilities.max_value_bytes != right_capabilities.max_value_bytes
    {
        return Err(DifferentialError::IncompatibleCapabilities(format!(
            "common size limits differ: left=({} key, {} value), right=({} key, {} value)",
            left_capabilities.max_key_bytes,
            left_capabilities.max_value_bytes,
            right_capabilities.max_key_bytes,
            right_capabilities.max_value_bytes
        )));
    }

    for (step_index, step) in workload.steps.iter().enumerate() {
        let left_outcome = execute_step(left, step)
            .map_err(|source| DifferentialError::LeftEngine { step_index, source })?;
        let right_outcome = execute_step(right, step)
            .map_err(|source| DifferentialError::RightEngine { step_index, source })?;
        if left_outcome != right_outcome {
            return Err(DifferentialError::Mismatch {
                step_index,
                step: step.clone(),
                left: left_outcome,
                right: right_outcome,
            });
        }
    }

    Ok(DifferentialReport {
        steps_checked: workload.steps.len(),
    })
}
