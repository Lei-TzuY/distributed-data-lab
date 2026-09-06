use serde::{Deserialize, Serialize};

use crate::{
    compare_experiment_trace_ordered, AmplificationInstrumented, DbError, ExperimentExecutionOrder,
    ExperimentTrace, KvEngine, OperationalTimingInstrumented, OrderedExperimentComparisonReport,
    Result,
};

/// Which whole-run engine order is executed first in one two-run counterbalanced pair.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CounterbalancedPairOrder {
    /// Execute left-then-right first, then right-then-left.
    LeftThenRightFirst,
    /// Execute right-then-left first, then left-then-right.
    RightThenLeftFirst,
}

impl CounterbalancedPairOrder {
    const fn first(self) -> ExperimentExecutionOrder {
        match self {
            Self::LeftThenRightFirst => ExperimentExecutionOrder::LeftThenRight,
            Self::RightThenLeftFirst => ExperimentExecutionOrder::RightThenLeft,
        }
    }

    const fn second(self) -> ExperimentExecutionOrder {
        match self {
            Self::LeftThenRightFirst => ExperimentExecutionOrder::RightThenLeft,
            Self::RightThenLeftFirst => ExperimentExecutionOrder::LeftThenRight,
        }
    }
}

/// Two fresh ordered comparisons containing one run of each possible whole-engine order.
///
/// No durations are aggregated here. Both raw runs remain available so downstream methodology can
/// retain per-run provenance and apply an explicitly documented estimator only after host/cache/device
/// controls and exclusion rules are frozen.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct CounterbalancedExperimentComparisonReport {
    pub pair_order: CounterbalancedPairOrder,
    pub first: OrderedExperimentComparisonReport,
    pub second: OrderedExperimentComparisonReport,
}

/// Runs one fresh AB/BA (or BA/AB) counterbalanced experiment pair.
///
/// Each factory is called exactly once per ordered comparison, so all four engine instances are fresh.
/// The two runs must expose identical left/right capabilities and identical measured logical outcomes;
/// otherwise the pair fails closed instead of archiving structurally incomparable repetitions.
pub fn compare_experiment_trace_counterbalanced<L, R, MakeLeft, MakeRight>(
    trace: &ExperimentTrace,
    pair_order: CounterbalancedPairOrder,
    mut make_left: MakeLeft,
    mut make_right: MakeRight,
) -> Result<CounterbalancedExperimentComparisonReport>
where
    L: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
    R: KvEngine + AmplificationInstrumented + OperationalTimingInstrumented,
    MakeLeft: FnMut() -> Result<L>,
    MakeRight: FnMut() -> Result<R>,
{
    trace.validate()?;

    let mut first_left = make_left()?;
    let mut first_right = make_right()?;
    let first = compare_experiment_trace_ordered(
        &mut first_left,
        &mut first_right,
        trace,
        pair_order.first(),
    )?;

    let mut second_left = make_left()?;
    let mut second_right = make_right()?;
    let second = compare_experiment_trace_ordered(
        &mut second_left,
        &mut second_right,
        trace,
        pair_order.second(),
    )?;

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

    Ok(CounterbalancedExperimentComparisonReport {
        pair_order,
        first,
        second,
    })
}

#[cfg(test)]
mod tests {
    use std::cell::{Cell, RefCell};
    use std::collections::BTreeMap;
    use std::rc::Rc;

    use super::{
        compare_experiment_trace_counterbalanced, CounterbalancedExperimentComparisonReport,
        CounterbalancedPairOrder,
    };
    use crate::{
        generate_experiment_trace, AmplificationInstrumented, AmplificationRatio,
        AmplificationReport, ConcurrencyMode, CrashRecovery, DistributionMode, EngineCapabilities,
        ExperimentExecutionOrder, ExperimentGeneratorConfig, ExperimentProfile, KvEngine,
        LogicalModel, OperationalTimingInstrumented, OperationalTimingReport, Persistence,
        ReadWorkUnit, Result, StorageArchitecture, StructuralReadAmplification, MAX_KEY_BYTES,
        MAX_VALUE_BYTES,
    };

    #[test]
    fn pair_uses_fresh_engines_and_reverses_whole_run_order() {
        let trace = generate_experiment_trace(ExperimentGeneratorConfig {
            seed: 0x2026_0829,
            profile: ExperimentProfile::RandomWrite,
            operations: 2,
            key_space: 8,
            value_bytes: 4,
            range_limit: 1,
            reopen_every: None,
        })
        .expect("generate trace");
        let events = Rc::new(RefCell::new(Vec::new()));
        let left_creations = Rc::new(Cell::new(0_u32));
        let right_creations = Rc::new(Cell::new(0_u32));

        let report = compare_experiment_trace_counterbalanced(
            &trace,
            CounterbalancedPairOrder::LeftThenRightFirst,
            {
                let events = Rc::clone(&events);
                let creations = Rc::clone(&left_creations);
                move || {
                    creations.set(creations.get() + 1);
                    Ok(FakeEngine::new(
                        "left",
                        StorageArchitecture::BPlusTree,
                        Rc::clone(&events),
                    ))
                }
            },
            {
                let events = Rc::clone(&events);
                let creations = Rc::clone(&right_creations);
                move || {
                    creations.set(creations.get() + 1);
                    Ok(FakeEngine::new(
                        "right",
                        StorageArchitecture::LsmTree,
                        Rc::clone(&events),
                    ))
                }
            },
        )
        .expect("counterbalanced pair");

        assert_eq!(left_creations.get(), 2);
        assert_eq!(right_creations.get(), 2);
        assert_eq!(
            report.first.execution_order,
            ExperimentExecutionOrder::LeftThenRight
        );
        assert_eq!(
            report.second.execution_order,
            ExperimentExecutionOrder::RightThenLeft
        );
        assert_eq!(
            report.first.comparison.outcomes,
            report.second.comparison.outcomes
        );
        assert_eq!(
            events.borrow().as_slice(),
            ["left", "left", "right", "right", "right", "right", "left", "left"]
        );
    }

    #[test]
    fn pair_order_serialization_freezes_repetition_provenance() {
        assert_eq!(
            serde_json::to_string(&CounterbalancedPairOrder::RightThenLeftFirst)
                .expect("serialize pair order"),
            "\"right_then_left_first\""
        );
    }

    #[allow(dead_code)]
    fn assert_report_is_publicly_nameable(_: CounterbalancedExperimentComparisonReport) {}

    struct FakeEngine {
        name: &'static str,
        architecture: StorageArchitecture,
        map: BTreeMap<Vec<u8>, Vec<u8>>,
        events: Rc<RefCell<Vec<&'static str>>>,
    }

    impl FakeEngine {
        fn new(
            name: &'static str,
            architecture: StorageArchitecture,
            events: Rc<RefCell<Vec<&'static str>>>,
        ) -> Self {
            Self {
                name,
                architecture,
                map: BTreeMap::new(),
                events,
            }
        }

        fn record(&self) {
            self.events.borrow_mut().push(self.name);
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
