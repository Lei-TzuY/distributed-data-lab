use serde::Serialize;
use thiserror::Error;

use crate::{
    compare_experiment_trace_ordered_captured, AmplificationInstrumented,
    CapturedOrderedExperimentError, CounterbalancedExperimentComparisonReport,
    CounterbalancedPairOrder, DbError, ErrorClass, ExperimentExecutionOrder, ExperimentTrace,
    KvEngine, OperationalTimingInstrumented, OrderedExperimentComparisonReport,
    OrderedExperimentFailureEvidence, Result,
};

/// Non-lossy evidence retained when one ordered comparison inside a counterbalanced pair fails.
///
/// A failure in repetition one can happen after repetition zero already completed successfully. That
/// completed report is preserved here instead of being discarded with the failed pair. The failing
/// ordered run carries both engine timing reports through `ordered_failure`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct CounterbalancedComparisonFailureEvidence {
    /// Outer AB/BA ordering selected for this pair.
    pub pair_order: CounterbalancedPairOrder,
    /// Zero for the first ordered comparison in the pair, one for the second.
    pub repetition_index: u8,
    /// Complete first repetition when the second repetition is the one that failed.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub completed_first: Option<OrderedExperimentComparisonReport>,
    /// Failure-boundary evidence from the ordered comparison that returned an error.
    pub ordered_failure: Box<OrderedExperimentFailureEvidence>,
}

/// Typed counterbalanced execution error with optional comparison-boundary evidence.
///
/// Factory/preflight/repetition-validation failures do not invent operational telemetry. Once both
/// fresh engines exist and an ordered comparison starts, `comparison_failure` is populated before
/// either engine instance is dropped.
#[derive(Debug, Error)]
#[error("{source}")]
pub struct CapturedCounterbalancedExperimentError {
    /// Original typed database error.
    #[source]
    pub source: DbError,
    /// Present only when an ordered comparison had started with both fresh engines available.
    pub comparison_failure: Option<Box<CounterbalancedComparisonFailureEvidence>>,
}

impl CapturedCounterbalancedExperimentError {
    /// Stable high-level class of the original error.
    #[must_use]
    pub fn class(&self) -> ErrorClass {
        self.source.class()
    }

    /// Returns failure-boundary evidence when ordered execution had started.
    #[must_use]
    pub fn comparison_failure(&self) -> Option<&CounterbalancedComparisonFailureEvidence> {
        self.comparison_failure.as_deref()
    }

    /// Consumes the wrapper into its typed source and optional evidence without cloning reports.
    #[must_use]
    pub fn into_parts(
        self,
    ) -> (
        DbError,
        Option<Box<CounterbalancedComparisonFailureEvidence>>,
    ) {
        (self.source, self.comparison_failure)
    }
}

/// Runs one fresh AB/BA pair while retaining non-lossy ordered-comparison failure evidence.
///
/// This is an additive counterpart to `compare_experiment_trace_counterbalanced`. Factory errors
/// remain typed errors with no fabricated telemetry. If repetition zero succeeds and repetition one
/// fails, the complete first report is retained alongside the failing run's engine-local timing.
pub fn compare_experiment_trace_counterbalanced_captured<L, R, MakeLeft, MakeRight>(
    trace: &ExperimentTrace,
    pair_order: CounterbalancedPairOrder,
    mut make_left: MakeLeft,
    mut make_right: MakeRight,
) -> std::result::Result<
    CounterbalancedExperimentComparisonReport,
    CapturedCounterbalancedExperimentError,
>
where
    L: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
    R: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
    MakeLeft: FnMut() -> Result<L>,
    MakeRight: FnMut() -> Result<R>,
{
    trace.validate().map_err(without_comparison_evidence)?;

    let first_order = first_execution_order(pair_order);
    let mut first_left = make_left().map_err(without_comparison_evidence)?;
    let mut first_right = make_right().map_err(without_comparison_evidence)?;
    let first = compare_experiment_trace_ordered_captured(
        &mut first_left,
        &mut first_right,
        trace,
        first_order,
    )
    .map_err(|error| ordered_failure(error, pair_order, 0, None))?;

    let second_order = second_execution_order(pair_order);
    let mut second_left = make_left().map_err(without_comparison_evidence)?;
    let mut second_right = make_right().map_err(without_comparison_evidence)?;
    let second = compare_experiment_trace_ordered_captured(
        &mut second_left,
        &mut second_right,
        trace,
        second_order,
    )
    .map_err(|error| ordered_failure(error, pair_order, 1, Some(first.clone())))?;

    validate_repetitions(&first, &second).map_err(without_comparison_evidence)?;
    Ok(CounterbalancedExperimentComparisonReport {
        pair_order,
        first,
        second,
    })
}

fn first_execution_order(pair_order: CounterbalancedPairOrder) -> ExperimentExecutionOrder {
    match pair_order {
        CounterbalancedPairOrder::LeftThenRightFirst => ExperimentExecutionOrder::LeftThenRight,
        CounterbalancedPairOrder::RightThenLeftFirst => ExperimentExecutionOrder::RightThenLeft,
    }
}

fn second_execution_order(pair_order: CounterbalancedPairOrder) -> ExperimentExecutionOrder {
    match pair_order {
        CounterbalancedPairOrder::LeftThenRightFirst => ExperimentExecutionOrder::RightThenLeft,
        CounterbalancedPairOrder::RightThenLeftFirst => ExperimentExecutionOrder::LeftThenRight,
    }
}

fn validate_repetitions(
    first: &OrderedExperimentComparisonReport,
    second: &OrderedExperimentComparisonReport,
) -> Result<()> {
    if first.comparison.left.capabilities != second.comparison.left.capabilities {
        return Err(DbError::InvalidInput(format!(
            "counterbalanced left-engine capabilities changed between repetitions: {} then {}",
            first.comparison.left.capabilities.name, second.comparison.left.capabilities.name
        )));
    }
    if first.comparison.right.capabilities != second.comparison.right.capabilities {
        return Err(DbError::InvalidInput(format!(
            "counterbalanced right-engine capabilities changed between repetitions: {} then {}",
            first.comparison.right.capabilities.name, second.comparison.right.capabilities.name
        )));
    }
    if first.comparison.outcomes != second.comparison.outcomes {
        return Err(DbError::InvalidInput(
            "counterbalanced measured logical outcomes changed between repetitions".to_owned(),
        ));
    }
    Ok(())
}

fn without_comparison_evidence(source: DbError) -> CapturedCounterbalancedExperimentError {
    CapturedCounterbalancedExperimentError {
        source,
        comparison_failure: None,
    }
}

fn ordered_failure(
    error: CapturedOrderedExperimentError,
    pair_order: CounterbalancedPairOrder,
    repetition_index: u8,
    completed_first: Option<OrderedExperimentComparisonReport>,
) -> CapturedCounterbalancedExperimentError {
    let CapturedOrderedExperimentError { source, evidence } = error;
    CapturedCounterbalancedExperimentError {
        source,
        comparison_failure: Some(Box::new(CounterbalancedComparisonFailureEvidence {
            pair_order,
            repetition_index,
            completed_first,
            ordered_failure: evidence,
        })),
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use super::compare_experiment_trace_counterbalanced_captured;
    use crate::{
        generate_experiment_trace, AmplificationInstrumented, AmplificationRatio,
        AmplificationReport, ConcurrencyMode, CounterbalancedPairOrder, CrashRecovery, DbError,
        DistributionMode, EngineCapabilities, ErrorClass, ExperimentExecutionOrder,
        ExperimentGeneratorConfig, ExperimentProfile, KvEngine, LogicalModel,
        OperationalTimingFailureSample, OperationalTimingInstrumented, OperationalTimingReport,
        Persistence, ReadWorkUnit, Result, StorageArchitecture, StructuralReadAmplification,
        MAX_KEY_BYTES, MAX_VALUE_BYTES,
    };

    #[test]
    fn second_repetition_failure_retains_completed_first_report_and_timing() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 0x38,
            profile: ExperimentProfile::RandomWrite,
            operations: 1,
            key_space: 4,
            value_bytes: 4,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");
        let mut right_creations = 0_u8;

        let error = compare_experiment_trace_counterbalanced_captured(
            &trace,
            CounterbalancedPairOrder::LeftThenRightFirst,
            || {
                Ok(FakeEngine::new(
                    "left",
                    StorageArchitecture::BPlusTree,
                    false,
                ))
            },
            || {
                let fail_put = right_creations == 1;
                right_creations = right_creations.saturating_add(1);
                Ok(FakeEngine::new(
                    "right",
                    StorageArchitecture::LsmTree,
                    fail_put,
                ))
            },
        )
        .expect_err("second right engine must fail");

        assert_eq!(right_creations, 2);
        assert_eq!(error.class(), ErrorClass::Io);
        let evidence = error
            .comparison_failure()
            .expect("ordered failure evidence");
        assert_eq!(evidence.repetition_index, 1);
        assert_eq!(
            evidence.ordered_failure.execution_order,
            ExperimentExecutionOrder::RightThenLeft
        );
        let first = evidence
            .completed_first
            .as_ref()
            .expect("first repetition must survive second failure");
        assert_eq!(
            first.execution_order,
            ExperimentExecutionOrder::LeftThenRight
        );
        assert_eq!(first.comparison.outcomes.len(), trace.measured_steps.len());
        assert_eq!(
            evidence
                .ordered_failure
                .right_operational_timing
                .compaction_stall_failure_samples
                .len(),
            1
        );
        let sample = evidence
            .ordered_failure
            .right_operational_timing
            .compaction_stall_failure_samples[0];
        assert_eq!(sample.measured_step_index, Some(0));
        assert_eq!(sample.duration_ns, 13);
        assert_eq!(sample.error_class, ErrorClass::Io);
    }

    struct FakeEngine {
        name: &'static str,
        architecture: StorageArchitecture,
        fail_put: bool,
        map: BTreeMap<Vec<u8>, Vec<u8>>,
        timing: OperationalTimingReport,
        step: Option<u64>,
    }

    impl FakeEngine {
        fn new(name: &'static str, architecture: StorageArchitecture, fail_put: bool) -> Self {
            Self {
                name,
                architecture,
                fail_put,
                map: BTreeMap::new(),
                timing: OperationalTimingReport::default(),
                step: None,
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
            if self.fail_put {
                self.timing
                    .compaction_stall_failure_samples
                    .push(OperationalTimingFailureSample {
                        measured_step_index: self.step,
                        duration_ns: 13,
                        work: None,
                        error_class: ErrorClass::Io,
                    });
                return Err(DbError::Io(std::io::Error::other(
                    "injected second-repetition compaction failure",
                )));
            }
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
            Ok(())
        }
    }

    impl OperationalTimingInstrumented for FakeEngine {
        fn reset_operational_timing(&mut self) {
            self.timing = OperationalTimingReport::default();
            self.step = None;
        }

        fn set_operational_step_index(&mut self, step_index: Option<u64>) {
            self.step = step_index;
        }

        fn operational_timing_report(&self) -> OperationalTimingReport {
            self.timing.clone()
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
