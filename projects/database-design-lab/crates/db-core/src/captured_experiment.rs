use serde::Serialize;
use thiserror::Error;

use crate::{
    compare_experiment_trace_ordered, AmplificationInstrumented, DbError, ErrorClass,
    ExperimentExecutionOrder, ExperimentTrace, KvEngine, OperationalTimingInstrumented,
    OperationalTimingReport, OrderedExperimentComparisonReport,
};

/// Serializable engine-local evidence retained when an ordered comparison fails.
///
/// This intentionally stores only stable error classification/text plus the two timing reports. The
/// original typed [`DbError`] remains available on [`CapturedOrderedExperimentError::source`] for callers
/// that need programmatic error handling without parsing the serialized message.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct OrderedExperimentFailureEvidence {
    /// Whole-run order selected for the comparison that failed.
    pub execution_order: ExperimentExecutionOrder,
    /// Stable coarse class of the original comparison error.
    pub error_class: ErrorClass,
    /// Human-readable original comparison error.
    pub message: String,
    /// Left engine timing samples retained at the exact failure boundary.
    pub left_operational_timing: OperationalTimingReport,
    /// Right engine timing samples retained at the exact failure boundary.
    pub right_operational_timing: OperationalTimingReport,
}

/// Typed ordered-comparison error plus serializable partial evidence.
#[derive(Debug, Error)]
#[error("{source}")]
pub struct CapturedOrderedExperimentError {
    /// Original typed comparison error.
    #[source]
    pub source: DbError,
    /// Engine-local timing evidence sampled immediately after the failure returned.
    /// Boxed so the normal `Result` remains compact while the uncommon error path can retain both reports.
    pub evidence: Box<OrderedExperimentFailureEvidence>,
}

/// Runs an ordered comparison while retaining both engine timing reports on failure.
///
/// The existing [`compare_experiment_trace_ordered`] API remains unchanged. This opt-in wrapper is the
/// non-lossy path for callers that must archive failed recovery/compaction attempts: both engines are
/// still borrowed here when the underlying runner returns, so process-local failure samples can be
/// captured before the caller drops or replaces either engine instance.
pub fn compare_experiment_trace_ordered_captured<L, R>(
    left: &mut L,
    right: &mut R,
    trace: &ExperimentTrace,
    execution_order: ExperimentExecutionOrder,
) -> std::result::Result<OrderedExperimentComparisonReport, CapturedOrderedExperimentError>
where
    L: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
    R: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
{
    compare_experiment_trace_ordered(left, right, trace, execution_order).map_err(|source| {
        let evidence = Box::new(OrderedExperimentFailureEvidence {
            execution_order,
            error_class: source.class(),
            message: source.to_string(),
            left_operational_timing: left.operational_timing_report(),
            right_operational_timing: right.operational_timing_report(),
        });
        CapturedOrderedExperimentError { source, evidence }
    })
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use super::compare_experiment_trace_ordered_captured;
    use crate::{
        generate_experiment_trace, AmplificationInstrumented, AmplificationRatio,
        AmplificationReport, ConcurrencyMode, CrashRecovery, DbError, DistributionMode,
        EngineCapabilities, ErrorClass, ExperimentExecutionOrder, ExperimentGeneratorConfig,
        ExperimentProfile, KvEngine, LogicalModel, OperationalTimingFailureSample,
        OperationalTimingInstrumented, OperationalTimingReport, Persistence, ReadWorkUnit, Result,
        StorageArchitecture, StructuralReadAmplification, MAX_KEY_BYTES, MAX_VALUE_BYTES,
    };

    #[test]
    fn captures_failure_sample_before_the_failing_engine_is_dropped() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 0x37,
            profile: ExperimentProfile::PointRead,
            operations: 1,
            key_space: 1,
            value_bytes: 4,
            range_limit: 1,
            reopen_every: Some(1),
        })
        .expect("generate trace with measured reopen");
        assert!(trace.measured_steps.len() >= 2);

        let mut left = FakeEngine::new("left", StorageArchitecture::BPlusTree, false);
        let mut right = FakeEngine::new("right", StorageArchitecture::LsmTree, true);

        let error = compare_experiment_trace_ordered_captured(
            &mut left,
            &mut right,
            &trace,
            ExperimentExecutionOrder::LeftThenRight,
        )
        .expect_err("right measured reopen must fail");

        assert_eq!(error.source.class(), ErrorClass::Io);
        assert_eq!(
            error.evidence.execution_order,
            ExperimentExecutionOrder::LeftThenRight
        );
        assert_eq!(error.evidence.error_class, ErrorClass::Io);
        assert!(error
            .evidence
            .left_operational_timing
            .reopen_failure_samples
            .is_empty());
        assert_eq!(
            error
                .evidence
                .right_operational_timing
                .reopen_failure_samples
                .len(),
            1
        );
        let failure = error
            .evidence
            .right_operational_timing
            .reopen_failure_samples[0];
        assert_eq!(failure.duration_ns, 77);
        assert_eq!(failure.error_class, ErrorClass::Io);
        assert!(failure.measured_step_index.is_some());
    }

    struct FakeEngine {
        name: &'static str,
        architecture: StorageArchitecture,
        map: BTreeMap<Vec<u8>, Vec<u8>>,
        fail_reopen: bool,
        operational_timing: OperationalTimingReport,
        operational_step_index: Option<u64>,
    }

    impl FakeEngine {
        fn new(name: &'static str, architecture: StorageArchitecture, fail_reopen: bool) -> Self {
            Self {
                name,
                architecture,
                map: BTreeMap::new(),
                fail_reopen,
                operational_timing: OperationalTimingReport::default(),
                operational_step_index: None,
            }
        }
    }

    impl KvEngine for FakeEngine {
        fn capabilities(&self) -> EngineCapabilities {
            EngineCapabilities {
                name: self.name,
                logical_model: LogicalModel::KeyValue,
                storage_architecture: self.architecture,
                concurrency: ConcurrencyMode::CallerSerialized,
                persistence: Persistence::Persistent,
                crash_recovery: match self.architecture {
                    StorageArchitecture::BPlusTree => CrashRecovery::MirroredCopyOnWritePages,
                    _ => CrashRecovery::WriteAheadLogReplay,
                },
                distribution: DistributionMode::Standalone,
                ordered_range_scan: true,
                max_key_bytes: MAX_KEY_BYTES,
                max_value_bytes: MAX_VALUE_BYTES,
            }
        }

        fn put(&mut self, key: &[u8], value: &[u8]) -> Result<Option<Vec<u8>>> {
            Ok(self.map.insert(key.to_vec(), value.to_vec()))
        }

        fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
            Ok(self.map.get(key).cloned())
        }

        fn delete(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
            Ok(self.map.remove(key))
        }

        fn range_scan(
            &mut self,
            start: &[u8],
            end: Option<&[u8]>,
            limit: usize,
        ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
            Ok(self
                .map
                .iter()
                .filter(|(key, _)| key.as_slice() >= start)
                .filter(|(key, _)| end.is_none_or(|end| key.as_slice() < end))
                .take(limit)
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect())
        }

        fn reopen(&mut self) -> Result<()> {
            if !self.fail_reopen {
                return Ok(());
            }
            self.operational_timing
                .reopen_failure_samples
                .push(OperationalTimingFailureSample {
                    measured_step_index: self.operational_step_index,
                    duration_ns: 77,
                    work: None,
                    error_class: ErrorClass::Io,
                });
            Err(DbError::Io(std::io::Error::other(
                "injected reopen failure",
            )))
        }
    }

    impl OperationalTimingInstrumented for FakeEngine {
        fn reset_operational_timing(&mut self) {
            self.operational_timing = OperationalTimingReport::default();
            self.operational_step_index = None;
        }

        fn set_operational_step_index(&mut self, step_index: Option<u64>) {
            self.operational_step_index = step_index;
        }

        fn operational_timing_report(&self) -> OperationalTimingReport {
            self.operational_timing.clone()
        }
    }

    impl AmplificationInstrumented for FakeEngine {
        fn reset_amplification(&mut self) {}

        fn amplification_report(&mut self) -> Result<AmplificationReport> {
            let point_unit = if self.architecture == StorageArchitecture::BPlusTree {
                ReadWorkUnit::BtreePageAccess
            } else {
                ReadWorkUnit::LsmSstableConsult
            };
            let range_unit = if self.architecture == StorageArchitecture::BPlusTree {
                ReadWorkUnit::BtreePageAccess
            } else {
                ReadWorkUnit::LsmSstableVersionDecoded
            };
            Ok(AmplificationReport {
                point_read: StructuralReadAmplification {
                    ratio: AmplificationRatio::default(),
                    unit: point_unit,
                },
                range_read: StructuralReadAmplification {
                    ratio: AmplificationRatio::default(),
                    unit: range_unit,
                },
                data_write_bytes_per_logical_byte: AmplificationRatio::default(),
                primary_structure_bytes_per_live_byte: AmplificationRatio::default(),
            })
        }
    }
}
