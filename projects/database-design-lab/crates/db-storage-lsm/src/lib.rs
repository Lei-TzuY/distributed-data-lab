//! Executable LSM engine with a checksummed WAL, ordered MemTables, and immutable SSTables.
//!
//! Mutations are synchronized to the WAL before entering the mutable MemTable. When a table freezes,
//! it is synchronously written as an indexed/checksummed SSTable, a new immutable manifest snapshot is
//! synchronized, and only then is the new version set published through one slot of mirrored `CURRENT`.
//! WAL segments rotate only after published SSTables cover their complete sequence range. SSTable v2
//! embeds a checksummed Bloom filter for point-read rejection. Flushes enter overlapping L0; four
//! L0 tables trigger a synchronous full-set merge into at most one non-overlapping L1 run. Because the
//! merge covers every authoritative table and this engine has no snapshots, it elides tombstones and
//! records that proof point in Manifest v5 before obsolete sorted-table/manifest files are reclaimed.
//! Deterministic
//! compaction fault tests exercise pre-write, torn durable output, and post-sync reported errors at
//! the replacement L1 SSTable, Manifest, first CURRENT, and mirror CURRENT boundaries. A second
//! deterministic evidence layer compares multi-cycle compaction against the in-memory oracle and exposes
//! exact integer read/data-write/sorted-table-space amplification counters for hand-computable traces.

mod bloom;
mod manifest;
mod memtable;
mod sstable;
mod wal;

use std::collections::{BTreeMap, BTreeSet};
use std::ffi::OsStr;
use std::fs;
use std::io;
use std::ops::Bound;
use std::path::{Path, PathBuf};
use std::time::Instant;

use db_core::{
    validate_key, validate_key_value, validate_range_scan, AmplificationInstrumented,
    AmplificationReport, ConcurrencyMode, CrashRecovery, DbError, DistributionMode,
    EngineCapabilities, KvEngine, LogicalModel, OperationalTimingFailureSample,
    OperationalTimingInstrumented, OperationalTimingReport, OperationalTimingSample,
    OperationalWork, OperationalWorkUnit, Persistence, ReadWorkUnit, Result, StorageArchitecture,
    StructuralReadAmplification, MAX_KEY_BYTES, MAX_VALUE_BYTES,
};
use serde::Serialize;

pub use db_core::AmplificationRatio;
use manifest::{VersionSet, CURRENT_FILE_NAME};
use memtable::{MemTableSet, VersionedEntry};
use sstable::{file_name as sstable_file_name, SsTable};
use wal::{file_name as wal_file_name, MutationKind, Wal, INITIAL_FIRST_SEQUENCE, INITIAL_WAL_ID};
pub use wal::{RecoveredWalTail, WalVerification};

/// Deterministic threshold used to freeze the current mutable MemTable.
pub const MUTABLE_MEMTABLE_BYTES_LIMIT: usize = 64 * 1024;

const LEVEL0_COMPACTION_TRIGGER: usize = 4;

#[cfg(test)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CompactionWriteKind {
    L1Sstable,
    Manifest,
    FirstCurrent,
    MirrorCurrent,
}

#[cfg(test)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CompactionFaultMode {
    BeforeWrite,
    TornWrite,
    AfterSync,
}

#[cfg(test)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct CompactionFaultSpec {
    kind: CompactionWriteKind,
    mode: CompactionFaultMode,
}

/// Current WAL/MemTable/SSTable structure counts, not performance instrumentation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct LsmStats {
    /// Complete mutation records retained in the authoritative active WAL segment.
    pub wal_records: u64,
    /// Authoritative WAL segment id selected by the manifest.
    pub active_wal_id: u64,
    /// First sequence encoded by the authoritative active WAL segment.
    pub active_wal_first_sequence: u64,
    /// Latest entries (including tombstones) in the active mutable table.
    pub mutable_entries: usize,
    /// Frozen in-memory tables not yet published as SSTables.
    pub immutable_memtables: usize,
    /// Latest entries (including tombstones) across unflushed immutable tables.
    pub immutable_entries: usize,
    /// Immutable sorted tables referenced by the authoritative manifest.
    pub sstables: usize,
    /// Overlapping flush tables in level zero.
    pub level0_sstables: usize,
    /// Non-overlapping level-one runs. The current policy permits at most one.
    pub level1_sstables: usize,
    /// Total indexed entries (including tombstones) across authoritative SSTables.
    pub sstable_entries: u64,
    /// Highest WAL sequence represented by the authoritative SSTable version set.
    pub durable_sequence: u64,
    /// Highest full-set compaction sequence through which tombstones were safely elided.
    pub tombstone_gc_sequence: u64,
}

/// Process-local, resettable counters for reproducible LSM amplification experiments.
///
/// These counters deliberately describe the implemented data path rather than pretending to be
/// device-level I/O telemetry. SSTables are fully resident after open, so point/range read counters
/// measure sorted-table work (tables consulted and physical versions decoded). Write counters include
/// WAL mutation records plus immutable SSTable bytes produced by flush and compaction; manifest,
/// CURRENT, filesystem metadata, cache traffic, and device writeback are outside this accounting.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize)]
pub struct LsmInstrumentation {
    /// Successful explicit `GET` operations.
    pub point_reads: u64,
    /// SSTables consulted by successful explicit `GET` operations.
    pub point_sstable_consults: u64,
    /// Successful explicit range-scan operations, including empty-result scans.
    pub range_scans: u64,
    /// Logical records returned by successful range scans.
    pub range_result_records: u64,
    /// Physical SSTable records decoded while serving successful range scans.
    pub range_sstable_records_decoded: u64,
    /// Successful acknowledged PUT/DELETE mutations.
    pub logical_mutations: u64,
    /// Key plus PUT-value bytes accepted by successful mutations; DELETE contributes key bytes only.
    pub logical_mutation_bytes: u64,
    /// Complete encoded WAL mutation-record bytes written during this measurement window.
    pub wal_record_bytes_written: u64,
    /// Number of immutable MemTable flush SSTables created.
    pub flushes: u64,
    /// SSTable file bytes written by immutable MemTable flushes.
    pub flush_sstable_bytes_written: u64,
    /// Number of full-set compactions started after the L0 trigger fired.
    pub compactions: u64,
    /// Authoritative SSTable bytes consumed as full-set compaction input.
    pub compaction_input_sstable_bytes: u64,
    /// Replacement L1 SSTable bytes written by compaction; table-less GC contributes zero.
    pub compaction_output_sstable_bytes_written: u64,
}

/// Backward-compatible name for the common amplification report shape.
pub type LsmAmplificationReport = AmplificationReport;

/// Read-only verification result for the implemented LSM directory state.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct VerificationReport {
    /// Active WAL validation summary.
    pub wal: WalVerification,
    /// Reconstructed memory/disk structure counts.
    pub memtables: LsmStats,
}

/// Standalone LSM engine implementing the common binary KV contract.
pub struct LsmEngine {
    path: PathBuf,
    wal: Option<Wal>,
    memtables: MemTableSet,
    tables: Vec<SsTable>,
    version: VersionSet,
    next_table_id: u64,
    next_manifest_id: u64,
    next_wal_id: u64,
    instrumentation: LsmInstrumentation,
    operational_timing: OperationalTimingReport,
    operational_step_index: Option<u64>,
    poisoned: bool,
    #[cfg(test)]
    compaction_fault_spec: Option<CompactionFaultSpec>,
    #[cfg(test)]
    compaction_fault_trace: Vec<CompactionWriteKind>,
}

impl std::fmt::Debug for LsmEngine {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LsmEngine")
            .field("path", &self.path)
            .field("stats", &self.stats().ok())
            .field("manifest_id", &self.version.manifest_id)
            .field("current_generation", &self.version.current_generation)
            .field(
                "recovered_tail",
                &self.wal.as_ref().and_then(Wal::recovered_tail),
            )
            .field("poisoned", &self.poisoned)
            .finish_non_exhaustive()
    }
}

impl LsmEngine {
    /// Opens an existing engine directory or creates a new one when the path is absent.
    ///
    /// Existing directories are never initialized implicitly. Unknown directory entries fail closed;
    /// canonical orphan SSTables/manifests left before `CURRENT` publication are tolerated and ignored.
    /// A structurally valid incomplete final WAL record is truncated and synchronized.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        match fs::create_dir(&path) {
            Ok(()) => Self::initialize_new(path),
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => Self::open_existing(path),
            Err(error) => Err(error.into()),
        }
    }

    /// Creates a fresh engine directory and rejects any pre-existing path.
    pub fn create_new(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        fs::create_dir(&path)?;
        Self::initialize_new(path)
    }

    fn initialize_new(path: PathBuf) -> Result<Self> {
        let wal = Wal::create_new(
            &path.join(wal_file_name(INITIAL_WAL_ID)),
            INITIAL_WAL_ID,
            INITIAL_FIRST_SEQUENCE,
        )?;
        let version = manifest::create_initial(&path, INITIAL_WAL_ID, INITIAL_FIRST_SEQUENCE)?;
        Ok(Self {
            path,
            wal: Some(wal),
            memtables: MemTableSet::new(MUTABLE_MEMTABLE_BYTES_LIMIT)?,
            tables: Vec::new(),
            version,
            next_table_id: 1,
            next_manifest_id: 2,
            next_wal_id: 2,
            instrumentation: LsmInstrumentation::default(),
            operational_timing: OperationalTimingReport::default(),
            operational_step_index: None,
            poisoned: false,
            #[cfg(test)]
            compaction_fault_spec: None,
            #[cfg(test)]
            compaction_fault_trace: Vec::new(),
        })
    }

    fn open_existing(path: PathBuf) -> Result<Self> {
        let layout = validate_layout(&path)?;
        let mut version = if layout.has_version_set {
            manifest::load(&path)?
        } else {
            VersionSet {
                current_generation: 0,
                manifest_id: 0,
                durable_sequence: 0,
                tombstone_gc_sequence: 0,
                table_id_high_watermark: 0,
                wal_id: INITIAL_WAL_ID,
                wal_first_sequence: INITIAL_FIRST_SEQUENCE,
                tables: Vec::new(),
            }
        };
        version.table_id_high_watermark = version.table_id_high_watermark.max(layout.max_table_id);
        let mut tables = Vec::with_capacity(version.tables.len());
        for descriptor in &version.tables {
            tables.push(SsTable::open(
                &path.join(sstable_file_name(descriptor.table_id)),
                descriptor.clone(),
            )?);
        }

        if !layout.wal_ids.contains(&version.wal_id) {
            return Err(corruption(format!(
                "manifest references missing WAL segment {}",
                version.wal_id
            )));
        }
        let wal_path = path.join(wal_file_name(version.wal_id));
        let mut memtables = MemTableSet::new(MUTABLE_MEMTABLE_BYTES_LIMIT)?;
        let durable_sequence = version.durable_sequence;
        let wal = Wal::open(
            &wal_path,
            version.wal_id,
            version.wal_first_sequence,
            |mutation| {
                if mutation.sequence > durable_sequence {
                    memtables.apply(mutation.sequence, mutation.key, mutation.value)?;
                }
                Ok(())
            },
        )?;
        if durable_sequence >= wal.next_sequence() {
            return Err(corruption(format!(
                "manifest durable sequence {durable_sequence} is not below WAL next sequence {}",
                wal.next_sequence()
            )));
        }

        Ok(Self {
            path,
            wal: Some(wal),
            memtables,
            tables,
            next_table_id: checked_next_id(version.table_id_high_watermark, "SSTable")?,
            next_manifest_id: checked_next_id(layout.max_manifest_id, "manifest")?,
            next_wal_id: checked_next_id(layout.max_wal_id, "WAL")?,
            version,
            instrumentation: LsmInstrumentation::default(),
            operational_timing: OperationalTimingReport::default(),
            operational_step_index: None,
            poisoned: false,
            #[cfg(test)]
            compaction_fault_spec: None,
            #[cfg(test)]
            compaction_fault_trace: Vec::new(),
        })
    }

    /// Validates the authoritative CURRENT/manifest/SSTable set and WAL without modifying bytes.
    pub fn verify(path: impl AsRef<Path>) -> Result<VerificationReport> {
        let path = path.as_ref();
        let layout = validate_layout(path)?;
        let version = if layout.has_version_set {
            manifest::load(path)?
        } else {
            VersionSet {
                current_generation: 0,
                manifest_id: 0,
                durable_sequence: 0,
                tombstone_gc_sequence: 0,
                table_id_high_watermark: 0,
                wal_id: INITIAL_WAL_ID,
                wal_first_sequence: INITIAL_FIRST_SEQUENCE,
                tables: Vec::new(),
            }
        };
        let mut sstable_entries = 0_u64;
        for descriptor in &version.tables {
            SsTable::open(
                &path.join(sstable_file_name(descriptor.table_id)),
                descriptor.clone(),
            )?;
            sstable_entries = sstable_entries
                .checked_add(descriptor.entry_count)
                .ok_or_else(|| corruption("SSTable verification entry count overflowed u64"))?;
        }

        if !layout.wal_ids.contains(&version.wal_id) {
            return Err(corruption(format!(
                "manifest references missing WAL segment {}",
                version.wal_id
            )));
        }
        let wal_path = path.join(wal_file_name(version.wal_id));
        let mut memtables = MemTableSet::new(MUTABLE_MEMTABLE_BYTES_LIMIT)?;
        let durable_sequence = version.durable_sequence;
        let wal = Wal::verify(
            &wal_path,
            version.wal_id,
            version.wal_first_sequence,
            |mutation| {
                if mutation.sequence > durable_sequence {
                    memtables.apply(mutation.sequence, mutation.key, mutation.value)?;
                }
                Ok(())
            },
        )?;
        if durable_sequence >= wal.next_sequence {
            return Err(corruption(format!(
                "manifest durable sequence {durable_sequence} is not below WAL next sequence {}",
                wal.next_sequence
            )));
        }
        Ok(VerificationReport {
            memtables: LsmStats {
                wal_records: wal.record_count,
                active_wal_id: wal.wal_id,
                active_wal_first_sequence: wal.first_sequence,
                mutable_entries: memtables.mutable_entries(),
                immutable_memtables: memtables.immutable_count(),
                immutable_entries: memtables.immutable_entries(),
                sstables: version.tables.len(),
                level0_sstables: version
                    .tables
                    .iter()
                    .filter(|table| table.level == 0)
                    .count(),
                level1_sstables: version
                    .tables
                    .iter()
                    .filter(|table| table.level == 1)
                    .count(),
                sstable_entries,
                durable_sequence,
                tombstone_gc_sequence: version.tombstone_gc_sequence,
            },
            wal,
        })
    }

    /// Engine directory path.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Structurally valid incomplete WAL tail repaired by the most recent open, if any.
    #[must_use]
    pub fn recovered_tail(&self) -> Option<&RecoveredWalTail> {
        self.wal.as_ref().and_then(Wal::recovered_tail)
    }

    /// Returns structural counts without presenting them as amplification metrics.
    pub fn stats(&self) -> Result<LsmStats> {
        self.ensure_usable()?;
        let wal = self.wal.as_ref().ok_or(DbError::Poisoned)?;
        let sstable_entries = self.version.tables.iter().try_fold(0_u64, |total, table| {
            total
                .checked_add(table.entry_count)
                .ok_or_else(|| corruption("SSTable entry count overflowed u64"))
        })?;
        Ok(LsmStats {
            wal_records: wal.record_count(),
            active_wal_id: wal.wal_id(),
            active_wal_first_sequence: wal.first_sequence(),
            mutable_entries: self.memtables.mutable_entries(),
            immutable_memtables: self.memtables.immutable_count(),
            immutable_entries: self.memtables.immutable_entries(),
            sstables: self.tables.len(),
            level0_sstables: self
                .version
                .tables
                .iter()
                .filter(|table| table.level == 0)
                .count(),
            level1_sstables: self
                .version
                .tables
                .iter()
                .filter(|table| table.level == 1)
                .count(),
            sstable_entries,
            durable_sequence: self.version.durable_sequence,
            tombstone_gc_sequence: self.version.tombstone_gc_sequence,
        })
    }

    /// Returns a copy of the process-local instrumentation counters.
    #[must_use]
    pub const fn instrumentation(&self) -> LsmInstrumentation {
        self.instrumentation
    }

    /// Resets process-local amplification counters without modifying database state.
    pub fn reset_instrumentation(&mut self) {
        self.instrumentation = LsmInstrumentation::default();
    }

    /// Builds exact raw amplification ratios from the current measurement window and durable SSTables.
    ///
    /// Computing the sorted-table space denominator decodes the authoritative SSTable set in memory but
    /// does not increment read counters. The denominator excludes unflushed WAL/MemTable state so it is
    /// paired with exactly the durable sorted-table bytes in the numerator.
    pub fn amplification_report(&self) -> Result<LsmAmplificationReport> {
        self.ensure_usable()?;
        let authoritative_sstable_bytes = self
            .version
            .tables
            .iter()
            .fold(0_u64, |total, table| total.saturating_add(table.file_bytes));
        let mut durable = BTreeMap::new();
        for table in &self.tables {
            let _ = table.overlay_range(b"", None, &mut durable)?;
        }
        let durable_live_bytes = durable.into_iter().try_fold(0_u64, |total, (key, entry)| {
            let Some(value) = entry.value else {
                return Ok(total);
            };
            let bytes = key
                .len()
                .checked_add(value.len())
                .ok_or_else(|| corruption("durable live logical byte count overflowed usize"))?;
            let bytes = u64::try_from(bytes)
                .map_err(|_| corruption("durable live logical byte count does not fit u64"))?;
            total
                .checked_add(bytes)
                .ok_or_else(|| corruption("durable live logical byte count overflowed u64"))
        })?;
        let data_write_bytes = self
            .instrumentation
            .wal_record_bytes_written
            .saturating_add(self.instrumentation.flush_sstable_bytes_written)
            .saturating_add(self.instrumentation.compaction_output_sstable_bytes_written);
        Ok(LsmAmplificationReport {
            point_read: StructuralReadAmplification {
                ratio: AmplificationRatio {
                    numerator: self.instrumentation.point_sstable_consults,
                    denominator: self.instrumentation.point_reads,
                },
                unit: ReadWorkUnit::LsmSstableConsult,
            },
            range_read: StructuralReadAmplification {
                ratio: AmplificationRatio {
                    numerator: self.instrumentation.range_sstable_records_decoded,
                    denominator: self.instrumentation.range_result_records,
                },
                unit: ReadWorkUnit::LsmSstableVersionDecoded,
            },
            data_write_bytes_per_logical_byte: AmplificationRatio {
                numerator: data_write_bytes,
                denominator: self.instrumentation.logical_mutation_bytes,
            },
            primary_structure_bytes_per_live_byte: AmplificationRatio {
                numerator: authoritative_sstable_bytes,
                denominator: durable_live_bytes,
            },
        })
    }

    fn current_entry_with_consults(&self, key: &[u8]) -> Result<(Option<VersionedEntry>, u64)> {
        if let Some(entry) = self.memtables.get(key) {
            return Ok((Some(entry.clone()), 0));
        }
        let mut consulted = 0_u64;
        for table in self.tables.iter().rev() {
            consulted = consulted.saturating_add(1);
            if let Some(entry) = table.get(key)? {
                return Ok((Some(entry), consulted));
            }
        }
        Ok((None, consulted))
    }

    fn current_entry(&self, key: &[u8]) -> Result<Option<VersionedEntry>> {
        Ok(self.current_entry_with_consults(key)?.0)
    }

    fn current_value(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        Ok(self.current_entry(key)?.and_then(|entry| entry.value))
    }

    fn persist_and_apply(
        &mut self,
        kind: MutationKind,
        key: &[u8],
        value: Option<&[u8]>,
    ) -> Result<()> {
        self.ensure_usable()?;
        let append = self.wal.as_mut().ok_or(DbError::Poisoned)?.append(
            kind,
            key,
            value.unwrap_or_default(),
        );
        let sequence = match append {
            Ok(sequence) => sequence,
            Err(error) => {
                self.poisoned = true;
                return Err(error);
            }
        };
        let encoded_record_bytes = wal::RECORD_HEADER_LEN
            .saturating_add(key.len())
            .saturating_add(value.map_or(0, <[u8]>::len));
        self.instrumentation.wal_record_bytes_written = self
            .instrumentation
            .wal_record_bytes_written
            .saturating_add(u64::try_from(encoded_record_bytes).unwrap_or(u64::MAX));
        if let Err(error) = self
            .memtables
            .apply(sequence, key.to_vec(), value.map(<[u8]>::to_vec))
        {
            self.poisoned = true;
            return Err(error);
        }
        if let Err(error) = self.flush_frozen_memtables() {
            self.poisoned = true;
            return Err(error);
        }
        self.instrumentation.logical_mutations =
            self.instrumentation.logical_mutations.saturating_add(1);
        let logical_bytes = key.len().saturating_add(value.map_or(0, <[u8]>::len));
        self.instrumentation.logical_mutation_bytes = self
            .instrumentation
            .logical_mutation_bytes
            .saturating_add(u64::try_from(logical_bytes).unwrap_or(u64::MAX));
        Ok(())
    }

    fn flush_frozen_memtables(&mut self) -> Result<()> {
        while let Some((entries, durable_sequence)) = self.memtables.oldest_immutable_snapshot()? {
            if self.version.manifest_id == 0 {
                let wal = self.wal.as_ref().ok_or(DbError::Poisoned)?;
                self.version =
                    manifest::create_initial(&self.path, wal.wal_id(), wal.first_sequence())?;
                self.next_manifest_id = 2;
            }
            let table_id = self.next_table_id;
            let manifest_id = self.next_manifest_id;
            let next_table_id = table_id
                .checked_add(1)
                .ok_or_else(|| corruption("SSTable id exhausted"))?;
            let next_manifest_id = manifest_id
                .checked_add(1)
                .ok_or_else(|| corruption("manifest id exhausted"))?;

            let table = SsTable::create_new(&self.path, table_id, durable_sequence, &entries)?;
            self.instrumentation.flushes = self.instrumentation.flushes.saturating_add(1);
            self.instrumentation.flush_sstable_bytes_written = self
                .instrumentation
                .flush_sstable_bytes_written
                .saturating_add(table.descriptor().file_bytes);
            let mut descriptors = self.version.tables.clone();
            descriptors.push(table.descriptor().clone());
            let next_version = manifest::install(
                &self.path,
                &self.version,
                manifest_id,
                manifest::ManifestState {
                    durable_sequence,
                    tombstone_gc_sequence: self.version.tombstone_gc_sequence,
                    wal_id: self.version.wal_id,
                    wal_first_sequence: self.version.wal_first_sequence,
                    tables: descriptors,
                },
            )?;

            self.tables.push(table);
            self.version = next_version;
            self.next_table_id = next_table_id;
            self.next_manifest_id = next_manifest_id;
            self.memtables.retire_oldest_immutable()?;
        }
        self.maybe_compact_level0()?;
        self.maybe_rotate_wal()?;
        Ok(())
    }

    fn maybe_compact_level0(&mut self) -> Result<()> {
        let level0_count = self
            .version
            .tables
            .iter()
            .filter(|table| table.level == 0)
            .count();
        if level0_count < LEVEL0_COMPACTION_TRIGGER {
            return Ok(());
        }

        let compaction_started = Instant::now();
        self.instrumentation.compactions = self.instrumentation.compactions.saturating_add(1);
        let input_bytes = self
            .version
            .tables
            .iter()
            .fold(0_u64, |total, table| total.saturating_add(table.file_bytes));
        let input_records = self.version.tables.iter().fold(0_u64, |total, table| {
            total.saturating_add(table.entry_count)
        });
        self.instrumentation.compaction_input_sstable_bytes = self
            .instrumentation
            .compaction_input_sstable_bytes
            .saturating_add(input_bytes);
        let work = OperationalWork {
            unit: OperationalWorkUnit::LsmSstableRecordVersion,
            units_examined: input_records,
            bytes_examined: input_bytes,
        };
        let mut input_fully_examined = false;
        let result = self.compact_level0_after_trigger(&mut input_fully_examined);
        let duration_ns =
            u64::try_from(compaction_started.elapsed().as_nanos()).unwrap_or(u64::MAX);
        match result {
            Ok(()) => {
                self.operational_timing
                    .compaction_stall_ns
                    .push(duration_ns);
                self.operational_timing
                    .compaction_stall_samples
                    .push(OperationalTimingSample {
                        measured_step_index: self.operational_step_index,
                        duration_ns,
                        work,
                    });
                Ok(())
            }
            Err(error) => {
                self.operational_timing
                    .compaction_stall_failure_samples
                    .push(OperationalTimingFailureSample {
                        measured_step_index: self.operational_step_index,
                        duration_ns,
                        work: input_fully_examined.then_some(work),
                        error_class: error.class(),
                    });
                Err(error)
            }
        }
    }

    fn compact_level0_after_trigger(&mut self, input_fully_examined: &mut bool) -> Result<()> {
        let mut merged = BTreeMap::new();
        for table in &self.tables {
            let _ = table.overlay_range(b"", None, &mut merged)?;
        }
        *input_fully_examined = true;
        if merged.is_empty() {
            return Err(corruption(
                "full-set compaction unexpectedly produced no entries",
            ));
        }

        let manifest_id = self.next_manifest_id;
        let next_manifest_id = checked_next_id(manifest_id, "manifest")?;
        let durable_sequence = self.version.durable_sequence;
        if merged
            .values()
            .any(|entry| entry.sequence > durable_sequence)
        {
            return Err(corruption(
                "full-set compaction input exceeds the manifest durable sequence",
            ));
        }
        merged.retain(|_, entry| entry.value.is_some());

        let (table, descriptors, next_table_id) = if merged.is_empty() {
            (None, Vec::new(), self.next_table_id)
        } else {
            let table_id = self.next_table_id;
            let next_table_id = checked_next_id(table_id, "SSTable")?;
            #[cfg(test)]
            self.compaction_before_write_for_test(CompactionWriteKind::L1Sstable)?;
            let table =
                SsTable::create_new_at_level(&self.path, table_id, 1, durable_sequence, &merged)?;
            #[cfg(test)]
            self.compaction_after_file_write_for_test(
                CompactionWriteKind::L1Sstable,
                &self.path.join(sstable_file_name(table_id)),
            )?;
            let descriptor = table.descriptor().clone();
            self.instrumentation.compaction_output_sstable_bytes_written = self
                .instrumentation
                .compaction_output_sstable_bytes_written
                .saturating_add(descriptor.file_bytes);
            (Some(table), vec![descriptor], next_table_id)
        };

        #[cfg(test)]
        self.compaction_before_write_for_test(CompactionWriteKind::Manifest)?;
        let compacted = manifest::prepare_install(
            &self.path,
            &self.version,
            manifest_id,
            manifest::ManifestState {
                durable_sequence,
                tombstone_gc_sequence: durable_sequence,
                wal_id: self.version.wal_id,
                wal_first_sequence: self.version.wal_first_sequence,
                tables: descriptors,
            },
        )?;
        #[cfg(test)]
        self.compaction_after_file_write_for_test(
            CompactionWriteKind::Manifest,
            &self.path.join(manifest::manifest_file_name(manifest_id)),
        )?;

        #[cfg(test)]
        self.compaction_before_write_for_test(CompactionWriteKind::FirstCurrent)?;
        manifest::publish_prepared(&self.path, &compacted)?;
        #[cfg(test)]
        self.compaction_after_current_write_for_test(
            CompactionWriteKind::FirstCurrent,
            compacted.current_generation,
        )?;

        #[cfg(test)]
        self.compaction_before_write_for_test(CompactionWriteKind::MirrorCurrent)?;
        let mirrored = manifest::mirror_current(&self.path, &compacted)?;
        #[cfg(test)]
        self.compaction_after_current_write_for_test(
            CompactionWriteKind::MirrorCurrent,
            mirrored.current_generation,
        )?;
        let active_table_id = table.as_ref().map(|table| table.descriptor().table_id);
        let active_manifest_id = mirrored.manifest_id;

        let new_tables = table.into_iter().collect();
        let old_tables = std::mem::replace(&mut self.tables, new_tables);
        self.version = mirrored;
        self.next_table_id = next_table_id;
        self.next_manifest_id = next_manifest_id;
        drop(old_tables);
        self.reclaim_obsolete_sstables(active_table_id);
        self.reclaim_obsolete_manifests(active_manifest_id);
        Ok(())
    }

    #[cfg(test)]
    fn begin_compaction_fault_trace_for_test(&mut self) {
        self.compaction_fault_spec = None;
        self.compaction_fault_trace.clear();
    }

    #[cfg(test)]
    fn inject_compaction_fault_for_test(
        &mut self,
        kind: CompactionWriteKind,
        mode: CompactionFaultMode,
    ) {
        self.compaction_fault_spec = Some(CompactionFaultSpec { kind, mode });
        self.compaction_fault_trace.clear();
    }

    #[cfg(test)]
    fn compaction_fault_trace_for_test(&self) -> &[CompactionWriteKind] {
        &self.compaction_fault_trace
    }

    #[cfg(test)]
    fn compaction_before_write_for_test(&mut self, kind: CompactionWriteKind) -> Result<()> {
        self.compaction_fault_trace.push(kind);
        if self.compaction_fault_spec
            == Some(CompactionFaultSpec {
                kind,
                mode: CompactionFaultMode::BeforeWrite,
            })
        {
            return Err(injected_compaction_fault(
                kind,
                CompactionFaultMode::BeforeWrite,
            ));
        }
        Ok(())
    }

    #[cfg(test)]
    fn compaction_after_file_write_for_test(
        &self,
        kind: CompactionWriteKind,
        path: &Path,
    ) -> Result<()> {
        let Some(spec) = self.compaction_fault_spec.filter(|spec| spec.kind == kind) else {
            return Ok(());
        };
        match spec.mode {
            CompactionFaultMode::BeforeWrite => Ok(()),
            CompactionFaultMode::TornWrite => {
                let bytes = fs::metadata(path)?.len();
                let torn_len = (bytes / 2).max(1);
                let file = fs::OpenOptions::new().write(true).open(path)?;
                file.set_len(torn_len)?;
                file.sync_all()?;
                Err(injected_compaction_fault(kind, spec.mode))
            }
            CompactionFaultMode::AfterSync => Err(injected_compaction_fault(kind, spec.mode)),
        }
    }

    #[cfg(test)]
    fn compaction_after_current_write_for_test(
        &self,
        kind: CompactionWriteKind,
        generation: u64,
    ) -> Result<()> {
        use std::io::{Seek, SeekFrom, Write};

        let Some(spec) = self.compaction_fault_spec.filter(|spec| spec.kind == kind) else {
            return Ok(());
        };
        match spec.mode {
            CompactionFaultMode::BeforeWrite => Ok(()),
            CompactionFaultMode::TornWrite => {
                let slot_id = usize::try_from(generation % 2).expect("modulo two fits usize");
                let offset = u64::try_from(slot_id * manifest::CURRENT_SLOT_BYTES)
                    .expect("CURRENT slot offset fits u64");
                let mut file = fs::OpenOptions::new()
                    .read(true)
                    .write(true)
                    .open(self.path.join(CURRENT_FILE_NAME))?;
                file.seek(SeekFrom::Start(offset))?;
                file.write_all(&vec![0xa5; manifest::CURRENT_SLOT_BYTES / 2])?;
                file.sync_data()?;
                Err(injected_compaction_fault(kind, spec.mode))
            }
            CompactionFaultMode::AfterSync => Err(injected_compaction_fault(kind, spec.mode)),
        }
    }

    fn reclaim_obsolete_sstables(&self, active_table_id: Option<u64>) {
        let Ok(entries) = fs::read_dir(&self.path) else {
            return;
        };
        for entry in entries.flatten() {
            let name = entry.file_name();
            let text = name.to_string_lossy();
            let Some(table_id) = parse_numbered_name(&text, "sst-", ".sst") else {
                continue;
            };
            if Some(table_id) != active_table_id {
                let _ = fs::remove_file(entry.path());
            }
        }
    }

    fn reclaim_obsolete_manifests(&self, active_manifest_id: u64) {
        let Ok(entries) = fs::read_dir(&self.path) else {
            return;
        };
        for entry in entries.flatten() {
            let name = entry.file_name();
            let text = name.to_string_lossy();
            let Some(manifest_id) = parse_numbered_name(&text, "MANIFEST-", "") else {
                continue;
            };
            if manifest_id != active_manifest_id {
                let _ = fs::remove_file(entry.path());
            }
        }
    }

    fn reopen_work(&self) -> Result<OperationalWork> {
        let wal = self.wal.as_ref().ok_or(DbError::Poisoned)?;
        let sstable_records = self.version.tables.iter().fold(0_u64, |total, table| {
            total.saturating_add(table.entry_count)
        });
        let sstable_bytes = self
            .version
            .tables
            .iter()
            .fold(0_u64, |total, table| total.saturating_add(table.file_bytes));
        Ok(OperationalWork {
            unit: OperationalWorkUnit::LsmRecordVersion,
            units_examined: wal.record_count().saturating_add(sstable_records),
            bytes_examined: wal.open_examined_bytes().saturating_add(sstable_bytes),
        })
    }

    fn maybe_rotate_wal(&mut self) -> Result<()> {
        let Some(first_sequence) = self.version.durable_sequence.checked_add(1) else {
            return Ok(());
        };
        let wal = self.wal.as_ref().ok_or(DbError::Poisoned)?;
        if wal.record_count() == 0
            || wal.next_sequence() != first_sequence
            || wal.first_sequence() == first_sequence
        {
            return Ok(());
        }

        let old_wal_id = wal.wal_id();
        let new_wal_id = self.next_wal_id;
        let new_manifest_id = self.next_manifest_id;
        let following_wal_id = checked_next_id(new_wal_id, "WAL")?;
        let following_manifest_id = checked_next_id(new_manifest_id, "manifest")?;
        let new_wal = Wal::create_new(
            &self.path.join(wal_file_name(new_wal_id)),
            new_wal_id,
            first_sequence,
        )?;
        let rotated = manifest::install(
            &self.path,
            &self.version,
            new_manifest_id,
            manifest::ManifestState {
                durable_sequence: self.version.durable_sequence,
                tombstone_gc_sequence: self.version.tombstone_gc_sequence,
                wal_id: new_wal_id,
                wal_first_sequence: first_sequence,
                tables: self.version.tables.clone(),
            },
        )?;
        let mirrored = manifest::mirror_current(&self.path, &rotated)?;

        let old_wal = self.wal.replace(new_wal).ok_or(DbError::Poisoned)?;
        self.version = mirrored;
        self.next_wal_id = following_wal_id;
        self.next_manifest_id = following_manifest_id;
        drop(old_wal);
        self.reclaim_obsolete_wals(new_wal_id);
        self.reclaim_obsolete_manifests(self.version.manifest_id);
        debug_assert_ne!(old_wal_id, new_wal_id);
        Ok(())
    }

    fn reclaim_obsolete_wals(&self, active_wal_id: u64) {
        let Ok(entries) = fs::read_dir(&self.path) else {
            return;
        };
        for entry in entries.flatten() {
            let name = entry.file_name();
            let text = name.to_string_lossy();
            let Some(wal_id) = parse_numbered_name(&text, "wal-", ".log") else {
                continue;
            };
            if wal_id != active_wal_id {
                let _ = fs::remove_file(entry.path());
            }
        }
    }

    fn ensure_usable(&self) -> Result<()> {
        if self.poisoned || self.wal.is_none() {
            Err(DbError::Poisoned)
        } else {
            Ok(())
        }
    }
}

impl KvEngine for LsmEngine {
    fn capabilities(&self) -> EngineCapabilities {
        EngineCapabilities {
            name: "lsm-level1-tombstone-gc-v5",
            logical_model: LogicalModel::KeyValue,
            storage_architecture: StorageArchitecture::LsmTree,
            concurrency: ConcurrencyMode::CallerSerialized,
            persistence: Persistence::Persistent,
            crash_recovery: CrashRecovery::WriteAheadLogReplay,
            distribution: DistributionMode::Standalone,
            ordered_range_scan: true,
            max_key_bytes: MAX_KEY_BYTES,
            max_value_bytes: MAX_VALUE_BYTES,
        }
    }

    fn put(&mut self, key: &[u8], value: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key_value(key, value)?;
        self.ensure_usable()?;
        let previous = self.current_value(key)?;
        self.persist_and_apply(MutationKind::Put, key, Some(value))?;
        Ok(previous)
    }

    fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        self.ensure_usable()?;
        let (entry, consulted) = self.current_entry_with_consults(key)?;
        self.instrumentation.point_reads = self.instrumentation.point_reads.saturating_add(1);
        self.instrumentation.point_sstable_consults = self
            .instrumentation
            .point_sstable_consults
            .saturating_add(consulted);
        Ok(entry.and_then(|entry| entry.value))
    }

    fn delete(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        self.ensure_usable()?;
        let previous = self.current_value(key)?;
        self.persist_and_apply(MutationKind::Delete, key, None)?;
        Ok(previous)
    }

    fn range_scan(
        &mut self,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        validate_range_scan(start, end)?;
        self.ensure_usable()?;
        self.instrumentation.range_scans = self.instrumentation.range_scans.saturating_add(1);
        if limit == 0 || end.is_some_and(|end| end == start) {
            return Ok(Vec::new());
        }

        let mut visible = BTreeMap::new();
        let mut decoded_records = 0_u64;
        for table in &self.tables {
            decoded_records =
                decoded_records.saturating_add(table.overlay_range(start, end, &mut visible)?);
        }
        let lower = Bound::Included(start.to_vec());
        let upper = end
            .map(|end| Bound::Excluded(end.to_vec()))
            .unwrap_or(Bound::Unbounded);
        for (key, entry) in self.memtables.visible_state().range((lower, upper)) {
            let replace = visible
                .get(key.as_slice())
                .is_none_or(|current: &VersionedEntry| entry.sequence > current.sequence);
            if replace {
                visible.insert(key.clone(), entry.clone());
            }
        }
        let result: Vec<_> = visible
            .into_iter()
            .filter_map(|(key, entry)| entry.value.map(|value| (key, value)))
            .take(limit)
            .collect();
        self.instrumentation.range_sstable_records_decoded = self
            .instrumentation
            .range_sstable_records_decoded
            .saturating_add(decoded_records);
        self.instrumentation.range_result_records = self
            .instrumentation
            .range_result_records
            .saturating_add(u64::try_from(result.len()).unwrap_or(u64::MAX));
        Ok(result)
    }

    fn reopen(&mut self) -> Result<()> {
        let instrumentation = self.instrumentation;
        let mut operational_timing = self.operational_timing.clone();
        let operational_step_index = self.operational_step_index;
        let started = Instant::now();
        self.wal.take();
        match Self::open_existing(self.path.clone()) {
            Ok(mut reopened) => {
                let work = reopened.reopen_work()?;
                let duration_ns = u64::try_from(started.elapsed().as_nanos()).unwrap_or(u64::MAX);
                operational_timing.reopen_ns.push(duration_ns);
                operational_timing
                    .reopen_samples
                    .push(OperationalTimingSample {
                        measured_step_index: operational_step_index,
                        duration_ns,
                        work,
                    });
                reopened.instrumentation = instrumentation;
                reopened.operational_timing = operational_timing;
                reopened.operational_step_index = operational_step_index;
                *self = reopened;
                Ok(())
            }
            Err(error) => {
                self.poisoned = true;
                Err(error)
            }
        }
    }
}

impl OperationalTimingInstrumented for LsmEngine {
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

impl AmplificationInstrumented for LsmEngine {
    fn reset_amplification(&mut self) {
        self.reset_instrumentation();
    }

    fn amplification_report(&mut self) -> Result<AmplificationReport> {
        LsmEngine::amplification_report(self)
    }
}

struct Layout {
    wal_ids: BTreeSet<u64>,
    max_wal_id: u64,
    max_table_id: u64,
    max_manifest_id: u64,
    has_version_set: bool,
}

fn validate_layout(path: &Path) -> Result<Layout> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.is_dir() {
        return Err(corruption("LSM engine path is not a direct directory"));
    }
    let current_name = OsStr::new(CURRENT_FILE_NAME);
    let mut wal_ids = BTreeSet::new();
    let mut found_current = false;
    let mut max_wal_id = 0_u64;
    let mut max_table_id = 0_u64;
    let mut max_manifest_id = 0_u64;

    for entry in fs::read_dir(path)? {
        let entry = entry?;
        let file_type = entry.file_type()?;
        if !file_type.is_file() {
            return Err(corruption(format!(
                "LSM directory entry is not a regular file: {}",
                entry.file_name().to_string_lossy()
            )));
        }
        let name = entry.file_name();
        if name == current_name {
            if found_current {
                return Err(corruption(
                    "LSM directory contains duplicate CURRENT entries",
                ));
            }
            found_current = true;
            continue;
        }
        let text = name.to_string_lossy();
        if let Some(id) = parse_numbered_name(&text, "wal-", ".log") {
            wal_ids.insert(id);
            max_wal_id = max_wal_id.max(id);
            continue;
        }
        if let Some(id) = parse_numbered_name(&text, "MANIFEST-", "") {
            max_manifest_id = max_manifest_id.max(id);
            continue;
        }
        if let Some(id) = parse_numbered_name(&text, "sst-", ".sst") {
            max_table_id = max_table_id.max(id);
            continue;
        }
        return Err(corruption(format!("unknown file in LSM directory: {text}")));
    }

    if wal_ids.is_empty() {
        return Err(corruption(
            "LSM directory contains no canonical WAL segment",
        ));
    }
    let has_version_set = match (found_current, max_manifest_id != 0) {
        (true, true) => true,
        (false, false)
            if max_table_id == 0 && wal_ids.len() == 1 && wal_ids.contains(&INITIAL_WAL_ID) =>
        {
            false
        }
        (false, false) => {
            return Err(corruption(
                "legacy WAL-only layout must contain exactly wal-0000000000000001.log",
            ));
        }
        (false, true) => {
            return Err(corruption(
                "LSM directory has manifest snapshots but is missing CURRENT",
            ));
        }
        (true, false) => {
            return Err(corruption(
                "LSM directory has CURRENT but no manifest snapshot",
            ));
        }
    };

    Ok(Layout {
        wal_ids,
        max_wal_id,
        max_table_id,
        max_manifest_id,
        has_version_set,
    })
}

fn parse_numbered_name(name: &str, prefix: &str, suffix: &str) -> Option<u64> {
    let digits = name.strip_prefix(prefix)?.strip_suffix(suffix)?;
    if digits.len() != 16 || !digits.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    let value = digits.parse::<u64>().ok()?;
    (value != 0).then_some(value)
}

fn checked_next_id(max_id: u64, label: &str) -> Result<u64> {
    max_id
        .checked_add(1)
        .ok_or_else(|| corruption(format!("{label} id space exhausted")))
}

fn corruption(reason: impl Into<String>) -> DbError {
    DbError::Corruption {
        offset: 0,
        reason: reason.into(),
    }
}

#[cfg(test)]
fn injected_compaction_fault(kind: CompactionWriteKind, mode: CompactionFaultMode) -> DbError {
    io::Error::other(format!(
        "injected LSM compaction durable-write fault at {kind:?} with mode {mode:?}"
    ))
    .into()
}

#[cfg(test)]
mod compaction_fault_tests;
#[cfg(test)]
mod compaction_tests;
#[cfg(test)]
mod instrumentation_tests;
#[cfg(test)]
mod sstable_tests;
#[cfg(test)]
mod tests;
#[cfg(test)]
mod wal_rotation_tests;
