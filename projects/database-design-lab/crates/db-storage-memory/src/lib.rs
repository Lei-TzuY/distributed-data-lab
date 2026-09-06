//! Deterministic in-memory reference implementation of the common KV semantics.

use std::collections::BTreeMap;
use std::ops::Bound;

use db_core::{
    ConcurrencyMode, CrashRecovery, DistributionMode, EngineCapabilities, KvEngine, LogicalModel,
    Persistence, Result, StorageArchitecture, MAX_KEY_BYTES, MAX_VALUE_BYTES,
};

/// A deliberately simple oracle, not a performance baseline.
#[derive(Debug, Default)]
pub struct MemoryEngine {
    values: BTreeMap<Vec<u8>, Vec<u8>>,
}

impl MemoryEngine {
    /// Creates an empty reference engine.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Returns the number of live keys.
    #[must_use]
    pub fn len(&self) -> usize {
        self.values.len()
    }

    /// Returns whether no keys are live.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }
}

impl KvEngine for MemoryEngine {
    fn capabilities(&self) -> EngineCapabilities {
        EngineCapabilities {
            name: "memory-reference",
            logical_model: LogicalModel::KeyValue,
            storage_architecture: StorageArchitecture::InMemoryReference,
            concurrency: ConcurrencyMode::CallerSerialized,
            persistence: Persistence::Volatile,
            crash_recovery: CrashRecovery::None,
            distribution: DistributionMode::Standalone,
            ordered_range_scan: true,
            max_key_bytes: MAX_KEY_BYTES,
            max_value_bytes: MAX_VALUE_BYTES,
        }
    }

    fn put(&mut self, key: &[u8], value: &[u8]) -> Result<Option<Vec<u8>>> {
        db_core::validate_key_value(key, value)?;
        Ok(self.values.insert(key.to_vec(), value.to_vec()))
    }

    fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        db_core::validate_key(key)?;
        Ok(self.values.get(key).cloned())
    }

    fn delete(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        db_core::validate_key(key)?;
        Ok(self.values.remove(key))
    }

    fn range_scan(
        &mut self,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        db_core::validate_range_scan(start, end)?;
        if limit == 0 || end.is_some_and(|end| end == start) {
            return Ok(Vec::new());
        }
        let lower = Bound::Included(start.to_vec());
        let upper = end
            .map(|end| Bound::Excluded(end.to_vec()))
            .unwrap_or(Bound::Unbounded);
        Ok(self
            .values
            .range((lower, upper))
            .take(limit)
            .map(|(key, value)| (key.clone(), value.clone()))
            .collect())
    }

    fn reopen(&mut self) -> Result<()> {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use db_core::KvEngine;

    use super::MemoryEngine;

    #[test]
    fn overwrite_delete_and_reinsert_follow_common_semantics() {
        let mut engine = MemoryEngine::new();
        assert_eq!(engine.put(b"key", b"one").expect("initial put"), None);
        assert_eq!(
            engine.put(b"key", b"two").expect("overwrite"),
            Some(b"one".to_vec())
        );
        assert_eq!(
            engine.delete(b"key").expect("delete"),
            Some(b"two".to_vec())
        );
        assert_eq!(engine.delete(b"key").expect("delete missing"), None);
        assert_eq!(engine.put(b"key", b"").expect("reinsert empty"), None);
        engine.reopen().expect("reference reopen is a no-op");
        assert_eq!(engine.get(b"key").expect("get"), Some(Vec::new()));
        assert_eq!(
            engine.range_scan(b"", None, 8).expect("ordered range scan"),
            vec![(b"key".to_vec(), Vec::new())]
        );
    }
}
