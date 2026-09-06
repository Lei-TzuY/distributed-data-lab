use db_core::{
    AmplificationInstrumented, AmplificationReport, ConcurrencyMode, CrashRecovery, DbError,
    DistributionMode, EngineCapabilities, KvEngine, LogicalModel, OperationalTimingFailureSample,
    OperationalTimingInstrumented, OperationalTimingReport, OperationalTimingSample,
    OperationalWork, OperationalWorkUnit, Persistence, StorageArchitecture,
};
use std::time::Instant;

use super::{BPlusTree, MAX_TREE_KEY_BYTES, MAX_TREE_VALUE_BYTES};
use crate::{BtreeError, PAGE_SIZE};

fn common_error(error: BtreeError) -> DbError {
    match error {
        BtreeError::InvalidInput(reason) => DbError::InvalidInput(reason),
        BtreeError::Io(error) => DbError::Io(error),
        BtreeError::Corruption { offset, reason } => DbError::Corruption { offset, reason },
        BtreeError::UnsupportedVersion { found, supported } => DbError::UnsupportedVersion {
            format: "B+ tree page file",
            found,
            supported,
        },
        BtreeError::Poisoned => DbError::Poisoned,
    }
}

impl KvEngine for BPlusTree {
    fn capabilities(&self) -> EngineCapabilities {
        EngineCapabilities {
            name: "bplus-tree-v1",
            logical_model: LogicalModel::KeyValue,
            storage_architecture: StorageArchitecture::BPlusTree,
            concurrency: ConcurrencyMode::CallerSerialized,
            persistence: Persistence::Persistent,
            crash_recovery: CrashRecovery::MirroredCopyOnWritePages,
            distribution: DistributionMode::Standalone,
            ordered_range_scan: true,
            max_key_bytes: MAX_TREE_KEY_BYTES,
            max_value_bytes: MAX_TREE_VALUE_BYTES,
        }
    }

    fn put(&mut self, key: &[u8], value: &[u8]) -> db_core::Result<Option<Vec<u8>>> {
        BPlusTree::put(self, key, value).map_err(common_error)
    }

    fn get(&mut self, key: &[u8]) -> db_core::Result<Option<Vec<u8>>> {
        BPlusTree::get(self, key).map_err(common_error)
    }

    fn delete(&mut self, key: &[u8]) -> db_core::Result<Option<Vec<u8>>> {
        BPlusTree::delete(self, key).map_err(common_error)
    }

    fn range_scan(
        &mut self,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
    ) -> db_core::Result<Vec<(Vec<u8>, Vec<u8>)>> {
        BPlusTree::range_scan(self, start, end, limit).map_err(common_error)
    }

    fn reopen(&mut self) -> db_core::Result<()> {
        let path = self.path().to_path_buf();
        let instrumentation = self.instrumentation;
        let mut operational_timing = self.operational_timing.clone();
        let operational_step_index = self.operational_step_index;
        let started = Instant::now();
        match BPlusTree::open(&path, self.cache_capacity) {
            Ok(mut reopened) => {
                let page_accesses = reopened.pager.read_page_calls();
                let duration_ns = u64::try_from(started.elapsed().as_nanos()).unwrap_or(u64::MAX);
                operational_timing.reopen_ns.push(duration_ns);
                operational_timing
                    .reopen_samples
                    .push(OperationalTimingSample {
                        measured_step_index: operational_step_index,
                        duration_ns,
                        work: OperationalWork {
                            unit: OperationalWorkUnit::BtreePageAccess,
                            units_examined: page_accesses,
                            bytes_examined: page_accesses.saturating_mul(PAGE_SIZE as u64),
                        },
                    });
                reopened.instrumentation = instrumentation;
                reopened.operational_timing = operational_timing;
                reopened.operational_step_index = operational_step_index;
                *self = reopened;
                Ok(())
            }
            Err(error) => {
                let error = common_error(error);
                let duration_ns = u64::try_from(started.elapsed().as_nanos()).unwrap_or(u64::MAX);
                operational_timing
                    .reopen_failure_samples
                    .push(OperationalTimingFailureSample {
                        measured_step_index: operational_step_index,
                        duration_ns,
                        work: None,
                        error_class: error.class(),
                    });
                self.operational_timing = operational_timing;
                self.pager.poisoned = true;
                Err(error)
            }
        }
    }
}

impl OperationalTimingInstrumented for BPlusTree {
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

impl AmplificationInstrumented for BPlusTree {
    fn reset_amplification(&mut self) {
        self.reset_instrumentation();
    }

    fn amplification_report(&mut self) -> db_core::Result<AmplificationReport> {
        BPlusTree::amplification_report(self).map_err(common_error)
    }
}

#[cfg(test)]
mod tests {
    use db_core::{
        compare_workload, ByteString, ErrorClass, KvEngine, OperationalTimingInstrumented,
        Workload, WorkloadStep, MAX_KEY_BYTES, MAX_VALUE_BYTES, WORKLOAD_FORMAT_VERSION,
    };
    use db_storage_memory::MemoryEngine;
    use tempfile::tempdir;

    use super::{BPlusTree, MAX_TREE_KEY_BYTES, MAX_TREE_VALUE_BYTES};

    #[test]
    fn capabilities_match_the_common_point_contract() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("capabilities.db");
        let tree = BPlusTree::create_new(&path, 4).expect("create tree");
        let capabilities = tree.capabilities();

        assert_eq!(MAX_TREE_KEY_BYTES, MAX_KEY_BYTES);
        assert_eq!(MAX_TREE_VALUE_BYTES, MAX_VALUE_BYTES);
        assert_eq!(capabilities.max_key_bytes, MAX_KEY_BYTES);
        assert_eq!(capabilities.max_value_bytes, MAX_VALUE_BYTES);
        assert!(capabilities.ordered_range_scan);
    }

    #[test]
    fn failed_reopen_retains_step_duration_and_error_class_without_inventing_work() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("failed-reopen.db");
        let mut tree = BPlusTree::create_new(&path, 4).expect("create tree");
        tree.put(b"key", b"value").expect("seed tree");
        tree.reset_operational_timing();
        tree.set_operational_step_index(Some(17));

        tree.cache_capacity = 0;
        let error =
            KvEngine::reopen(&mut tree).expect_err("zero cache capacity must reject reopen");
        assert_eq!(error.class(), ErrorClass::InvalidInput);

        let timing = tree.operational_timing_report();
        assert!(timing.reopen_ns.is_empty());
        assert!(timing.reopen_samples.is_empty());
        assert_eq!(timing.reopen_failure_samples.len(), 1);
        let failure = timing.reopen_failure_samples[0];
        assert_eq!(failure.measured_step_index, Some(17));
        assert_eq!(failure.error_class, ErrorClass::InvalidInput);
        assert_eq!(failure.work, None);
        assert!(timing.compaction_stall_failure_samples.is_empty());
        assert_eq!(
            KvEngine::get(&mut tree, b"key")
                .expect_err("failed reopen poisons handle")
                .class(),
            ErrorClass::Poisoned
        );
    }

    #[test]
    fn common_ordered_ranges_match_memory_after_splits_reopen_and_delete() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("range-differential.db");
        let mut reference = MemoryEngine::new();
        let mut candidate = BPlusTree::create_new(&path, 8).expect("create B+ tree");

        for index in 0..96_u16 {
            let key = index.to_be_bytes().to_vec();
            let value = if index == 48 {
                vec![0x7c; 64 * 1024]
            } else {
                vec![(index & 0xff) as u8; 160]
            };
            reference.put(&key, &value).expect("reference insert");
            candidate.put(&key, &value).expect("candidate insert");
        }
        candidate.reopen().expect("candidate reopen");
        reference.reopen().expect("reference reopen");
        for index in [0_u16, 17, 47, 49, 95] {
            let key = index.to_be_bytes();
            assert_eq!(
                reference.delete(&key).expect("reference delete"),
                candidate.delete(&key).expect("candidate delete")
            );
        }

        let cases = [
            (0_u16, Some(96_u16), 200_usize),
            (16, Some(52), 11),
            (48, Some(49), 8),
            (80, None, 7),
            (30, Some(30), 10),
        ];
        for (start, end, limit) in cases {
            let start = start.to_be_bytes();
            let end_bytes = end.map(u16::to_be_bytes);
            let end = end_bytes.as_ref().map(<[u8; 2]>::as_slice);
            assert_eq!(
                reference
                    .range_scan(&start, end, limit)
                    .expect("reference range"),
                candidate
                    .range_scan(&start, end, limit)
                    .expect("candidate range")
            );
        }
    }

    #[test]
    fn common_differential_harness_covers_limits_delete_and_reopen() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("differential.db");
        let mut reference = MemoryEngine::new();
        let mut candidate = BPlusTree::create_new(&path, 16).expect("create B+ tree");

        let mut maximum_key = vec![0xa5; MAX_KEY_BYTES];
        maximum_key[MAX_KEY_BYTES - 1] = 0xfe;
        let maximum_value = (0..MAX_VALUE_BYTES)
            .map(|index| ((index * 29 + 7) & 0xff) as u8)
            .collect::<Vec<_>>();
        let binary_key = vec![0x00, 0xff, 0x10, 0x80];

        let workload = Workload {
            format_version: WORKLOAD_FORMAT_VERSION,
            seed: None,
            steps: vec![
                WorkloadStep::Put {
                    key: ByteString::from(Vec::new()),
                    value: ByteString::from(Vec::new()),
                },
                WorkloadStep::Put {
                    key: ByteString::from(binary_key.clone()),
                    value: ByteString::from(b"binary".to_vec()),
                },
                WorkloadStep::Put {
                    key: ByteString::from(maximum_key.clone()),
                    value: ByteString::from(maximum_value.clone()),
                },
                WorkloadStep::Get {
                    key: ByteString::from(maximum_key.clone()),
                },
                WorkloadStep::Reopen,
                WorkloadStep::Get {
                    key: ByteString::from(Vec::new()),
                },
                WorkloadStep::Put {
                    key: ByteString::from(maximum_key.clone()),
                    value: ByteString::from(b"replacement".to_vec()),
                },
                WorkloadStep::Delete {
                    key: ByteString::from(binary_key.clone()),
                },
                WorkloadStep::Delete {
                    key: ByteString::from(binary_key),
                },
                WorkloadStep::Reopen,
                WorkloadStep::Get {
                    key: ByteString::from(maximum_key.clone()),
                },
                WorkloadStep::Delete {
                    key: ByteString::from(maximum_key),
                },
                WorkloadStep::Reopen,
            ],
        };

        let report = compare_workload(&mut reference, &mut candidate, &workload)
            .expect("B+ tree matches common memory oracle");
        assert_eq!(report.steps_checked, workload.steps.len());
    }
}
