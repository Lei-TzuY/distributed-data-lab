use std::collections::BTreeMap;

use db_core::{DbError, Result};

const ENTRY_OVERHEAD_BYTES: usize = 24;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct VersionedEntry {
    pub(super) sequence: u64,
    pub(super) value: Option<Vec<u8>>,
}

type FrozenSnapshot = (BTreeMap<Vec<u8>, VersionedEntry>, u64);

#[derive(Debug, Clone, Default)]
struct MemTable {
    entries: BTreeMap<Vec<u8>, VersionedEntry>,
    approximate_bytes: usize,
    last_sequence: Option<u64>,
}

impl MemTable {
    fn apply(&mut self, sequence: u64, key: Vec<u8>, value: Option<Vec<u8>>) -> Result<()> {
        if self
            .last_sequence
            .is_some_and(|last_sequence| sequence <= last_sequence)
        {
            return Err(corruption(format!(
                "MemTable sequence {sequence} does not follow {:?}",
                self.last_sequence
            )));
        }

        let new_bytes = resident_bytes(&key, value.as_deref())?;
        let old_bytes = self
            .entries
            .get(key.as_slice())
            .map(|entry| resident_bytes(&key, entry.value.as_deref()))
            .transpose()?
            .unwrap_or(0);
        let without_old = self
            .approximate_bytes
            .checked_sub(old_bytes)
            .ok_or_else(|| {
                corruption("MemTable byte accounting underflowed while replacing an entry")
            })?;
        self.approximate_bytes = without_old.checked_add(new_bytes).ok_or_else(|| {
            corruption("MemTable byte accounting overflowed while applying an entry")
        })?;
        self.entries.insert(key, VersionedEntry { sequence, value });
        self.last_sequence = Some(sequence);
        Ok(())
    }

    fn get(&self, key: &[u8]) -> Option<&VersionedEntry> {
        self.entries.get(key)
    }

    fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

#[derive(Debug)]
pub(super) struct MemTableSet {
    mutable: MemTable,
    immutable: Vec<MemTable>,
    mutable_bytes_limit: usize,
}

impl MemTableSet {
    pub(super) fn new(mutable_bytes_limit: usize) -> Result<Self> {
        if mutable_bytes_limit == 0 {
            return Err(DbError::InvalidInput(
                "LSM mutable MemTable byte limit must be greater than zero".to_owned(),
            ));
        }
        Ok(Self {
            mutable: MemTable::default(),
            immutable: Vec::new(),
            mutable_bytes_limit,
        })
    }

    pub(super) fn apply(
        &mut self,
        sequence: u64,
        key: Vec<u8>,
        value: Option<Vec<u8>>,
    ) -> Result<()> {
        self.mutable.apply(sequence, key, value)?;
        if self.mutable.approximate_bytes >= self.mutable_bytes_limit {
            let frozen = std::mem::take(&mut self.mutable);
            if frozen.is_empty() {
                return Err(corruption("attempted to freeze an empty MemTable"));
            }
            self.immutable.push(frozen);
        }
        Ok(())
    }

    pub(super) fn get(&self, key: &[u8]) -> Option<&VersionedEntry> {
        self.mutable
            .get(key)
            .or_else(|| self.immutable.iter().rev().find_map(|table| table.get(key)))
    }

    pub(super) fn visible_state(&self) -> BTreeMap<Vec<u8>, VersionedEntry> {
        let mut visible = BTreeMap::new();
        for table in self.immutable.iter().chain(std::iter::once(&self.mutable)) {
            for (key, entry) in &table.entries {
                let replace = visible
                    .get(key)
                    .is_none_or(|current: &VersionedEntry| entry.sequence > current.sequence);
                if replace {
                    visible.insert(key.clone(), entry.clone());
                }
            }
        }
        visible
    }

    pub(super) fn oldest_immutable_snapshot(&self) -> Result<Option<FrozenSnapshot>> {
        let Some(table) = self.immutable.first() else {
            return Ok(None);
        };
        let durable_sequence = table
            .last_sequence
            .ok_or_else(|| corruption("frozen MemTable has no last sequence"))?;
        Ok(Some((table.entries.clone(), durable_sequence)))
    }

    pub(super) fn retire_oldest_immutable(&mut self) -> Result<()> {
        if self.immutable.is_empty() {
            return Err(corruption("attempted to retire a missing frozen MemTable"));
        }
        self.immutable.remove(0);
        Ok(())
    }

    pub(super) fn mutable_entries(&self) -> usize {
        self.mutable.entries.len()
    }

    pub(super) fn immutable_count(&self) -> usize {
        self.immutable.len()
    }

    pub(super) fn immutable_entries(&self) -> usize {
        self.immutable.iter().map(|table| table.entries.len()).sum()
    }
}

fn resident_bytes(key: &[u8], value: Option<&[u8]>) -> Result<usize> {
    ENTRY_OVERHEAD_BYTES
        .checked_add(key.len())
        .and_then(|bytes| bytes.checked_add(value.map_or(0, <[u8]>::len)))
        .ok_or_else(|| corruption("MemTable resident-byte calculation overflowed usize"))
}

fn corruption(reason: impl Into<String>) -> DbError {
    DbError::Corruption {
        offset: 0,
        reason: reason.into(),
    }
}
