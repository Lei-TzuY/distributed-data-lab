use serde::Serialize;

use crate::{
    compare_experiment_trace_counterbalanced_captured, AmplificationInstrumented,
    CounterbalancedComparisonFailureEvidence, CounterbalancedExperimentBatchReport,
    CounterbalancedExperimentComparisonReport, CounterbalancedPairOrder, DbError,
    ExperimentAttemptAdmission, ExperimentAttemptContext, ExperimentAttemptDisposition,
    ExperimentAttemptFailure, ExperimentAttemptFailureStage, ExperimentAttemptRecord,
    ExperimentEngineRole, ExperimentInstanceContext, ExperimentTrace, KvEngine,
    OperationalTimingInstrumented, MAX_EXPERIMENT_BATCH_PAIRS,
};

/// One comparison-failure sidecar entry bound to its unique requested-pair identity.
///
/// `CounterbalancedComparisonFailureEvidence` intentionally describes one AB/BA pair in isolation and
/// therefore carries pair order but not the repeated-batch index. Repeated evidence must retain the
/// surrounding `ExperimentAttemptContext` so a sidecar can be joined to exactly one failed ledger row
/// without guessing among later pairs that reuse the same alternating order.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct CounterbalancedBatchComparisonFailureEvidence {
    /// Unique pair context within the repeated batch.
    pub context: ExperimentAttemptContext,
    /// Failure-boundary evidence captured before either fresh engine is dropped.
    pub failure: CounterbalancedComparisonFailureEvidence,
}

/// Stable batch ledger plus sidecar-ready evidence from failed ordered comparisons.
///
/// The nested `batch` keeps the existing report shape. `comparison_failures` is deliberately separate
/// so archive formats can version failure telemetry independently of the stable denominator ledger.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct CounterbalancedExperimentBatchCapturedReport {
    /// Existing non-lossy included/failed/excluded pair ledger.
    pub batch: CounterbalancedExperimentBatchReport,
    /// Failure-boundary reports for unsuccessful pairs whose ordered comparison had started.
    pub comparison_failures: Vec<CounterbalancedBatchComparisonFailureEvidence>,
}

/// Runs repeated fresh counterbalanced pairs while retaining comparison-boundary timing sidecars.
///
/// Factory failures remain represented only in the stable batch ledger because one or both fresh
/// instances never existed. Ordered-comparison failures retain both engine timing reports, and a
/// second-repetition failure additionally retains the complete first repetition. Every sidecar is bound
/// to the exact pair index/order from the stable ledger before later requested pairs continue.
pub fn run_counterbalanced_experiment_batch_captured<L, R, MakeLeft, MakeRight, Admit>(
    trace: &ExperimentTrace,
    pair_seed: u64,
    requested_pairs: u32,
    mut make_left: MakeLeft,
    mut make_right: MakeRight,
    mut admit: Admit,
) -> std::result::Result<CounterbalancedExperimentBatchCapturedReport, DbError>
where
    L: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
    R: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
    MakeLeft: FnMut(ExperimentInstanceContext) -> std::result::Result<L, DbError>,
    MakeRight: FnMut(ExperimentInstanceContext) -> std::result::Result<R, DbError>,
    Admit: FnMut(ExperimentAttemptContext) -> ExperimentAttemptAdmission,
{
    trace.validate()?;
    if requested_pairs == 0 || requested_pairs > MAX_EXPERIMENT_BATCH_PAIRS {
        return Err(DbError::InvalidInput(format!(
            "experiment batch pairs is {requested_pairs}; expected 1..={MAX_EXPERIMENT_BATCH_PAIRS}"
        )));
    }

    let mut attempts = Vec::with_capacity(requested_pairs as usize);
    let mut comparison_failures = Vec::new();
    let mut included_pairs = 0_u32;
    let mut failed_pairs = 0_u32;
    let mut excluded_pairs = 0_u32;

    for pair_index in 0..requested_pairs {
        let pair_order = seeded_pair_order(pair_seed, pair_index);
        let context = ExperimentAttemptContext {
            pair_index,
            pair_order,
        };
        match admit(context) {
            ExperimentAttemptAdmission::Exclude { reason } => {
                let reason = reason.trim().to_owned();
                if reason.is_empty() {
                    return Err(DbError::InvalidInput(format!(
                        "experiment batch exclusion for pair {pair_index} must include a non-empty reason"
                    )));
                }
                excluded_pairs = excluded_pairs.saturating_add(1);
                attempts.push(ExperimentAttemptRecord {
                    context,
                    disposition: ExperimentAttemptDisposition::Excluded,
                    report: None,
                    failure: None,
                    exclusion_reason: Some(reason),
                });
            }
            ExperimentAttemptAdmission::Include => {
                let mut left_repetition = 0_u8;
                let mut right_repetition = 0_u8;
                let mut left_factory_failure = None;
                let mut right_factory_failure = None;
                let result = compare_experiment_trace_counterbalanced_captured(
                    trace,
                    pair_order,
                    || {
                        let repetition_index = left_repetition;
                        left_repetition = left_repetition.saturating_add(1);
                        let instance = ExperimentInstanceContext {
                            attempt: context,
                            repetition_index,
                        };
                        make_left(instance).inspect_err(|error| {
                            left_factory_failure.get_or_insert_with(|| ExperimentAttemptFailure {
                                stage: ExperimentAttemptFailureStage::EngineFactory,
                                engine_role: Some(ExperimentEngineRole::Left),
                                repetition_index: Some(repetition_index),
                                class: error.class(),
                                message: error.to_string(),
                            });
                        })
                    },
                    || {
                        let repetition_index = right_repetition;
                        right_repetition = right_repetition.saturating_add(1);
                        let instance = ExperimentInstanceContext {
                            attempt: context,
                            repetition_index,
                        };
                        make_right(instance).inspect_err(|error| {
                            right_factory_failure.get_or_insert_with(|| ExperimentAttemptFailure {
                                stage: ExperimentAttemptFailureStage::EngineFactory,
                                engine_role: Some(ExperimentEngineRole::Right),
                                repetition_index: Some(repetition_index),
                                class: error.class(),
                                message: error.to_string(),
                            });
                        })
                    },
                );

                match result {
                    Ok(report) => {
                        included_pairs = included_pairs.saturating_add(1);
                        attempts.push(included_record(context, report));
                    }
                    Err(error) => {
                        failed_pairs = failed_pairs.saturating_add(1);
                        let (source, comparison_failure) = error.into_parts();
                        if let Some(evidence) = comparison_failure {
                            comparison_failures.push(
                                CounterbalancedBatchComparisonFailureEvidence {
                                    context,
                                    failure: *evidence,
                                },
                            );
                        }
                        let class = source.class();
                        let message = source.to_string();
                        let failure = left_factory_failure.or(right_factory_failure).unwrap_or(
                            ExperimentAttemptFailure {
                                stage: ExperimentAttemptFailureStage::Comparison,
                                engine_role: None,
                                repetition_index: None,
                                class,
                                message,
                            },
                        );
                        attempts.push(ExperimentAttemptRecord {
                            context,
                            disposition: ExperimentAttemptDisposition::Failed,
                            report: None,
                            failure: Some(failure),
                            exclusion_reason: None,
                        });
                    }
                }
            }
        }
    }

    Ok(CounterbalancedExperimentBatchCapturedReport {
        batch: CounterbalancedExperimentBatchReport {
            trace: trace.clone(),
            pair_seed,
            requested_pairs,
            included_pairs,
            failed_pairs,
            excluded_pairs,
            attempts,
        },
        comparison_failures,
    })
}

fn included_record(
    context: ExperimentAttemptContext,
    report: CounterbalancedExperimentComparisonReport,
) -> ExperimentAttemptRecord {
    ExperimentAttemptRecord {
        context,
        disposition: ExperimentAttemptDisposition::Included,
        report: Some(report),
        failure: None,
        exclusion_reason: None,
    }
}

fn seeded_pair_order(seed: u64, pair_index: u32) -> CounterbalancedPairOrder {
    if ((seed & 1) ^ u64::from(pair_index & 1)) == 0 {
        CounterbalancedPairOrder::LeftThenRightFirst
    } else {
        CounterbalancedPairOrder::RightThenLeftFirst
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use super::run_counterbalanced_experiment_batch_captured;
    use crate::{
        generate_experiment_trace, AmplificationInstrumented, AmplificationRatio,
        AmplificationReport, ConcurrencyMode, CounterbalancedPairOrder, CrashRecovery, DbError,
        DistributionMode, EngineCapabilities, ErrorClass, ExperimentAttemptAdmission,
        ExperimentGeneratorConfig, ExperimentProfile, KvEngine, LogicalModel,
        OperationalTimingFailureSample, OperationalTimingInstrumented, OperationalTimingReport,
        Persistence, ReadWorkUnit, Result, StorageArchitecture, StructuralReadAmplification,
        MAX_KEY_BYTES, MAX_VALUE_BYTES,
    };

    #[test]
    fn batch_retains_pair_identity_second_repetition_failure_and_later_pairs() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 0x39,
            profile: ExperimentProfile::RandomWrite,
            operations: 1,
            key_space: 4,
            value_bytes: 4,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");

        let report = run_counterbalanced_experiment_batch_captured(
            &trace,
            0,
            3,
            |_context| {
                Ok(FakeEngine::new(
                    "left",
                    StorageArchitecture::BPlusTree,
                    false,
                ))
            },
            |context| {
                let fail_put = context.attempt.pair_index == 0 && context.repetition_index == 1;
                Ok(FakeEngine::new(
                    "right",
                    StorageArchitecture::LsmTree,
                    fail_put,
                ))
            },
            |_| ExperimentAttemptAdmission::Include,
        )
        .expect("captured batch");

        assert_eq!(report.batch.requested_pairs, 3);
        assert_eq!(report.batch.failed_pairs, 1);
        assert_eq!(report.batch.included_pairs, 2);
        assert_eq!(report.batch.excluded_pairs, 0);
        assert_eq!(report.comparison_failures.len(), 1);
        let sidecar = &report.comparison_failures[0];
        assert_eq!(sidecar.context.pair_index, 0);
        assert_eq!(
            sidecar.context.pair_order,
            CounterbalancedPairOrder::LeftThenRightFirst
        );
        assert_eq!(sidecar.context.pair_order, sidecar.failure.pair_order);
        assert_eq!(sidecar.failure.repetition_index, 1);
        assert!(sidecar.failure.completed_first.is_some());
        assert_eq!(sidecar.failure.ordered_failure.error_class, ErrorClass::Io);
        let sample = sidecar
            .failure
            .ordered_failure
            .right_operational_timing
            .compaction_stall_failure_samples[0];
        assert_eq!(sample.measured_step_index, Some(0));
        assert_eq!(sample.duration_ns, 19);
        assert_eq!(sample.error_class, ErrorClass::Io);
        assert_eq!(report.batch.attempts[0].context, sidecar.context);
        assert_eq!(
            report.batch.attempts[2].context.pair_order,
            CounterbalancedPairOrder::LeftThenRightFirst,
            "pair order repeats by pair 2, so pair index is required for an unambiguous join"
        );
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
                        duration_ns: 19,
                        work: None,
                        error_class: ErrorClass::Io,
                    });
                return Err(DbError::Io(std::io::Error::other(
                    "injected captured batch compaction failure",
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
