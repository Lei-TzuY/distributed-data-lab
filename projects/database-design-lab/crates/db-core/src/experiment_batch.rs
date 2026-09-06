use serde::{Deserialize, Serialize};

use crate::{
    compare_experiment_trace_counterbalanced, AmplificationInstrumented,
    CounterbalancedExperimentComparisonReport, CounterbalancedPairOrder, DbError, ErrorClass,
    ExperimentTrace, KvEngine, OperationalTimingInstrumented,
};

/// Maximum number of counterbalanced pairs accepted by one batch request.
pub const MAX_EXPERIMENT_BATCH_PAIRS: u32 = 10_000;

/// Stable inclusion state for one requested counterbalanced pair.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ExperimentAttemptDisposition {
    /// The pair completed successfully and is eligible for later distribution analysis.
    Included,
    /// The pair was attempted but failed before a valid comparison report was produced.
    Failed,
    /// The pair was deliberately skipped before engine creation under a caller-supplied protocol rule.
    Excluded,
}

/// Caller decision made before one pair starts.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ExperimentAttemptAdmission {
    /// Execute the pair normally.
    Include,
    /// Skip the pair and retain a non-empty exclusion reason in the ledger.
    Exclude { reason: String },
}

/// Context supplied to the admission callback and fresh-engine factories.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct ExperimentAttemptContext {
    /// Zero-based pair index within the requested batch.
    pub pair_index: u32,
    /// Which whole-run order executes first in this pair.
    pub pair_order: CounterbalancedPairOrder,
}

/// Context supplied for each fresh engine instance created inside one included pair.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct ExperimentInstanceContext {
    /// Pair-level provenance.
    pub attempt: ExperimentAttemptContext,
    /// Zero for the first ordered comparison in the pair, one for the second.
    pub repetition_index: u8,
}

/// Stable side identity for one fresh engine factory.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ExperimentEngineRole {
    Left,
    Right,
}

/// Stage at which a requested pair became unsuccessful.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ExperimentAttemptFailureStage {
    /// A fresh engine instance could not be created before its ordered comparison started.
    EngineFactory,
    /// Engine creation succeeded, but setup/measured execution or evidence collection failed.
    Comparison,
}

/// Stable failure evidence retained instead of silently dropping an unsuccessful repetition.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ExperimentAttemptFailure {
    /// Broad stage at which the pair failed.
    pub stage: ExperimentAttemptFailureStage,
    /// Factory side when `stage == engine_factory`; absent for comparison/runtime failures.
    pub engine_role: Option<ExperimentEngineRole>,
    /// Zero or one for a factory failure inside the counterbalanced pair; absent otherwise.
    pub repetition_index: Option<u8>,
    /// Stable coarse error class suitable for aggregation without parsing error text.
    pub class: ErrorClass,
    /// Detailed human-readable error retained for diagnosis.
    pub message: String,
}

/// One requested counterbalanced pair in the batch ledger.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ExperimentAttemptRecord {
    pub context: ExperimentAttemptContext,
    pub disposition: ExperimentAttemptDisposition,
    /// Present only for `included` attempts.
    pub report: Option<CounterbalancedExperimentComparisonReport>,
    /// Present only for `failed` attempts.
    pub failure: Option<ExperimentAttemptFailure>,
    /// Present only for `excluded` attempts.
    pub exclusion_reason: Option<String>,
}

/// Complete non-lossy ledger for one repeated counterbalanced experiment request.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct CounterbalancedExperimentBatchReport {
    pub trace: ExperimentTrace,
    pub pair_seed: u64,
    pub requested_pairs: u32,
    pub included_pairs: u32,
    pub failed_pairs: u32,
    pub excluded_pairs: u32,
    pub attempts: Vec<ExperimentAttemptRecord>,
}

/// Runs repeated fresh counterbalanced pairs while retaining included, failed, and excluded attempts.
///
/// The low bit of `pair_seed` selects the first pair order. Later pairs strictly alternate that
/// provenance, so any batch with at least two pairs cannot accidentally run every AB/BA pair in the
/// same outer order. Each included pair still contains one left-then-right and one right-then-left
/// comparison as guaranteed by `compare_experiment_trace_counterbalanced`.
///
/// Failures are captured into the returned ledger and do not abort later pairs. This is deliberate:
/// callers can archive the complete sampling process instead of constructing latency distributions
/// from successes only. Exclusion happens before engine creation and requires a non-empty reason.
pub fn run_counterbalanced_experiment_batch<L, R, MakeLeft, MakeRight, Admit>(
    trace: &ExperimentTrace,
    pair_seed: u64,
    requested_pairs: u32,
    mut make_left: MakeLeft,
    mut make_right: MakeRight,
    mut admit: Admit,
) -> std::result::Result<CounterbalancedExperimentBatchReport, DbError>
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
    let mut included_pairs = 0_u32;
    let mut failed_pairs = 0_u32;
    let mut excluded_pairs = 0_u32;

    for pair_index in 0..requested_pairs {
        let pair_order = pair_order(pair_seed, pair_index);
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
                let result = compare_experiment_trace_counterbalanced(
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
                        attempts.push(ExperimentAttemptRecord {
                            context,
                            disposition: ExperimentAttemptDisposition::Included,
                            report: Some(report),
                            failure: None,
                            exclusion_reason: None,
                        });
                    }
                    Err(error) => {
                        failed_pairs = failed_pairs.saturating_add(1);
                        let failure = left_factory_failure
                            .or(right_factory_failure)
                            .unwrap_or_else(|| ExperimentAttemptFailure {
                                stage: ExperimentAttemptFailureStage::Comparison,
                                engine_role: None,
                                repetition_index: None,
                                class: error.class(),
                                message: error.to_string(),
                            });
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

    Ok(CounterbalancedExperimentBatchReport {
        trace: trace.clone(),
        pair_seed,
        requested_pairs,
        included_pairs,
        failed_pairs,
        excluded_pairs,
        attempts,
    })
}

fn pair_order(seed: u64, pair_index: u32) -> CounterbalancedPairOrder {
    if ((seed & 1) ^ u64::from(pair_index & 1)) == 0 {
        CounterbalancedPairOrder::LeftThenRightFirst
    } else {
        CounterbalancedPairOrder::RightThenLeftFirst
    }
}

#[cfg(test)]
mod tests {
    use std::cell::{Cell, RefCell};
    use std::collections::BTreeMap;
    use std::rc::Rc;

    use super::{
        run_counterbalanced_experiment_batch, ExperimentAttemptAdmission,
        ExperimentAttemptDisposition, ExperimentAttemptFailureStage, ExperimentEngineRole,
    };
    use crate::{
        generate_experiment_trace, AmplificationInstrumented, AmplificationRatio,
        AmplificationReport, ConcurrencyMode, CounterbalancedPairOrder, CrashRecovery, DbError,
        DistributionMode, EngineCapabilities, ErrorClass, ExperimentGeneratorConfig,
        ExperimentProfile, KvEngine, LogicalModel, OperationalTimingInstrumented,
        OperationalTimingReport, Persistence, ReadWorkUnit, Result, StorageArchitecture,
        StructuralReadAmplification, MAX_KEY_BYTES, MAX_VALUE_BYTES,
    };

    #[test]
    fn batch_alternates_outer_pair_order_and_uses_fresh_instances() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 7,
            profile: ExperimentProfile::RandomWrite,
            operations: 2,
            key_space: 8,
            value_bytes: 4,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");
        let contexts = Rc::new(RefCell::new(Vec::new()));
        let left_creations = Rc::new(Cell::new(0_u32));
        let right_creations = Rc::new(Cell::new(0_u32));

        let report = run_counterbalanced_experiment_batch(
            &trace,
            1,
            3,
            {
                let contexts = Rc::clone(&contexts);
                let creations = Rc::clone(&left_creations);
                move |context| {
                    contexts.borrow_mut().push(("left", context));
                    creations.set(creations.get() + 1);
                    Ok(FakeEngine::new("left", StorageArchitecture::BPlusTree))
                }
            },
            {
                let contexts = Rc::clone(&contexts);
                let creations = Rc::clone(&right_creations);
                move |context| {
                    contexts.borrow_mut().push(("right", context));
                    creations.set(creations.get() + 1);
                    Ok(FakeEngine::new("right", StorageArchitecture::LsmTree))
                }
            },
            |_| ExperimentAttemptAdmission::Include,
        )
        .expect("batch");

        assert_eq!(report.included_pairs, 3);
        assert_eq!(report.failed_pairs, 0);
        assert_eq!(report.excluded_pairs, 0);
        assert_eq!(left_creations.get(), 6);
        assert_eq!(right_creations.get(), 6);
        assert_eq!(
            report
                .attempts
                .iter()
                .map(|attempt| attempt.context.pair_order)
                .collect::<Vec<_>>(),
            vec![
                CounterbalancedPairOrder::RightThenLeftFirst,
                CounterbalancedPairOrder::LeftThenRightFirst,
                CounterbalancedPairOrder::RightThenLeftFirst,
            ]
        );
        assert!(contexts
            .borrow()
            .iter()
            .all(|(_, context)| context.repetition_index <= 1));
    }

    #[test]
    fn batch_retains_failure_and_continues_later_pairs() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 9,
            profile: ExperimentProfile::RandomWrite,
            operations: 1,
            key_space: 4,
            value_bytes: 1,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");

        let report = run_counterbalanced_experiment_batch(
            &trace,
            0,
            2,
            |context| {
                if context.attempt.pair_index == 0 && context.repetition_index == 0 {
                    Err(DbError::InvalidInput(
                        "synthetic factory failure".to_owned(),
                    ))
                } else {
                    Ok(FakeEngine::new("left", StorageArchitecture::BPlusTree))
                }
            },
            |_| Ok(FakeEngine::new("right", StorageArchitecture::LsmTree)),
            |_| ExperimentAttemptAdmission::Include,
        )
        .expect("batch ledger survives failed pair");

        assert_eq!(report.included_pairs, 1);
        assert_eq!(report.failed_pairs, 1);
        assert_eq!(report.attempts.len(), 2);
        assert_eq!(
            report.attempts[0].disposition,
            ExperimentAttemptDisposition::Failed
        );
        let failure = report.attempts[0]
            .failure
            .as_ref()
            .expect("failure evidence");
        assert_eq!(failure.stage, ExperimentAttemptFailureStage::EngineFactory);
        assert_eq!(failure.engine_role, Some(ExperimentEngineRole::Left));
        assert_eq!(failure.repetition_index, Some(0));
        assert_eq!(failure.class, ErrorClass::InvalidInput);
        assert!(failure.message.contains("synthetic factory failure"));
        assert_eq!(
            report.attempts[1].disposition,
            ExperimentAttemptDisposition::Included
        );
        assert!(report.attempts[1].report.is_some());
    }

    #[test]
    fn comparison_failure_is_distinguished_from_factory_failure() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 10,
            profile: ExperimentProfile::PointRead,
            operations: 1,
            key_space: 1,
            value_bytes: 1,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");

        let report = run_counterbalanced_experiment_batch(
            &trace,
            0,
            1,
            |_| Ok(FakeEngine::new("left", StorageArchitecture::BPlusTree)),
            |_| {
                let mut engine = FakeEngine::new("right", StorageArchitecture::LsmTree);
                engine
                    .map
                    .insert(0_u64.to_be_bytes().to_vec(), b"preexisting".to_vec());
                Ok(engine)
            },
            |_| ExperimentAttemptAdmission::Include,
        )
        .expect("comparison failure is ledger evidence");

        assert_eq!(report.failed_pairs, 1);
        let failure = report.attempts[0]
            .failure
            .as_ref()
            .expect("failure evidence");
        assert_eq!(failure.stage, ExperimentAttemptFailureStage::Comparison);
        assert_eq!(failure.engine_role, None);
        assert_eq!(failure.repetition_index, None);
        assert_eq!(failure.class, ErrorClass::InvalidInput);
        assert!(failure.message.contains("setup step 0"));
    }

    #[test]
    fn exclusion_is_recorded_before_any_engine_is_created() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 11,
            profile: ExperimentProfile::PointRead,
            operations: 1,
            key_space: 1,
            value_bytes: 1,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");
        let creations = Rc::new(Cell::new(0_u32));

        let report = run_counterbalanced_experiment_batch(
            &trace,
            0,
            2,
            {
                let creations = Rc::clone(&creations);
                move |_| {
                    creations.set(creations.get() + 1);
                    Ok(FakeEngine::new("left", StorageArchitecture::BPlusTree))
                }
            },
            {
                let creations = Rc::clone(&creations);
                move |_| {
                    creations.set(creations.get() + 1);
                    Ok(FakeEngine::new("right", StorageArchitecture::LsmTree))
                }
            },
            |context| {
                if context.pair_index == 0 {
                    ExperimentAttemptAdmission::Exclude {
                        reason: "cache protocol not satisfied".to_owned(),
                    }
                } else {
                    ExperimentAttemptAdmission::Include
                }
            },
        )
        .expect("batch");

        assert_eq!(report.excluded_pairs, 1);
        assert_eq!(report.included_pairs, 1);
        assert_eq!(creations.get(), 4);
        assert_eq!(
            report.attempts[0].disposition,
            ExperimentAttemptDisposition::Excluded
        );
        assert_eq!(
            report.attempts[0].exclusion_reason.as_deref(),
            Some("cache protocol not satisfied")
        );
    }

    #[test]
    fn empty_exclusion_reason_fails_closed() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 13,
            profile: ExperimentProfile::RandomWrite,
            operations: 1,
            key_space: 1,
            value_bytes: 1,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");
        let error = run_counterbalanced_experiment_batch::<FakeEngine, FakeEngine, _, _, _>(
            &trace,
            0,
            1,
            |_| Ok(FakeEngine::new("left", StorageArchitecture::BPlusTree)),
            |_| Ok(FakeEngine::new("right", StorageArchitecture::LsmTree)),
            |_| ExperimentAttemptAdmission::Exclude {
                reason: "   ".to_owned(),
            },
        )
        .expect_err("blank reason must fail");
        assert!(error.to_string().contains("non-empty reason"));
    }

    struct FakeEngine {
        name: &'static str,
        architecture: StorageArchitecture,
        map: BTreeMap<Vec<u8>, Vec<u8>>,
    }

    impl FakeEngine {
        fn new(name: &'static str, architecture: StorageArchitecture) -> Self {
            Self {
                name,
                architecture,
                map: BTreeMap::new(),
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
            Ok(())
        }
    }

    impl OperationalTimingInstrumented for FakeEngine {
        fn reset_operational_timing(&mut self) {}

        fn set_operational_step_index(&mut self, _: Option<u64>) {}

        fn operational_timing_report(&self) -> OperationalTimingReport {
            OperationalTimingReport::default()
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
                    ratio: AmplificationRatio {
                        numerator: 0,
                        denominator: 0,
                    },
                    unit: point_unit,
                },
                range_read: StructuralReadAmplification {
                    ratio: AmplificationRatio {
                        numerator: 0,
                        denominator: 0,
                    },
                    unit: range_unit,
                },
                data_write_bytes_per_logical_byte: AmplificationRatio {
                    numerator: 0,
                    denominator: 0,
                },
                primary_structure_bytes_per_live_byte: AmplificationRatio {
                    numerator: 0,
                    denominator: 0,
                },
            })
        }
    }
}
