use serde::{Deserialize, Serialize};

use crate::{
    execute_experiment_step, validate_experiment_compatibility, AmplificationInstrumented, DbError,
    EngineCapabilities, ExperimentComparisonReport, ExperimentEngineEvidence, ExperimentOutcome,
    ExperimentStep, ExperimentTrace, KvEngine, OperationalTimingInstrumented, Result,
    MAX_EXPERIMENT_OUTCOME_PAYLOAD_BYTES,
};

/// Whole-run execution order for a two-engine experiment comparison.
///
/// The selected first engine completes setup and the entire measured window before the second engine
/// starts. `left` and `right` remain stable report identities regardless of run order.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ExperimentExecutionOrder {
    /// Execute the left engine completely, then execute the right engine.
    LeftThenRight,
    /// Execute the right engine completely, then execute the left engine.
    RightThenLeft,
}

/// Comparison evidence carrying the whole-run execution order as explicit provenance.
///
/// The nested comparison retains the existing `ExperimentComparisonReport` schema so callers that do
/// not need order-aware methodology can continue using `compare_experiment_trace` unchanged.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct OrderedExperimentComparisonReport {
    pub execution_order: ExperimentExecutionOrder,
    pub comparison: ExperimentComparisonReport,
}

struct FirstEngineRun {
    setup_outcomes: Vec<ExperimentOutcome>,
    measured_outcomes: Vec<ExperimentOutcome>,
    evidence: ExperimentEngineEvidence,
}

/// Runs one shared trace with an explicit whole-engine execution order.
///
/// Unlike the legacy lockstep comparison, this function never alternates engines inside the measured
/// window. The first engine completes setup, resets instrumentation, and completes every measured step
/// before the second engine starts. The second engine's setup and measured outcomes are compared against
/// the first engine at the exact corresponding step, preserving fail-closed logical equivalence while
/// making AB/BA counterbalancing possible at the caller level.
pub fn compare_experiment_trace_ordered<L, R>(
    left: &mut L,
    right: &mut R,
    trace: &ExperimentTrace,
    execution_order: ExperimentExecutionOrder,
) -> Result<OrderedExperimentComparisonReport>
where
    L: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
    R: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
{
    trace.validate()?;
    let left_capabilities = left.capabilities();
    let right_capabilities = right.capabilities();
    validate_experiment_compatibility(
        left_capabilities,
        right_capabilities,
        trace.requires_ordered_range(),
    )?;

    let (left_evidence, right_evidence, outcomes) = match execution_order {
        ExperimentExecutionOrder::LeftThenRight => {
            let first = run_first_engine(left, trace, left_capabilities)?;
            let right_evidence = run_second_engine(
                right,
                trace,
                &first.setup_outcomes,
                &first.measured_outcomes,
                left_capabilities,
                right_capabilities,
            )?;
            (first.evidence, right_evidence, first.measured_outcomes)
        }
        ExperimentExecutionOrder::RightThenLeft => {
            let first = run_first_engine(right, trace, right_capabilities)?;
            let left_evidence = run_second_engine(
                left,
                trace,
                &first.setup_outcomes,
                &first.measured_outcomes,
                left_capabilities,
                right_capabilities,
            )?;
            (left_evidence, first.evidence, first.measured_outcomes)
        }
    };

    Ok(OrderedExperimentComparisonReport {
        execution_order,
        comparison: ExperimentComparisonReport {
            trace: trace.clone(),
            setup_steps_executed: trace.setup_steps.len(),
            measured_steps_executed: outcomes.len(),
            outcomes,
            left: left_evidence,
            right: right_evidence,
        },
    })
}

fn run_first_engine<E>(
    engine: &mut E,
    trace: &ExperimentTrace,
    capabilities: EngineCapabilities,
) -> Result<FirstEngineRun>
where
    E: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
{
    let mut setup_outcome_bytes = 0_u64;
    let mut setup_outcomes = Vec::with_capacity(trace.setup_steps.len());
    for step in &trace.setup_steps {
        let outcome = execute_experiment_step(engine, step)?;
        setup_outcome_bytes = checked_add_outcome_payload(
            setup_outcome_bytes,
            &outcome,
            "experiment setup outcomes",
        )?;
        setup_outcomes.push(outcome);
    }

    engine.reset_amplification();
    engine.reset_operational_timing();

    let mut measured_outcome_bytes = 0_u64;
    let mut measured_outcomes = Vec::with_capacity(trace.measured_steps.len());
    for (index, step) in trace.measured_steps.iter().enumerate() {
        let outcome = execute_measured_step(engine, step, index)?;
        measured_outcome_bytes = checked_add_outcome_payload(
            measured_outcome_bytes,
            &outcome,
            "experiment measured outcomes",
        )?;
        measured_outcomes.push(outcome);
    }

    Ok(FirstEngineRun {
        setup_outcomes,
        measured_outcomes,
        evidence: ExperimentEngineEvidence {
            capabilities,
            amplification: engine.amplification_report()?,
            operational_timing: engine.operational_timing_report(),
        },
    })
}

fn run_second_engine<E>(
    engine: &mut E,
    trace: &ExperimentTrace,
    expected_setup: &[ExperimentOutcome],
    expected_measured: &[ExperimentOutcome],
    left_capabilities: EngineCapabilities,
    right_capabilities: EngineCapabilities,
) -> Result<ExperimentEngineEvidence>
where
    E: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
{
    let mut setup_outcome_bytes = 0_u64;
    for (index, step) in trace.setup_steps.iter().enumerate() {
        let outcome = execute_experiment_step(engine, step)?;
        setup_outcome_bytes = checked_add_outcome_payload(
            setup_outcome_bytes,
            &outcome,
            "experiment setup outcomes",
        )?;
        if outcome != expected_setup[index] {
            return Err(logical_mismatch(
                "setup",
                index,
                left_capabilities,
                right_capabilities,
            ));
        }
    }

    engine.reset_amplification();
    engine.reset_operational_timing();

    let mut measured_outcome_bytes = 0_u64;
    for (index, step) in trace.measured_steps.iter().enumerate() {
        let outcome = execute_measured_step(engine, step, index)?;
        measured_outcome_bytes = checked_add_outcome_payload(
            measured_outcome_bytes,
            &outcome,
            "experiment measured outcomes",
        )?;
        if outcome != expected_measured[index] {
            return Err(logical_mismatch(
                "measured",
                index,
                left_capabilities,
                right_capabilities,
            ));
        }
    }

    Ok(ExperimentEngineEvidence {
        capabilities: engine.capabilities(),
        amplification: engine.amplification_report()?,
        operational_timing: engine.operational_timing_report(),
    })
}

fn execute_measured_step<E>(
    engine: &mut E,
    step: &ExperimentStep,
    index: usize,
) -> Result<ExperimentOutcome>
where
    E: KvEngine + OperationalTimingInstrumented,
{
    let index = u64::try_from(index).map_err(|_| {
        DbError::InvalidInput("measured experiment step index does not fit u64".to_owned())
    })?;
    engine.set_operational_step_index(Some(index));
    let result = execute_experiment_step(engine, step);
    engine.set_operational_step_index(None);
    result
}

fn logical_mismatch(
    phase: &str,
    index: usize,
    left: EngineCapabilities,
    right: EngineCapabilities,
) -> DbError {
    DbError::InvalidInput(format!(
        "experiment logical outcomes diverged at {phase} step {index} between {} and {}",
        left.name, right.name
    ))
}

fn checked_add_outcome_payload(total: u64, outcome: &ExperimentOutcome, kind: &str) -> Result<u64> {
    let next = match outcome {
        ExperimentOutcome::Put { previous }
        | ExperimentOutcome::Delete { previous }
        | ExperimentOutcome::Get { value: previous } => {
            previous.as_ref().map_or(Ok(0), |value| {
                u64::try_from(value.as_slice().len()).map_err(|_| {
                    DbError::InvalidInput("experiment outcome length does not fit u64".to_owned())
                })
            })?
        }
        ExperimentOutcome::RangeScan { rows } => rows.iter().try_fold(0_u64, |total, row| {
            let row_bytes =
                checked_payload_lengths(&[row.key.as_slice().len(), row.value.as_slice().len()])?;
            total.checked_add(row_bytes).ok_or_else(|| {
                DbError::InvalidInput("experiment outcome payload size overflowed".to_owned())
            })
        })?,
        ExperimentOutcome::Reopened => 0,
    };
    let total = total
        .checked_add(next)
        .ok_or_else(|| DbError::InvalidInput(format!("{kind} payload size overflowed")))?;
    if total > MAX_EXPERIMENT_OUTCOME_PAYLOAD_BYTES {
        return Err(DbError::InvalidInput(format!(
            "{kind} has {total} payload bytes; maximum is {MAX_EXPERIMENT_OUTCOME_PAYLOAD_BYTES}"
        )));
    }
    Ok(total)
}

fn checked_payload_lengths(lengths: &[usize]) -> Result<u64> {
    lengths.iter().try_fold(0_u64, |total, length| {
        let length = u64::try_from(*length).map_err(|_| {
            DbError::InvalidInput("experiment payload length does not fit u64".to_owned())
        })?;
        total
            .checked_add(length)
            .ok_or_else(|| DbError::InvalidInput("experiment payload size overflowed".to_owned()))
    })
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::collections::BTreeMap;
    use std::rc::Rc;

    use super::{
        compare_experiment_trace_ordered, ExperimentExecutionOrder,
        OrderedExperimentComparisonReport,
    };
    use crate::{
        generate_experiment_trace, AmplificationInstrumented, AmplificationRatio,
        AmplificationReport, ConcurrencyMode, CrashRecovery, DistributionMode, EngineCapabilities,
        ExperimentGeneratorConfig, ExperimentProfile, KvEngine, LogicalModel,
        OperationalTimingInstrumented, OperationalTimingReport, Persistence, ReadWorkUnit, Result,
        StorageArchitecture, StructuralReadAmplification, MAX_KEY_BYTES, MAX_VALUE_BYTES,
    };

    #[test]
    fn right_then_left_runs_the_complete_right_trace_before_left_starts() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 0x26_0829,
            profile: ExperimentProfile::Mixed,
            operations: 24,
            key_space: 12,
            value_bytes: 8,
            range_limit: 3,
            reopen_every: Some(7),
        })
        .expect("generate trace");
        let event_log = Rc::new(RefCell::new(Vec::new()));
        let mut left = FakeEngine::new(
            "left",
            StorageArchitecture::BPlusTree,
            Rc::clone(&event_log),
        );
        let mut right =
            FakeEngine::new("right", StorageArchitecture::LsmTree, Rc::clone(&event_log));

        let report = compare_experiment_trace_ordered(
            &mut left,
            &mut right,
            &trace,
            ExperimentExecutionOrder::RightThenLeft,
        )
        .expect("ordered comparison");

        assert_eq!(
            report.execution_order,
            ExperimentExecutionOrder::RightThenLeft
        );
        assert_eq!(report.comparison.outcomes.len(), trace.measured_steps.len());
        let events = event_log.borrow();
        let first_left = events
            .iter()
            .position(|engine| *engine == "left")
            .expect("left engine eventually runs");
        assert!(first_left > 0);
        assert!(events[..first_left].iter().all(|engine| *engine == "right"));
        assert!(events[first_left..].iter().all(|engine| *engine == "left"));
    }

    #[test]
    fn ordered_comparison_preserves_setup_divergence_detection() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 4,
            profile: ExperimentProfile::PointRead,
            operations: 1,
            key_space: 1,
            value_bytes: 4,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("point trace");
        let event_log = Rc::new(RefCell::new(Vec::new()));
        let mut left = FakeEngine::new(
            "left",
            StorageArchitecture::BPlusTree,
            Rc::clone(&event_log),
        );
        let mut right =
            FakeEngine::new("right", StorageArchitecture::LsmTree, Rc::clone(&event_log));
        right
            .map
            .insert(0_u64.to_be_bytes().to_vec(), b"preexisting".to_vec());

        let error = compare_experiment_trace_ordered(
            &mut left,
            &mut right,
            &trace,
            ExperimentExecutionOrder::LeftThenRight,
        )
        .expect_err("different setup outcomes must fail");
        assert!(error.to_string().contains("setup step 0"));
    }

    #[test]
    fn execution_order_serializes_as_stable_snake_case_provenance() {
        let encoded = serde_json::to_string(&ExperimentExecutionOrder::LeftThenRight)
            .expect("serialize execution order");
        assert_eq!(encoded, "\"left_then_right\"");
        let decoded: ExperimentExecutionOrder =
            serde_json::from_str("\"right_then_left\"").expect("deserialize execution order");
        assert_eq!(decoded, ExperimentExecutionOrder::RightThenLeft);
    }

    #[allow(dead_code)]
    fn assert_report_is_publicly_nameable(_: OrderedExperimentComparisonReport) {}

    struct FakeEngine {
        name: &'static str,
        architecture: StorageArchitecture,
        map: BTreeMap<Vec<u8>, Vec<u8>>,
        event_log: Rc<RefCell<Vec<&'static str>>>,
    }

    impl FakeEngine {
        fn new(
            name: &'static str,
            architecture: StorageArchitecture,
            event_log: Rc<RefCell<Vec<&'static str>>>,
        ) -> Self {
            Self {
                name,
                architecture,
                map: BTreeMap::new(),
                event_log,
            }
        }

        fn record(&self) {
            self.event_log.borrow_mut().push(self.name);
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
            self.record();
            Ok(self.map.insert(key.to_vec(), value.to_vec()))
        }

        fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
            self.record();
            Ok(self.map.get(key).cloned())
        }

        fn delete(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
            self.record();
            Ok(self.map.remove(key))
        }

        fn range_scan(
            &mut self,
            start: &[u8],
            end: Option<&[u8]>,
            limit: usize,
        ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
            self.record();
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
            self.record();
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
            Ok(AmplificationReport {
                point_read: StructuralReadAmplification {
                    ratio: AmplificationRatio {
                        numerator: 0,
                        denominator: 0,
                    },
                    unit: if self.architecture == StorageArchitecture::BPlusTree {
                        ReadWorkUnit::BtreePageAccess
                    } else {
                        ReadWorkUnit::LsmSstableConsult
                    },
                },
                range_read: StructuralReadAmplification {
                    ratio: AmplificationRatio {
                        numerator: 0,
                        denominator: 0,
                    },
                    unit: if self.architecture == StorageArchitecture::BPlusTree {
                        ReadWorkUnit::BtreePageAccess
                    } else {
                        ReadWorkUnit::LsmSstableVersionDecoded
                    },
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
