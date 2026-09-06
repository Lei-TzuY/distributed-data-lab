use std::collections::{BTreeSet, VecDeque};
use std::path::Path;

mod common;
mod delete;
#[cfg(test)]
mod fault;
#[cfg(test)]
mod instrumentation_tests;
mod overflow;
mod reuse;
mod scan;

use db_core::{
    AmplificationRatio, AmplificationReport, OperationalTimingReport, ReadWorkUnit,
    StructuralReadAmplification,
};

use super::{
    corruption, BtreeError, Page, PageKind, Pager, Result, CHECKSUM_OFFSET, DATA_HEADER_LEN,
    PAGE_SIZE, SLOT_LEN, SUPERBLOCK_COUNT,
};

/// Maximum key size accepted by the tree; matches the common KV key contract.
pub const MAX_TREE_KEY_BYTES: usize = 4 * 1024;
/// Maximum value size accepted by the tree; matches the common KV value contract.
pub const MAX_TREE_VALUE_BYTES: usize = 1024 * 1024;

const LEAF_CELL_HEADER_LEN: usize = 6;
const KEY_OVERFLOW_FLAG: u16 = 1 << 15;
const KEY_LENGTH_MASK: u16 = KEY_OVERFLOW_FLAG - 1;
const KEY_OVERFLOW_REF_LEN: usize = 8;
const VALUE_OVERFLOW_FLAG: u32 = 1 << 31;
const VALUE_LENGTH_MASK: u32 = VALUE_OVERFLOW_FLAG - 1;
const OVERFLOW_VALUE_REF_LEN: usize = 8;
const INTERNAL_CELL_HEADER_LEN: usize = 10;
const MAX_TREE_HEIGHT: usize = 64;
const PAGE_BODY_CAPACITY: usize = CHECKSUM_OFFSET - DATA_HEADER_LEN;
const MAX_INLINE_KEY_BYTES: usize =
    PAGE_BODY_CAPACITY - SLOT_LEN - LEAF_CELL_HEADER_LEN - OVERFLOW_VALUE_REF_LEN;

/// Process-local, resettable counters for reproducible B+ tree amplification experiments.
///
/// Read counters measure logical validated-page accesses, including cache hits. Data-write bytes count
/// synchronized leaf/internal/overflow page images and exclude mirrored superblock metadata. The
/// counters describe the implemented data path; they are not device I/O telemetry.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct BtreeInstrumentation {
    /// Successful explicit `GET` operations.
    pub point_reads: u64,
    /// Validated B+ tree/overflow page accesses serving explicit GETs.
    pub point_page_accesses: u64,
    /// Successful explicit range scans, including empty-result scans.
    pub range_scans: u64,
    /// Logical key/value records returned by successful range scans.
    pub range_result_records: u64,
    /// Validated B+ tree/overflow page accesses serving explicit range scans.
    pub range_page_accesses: u64,
    /// Successful acknowledged PUT/DELETE calls, including a missing-key DELETE.
    pub logical_mutations: u64,
    /// Key plus PUT-value bytes accepted by mutations; DELETE contributes key bytes only.
    pub logical_mutation_bytes: u64,
    /// Synchronized leaf/internal/overflow page bytes produced by successful mutations.
    pub data_page_bytes_written: u64,
}

/// Persistent copy-on-write B+ tree supporting binary point lookup, insertion/update, and deletion.
///
/// Mutations never overwrite a reachable data page. `put` and `delete` append replacement leaves and
/// ancestors through [`Pager`] before publishing exactly one new root pointer through the mirrored
/// superblock. Deletion byte-balances or merges an underfull child with an adjacent sibling and
/// contracts one-child roots. Before each mutation, committed pages outside current-root reachability
/// become a reusable pool; overwriting such an orphan cannot damage the authoritative old tree. A
/// crash before root publication therefore leaves the previous tree authoritative.
#[derive(Debug)]
pub struct BPlusTree {
    pager: Pager,
    reusable_pages: VecDeque<u64>,
    cache_capacity: usize,
    instrumentation: BtreeInstrumentation,
    operational_timing: OperationalTimingReport,
    operational_step_index: Option<u64>,
}

impl BPlusTree {
    /// Creates a new empty tree without overwriting an existing file.
    pub fn create_new(path: impl AsRef<Path>, cache_capacity: usize) -> Result<Self> {
        Ok(Self {
            pager: Pager::create_new(path, cache_capacity)?,
            reusable_pages: VecDeque::new(),
            cache_capacity,
            instrumentation: BtreeInstrumentation::default(),
            operational_timing: OperationalTimingReport::default(),
            operational_step_index: None,
        })
    }

    /// Opens an existing tree and validates every page reachable from the committed root.
    pub fn open(path: impl AsRef<Path>, cache_capacity: usize) -> Result<Self> {
        let mut tree = Self {
            pager: Pager::open(path, cache_capacity)?,
            reusable_pages: VecDeque::new(),
            cache_capacity,
            instrumentation: BtreeInstrumentation::default(),
            operational_timing: OperationalTimingReport::default(),
            operational_step_index: None,
        };
        tree.validate_reachable_tree()?;
        tree.refresh_reusable_pages()?;
        Ok(tree)
    }

    /// Returns the backing path.
    #[must_use]
    pub fn path(&self) -> &Path {
        self.pager.path()
    }

    /// Returns the currently published root page id.
    #[must_use]
    pub fn root_page_id(&self) -> Option<u64> {
        self.pager.root_page_id()
    }

    /// Returns the number of committed data pages, including unreachable copy-on-write history.
    #[must_use]
    pub fn data_page_count(&self) -> u64 {
        self.pager.data_page_count()
    }

    /// Returns the committed metadata generation.
    #[must_use]
    pub const fn generation(&self) -> u64 {
        self.pager.generation()
    }

    /// Returns a copy of process-local B+ tree instrumentation counters.
    #[must_use]
    pub const fn instrumentation(&self) -> BtreeInstrumentation {
        self.instrumentation
    }

    /// Resets process-local amplification counters without modifying database state.
    pub fn reset_instrumentation(&mut self) {
        self.instrumentation = BtreeInstrumentation::default();
    }

    /// Builds the common exact amplification report for the current window and retained page file.
    ///
    /// The primary-structure numerator is every committed data page retained by the page file,
    /// including unreachable COW history that has not yet been recycled; the two mirrored
    /// superblocks are excluded. The live-byte denominator is reconstructed from the authoritative
    /// tree without incrementing the public read counters.
    pub fn amplification_report(&mut self) -> Result<AmplificationReport> {
        let rows = self.range_scan_uninstrumented(b"", None, usize::MAX)?;
        let live_bytes = rows.into_iter().try_fold(0_u64, |total, (key, value)| {
            let bytes = key
                .len()
                .checked_add(value.len())
                .ok_or_else(|| corruption(0, "B+ tree live logical byte count overflowed usize"))?;
            let bytes = u64::try_from(bytes)
                .map_err(|_| corruption(0, "B+ tree live logical byte count does not fit u64"))?;
            total
                .checked_add(bytes)
                .ok_or_else(|| corruption(0, "B+ tree live logical byte count overflowed u64"))
        })?;
        let primary_bytes = self
            .pager
            .data_page_count()
            .saturating_mul(u64::try_from(PAGE_SIZE).expect("page size fits u64"));
        Ok(AmplificationReport {
            point_read: StructuralReadAmplification {
                ratio: AmplificationRatio {
                    numerator: self.instrumentation.point_page_accesses,
                    denominator: self.instrumentation.point_reads,
                },
                unit: ReadWorkUnit::BtreePageAccess,
            },
            range_read: StructuralReadAmplification {
                ratio: AmplificationRatio {
                    numerator: self.instrumentation.range_page_accesses,
                    denominator: self.instrumentation.range_result_records,
                },
                unit: ReadWorkUnit::BtreePageAccess,
            },
            data_write_bytes_per_logical_byte: AmplificationRatio {
                numerator: self.instrumentation.data_page_bytes_written,
                denominator: self.instrumentation.logical_mutation_bytes,
            },
            primary_structure_bytes_per_live_byte: AmplificationRatio {
                numerator: primary_bytes,
                denominator: live_bytes,
            },
        })
    }

    pub(super) fn record_mutation(&mut self, logical_bytes: usize, data_write_before: u64) {
        self.instrumentation.logical_mutations =
            self.instrumentation.logical_mutations.saturating_add(1);
        self.instrumentation.logical_mutation_bytes = self
            .instrumentation
            .logical_mutation_bytes
            .saturating_add(u64::try_from(logical_bytes).unwrap_or(u64::MAX));
        self.instrumentation.data_page_bytes_written =
            self.instrumentation.data_page_bytes_written.saturating_add(
                self.pager
                    .data_page_bytes_written()
                    .saturating_sub(data_write_before),
            );
    }

    /// Returns tree height (`0` for empty, `1` for a leaf root).
    pub fn height(&mut self) -> Result<usize> {
        let Some(root) = self.pager.root_page_id() else {
            return Ok(0);
        };
        let mut seen = BTreeSet::new();
        Ok(self.validate_subtree(root, 0, &mut seen)?.height)
    }

    /// Looks up one opaque binary key and records structural page-access evidence.
    pub fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        let before = self.pager.read_page_calls();
        let result = self.get_uninstrumented(key);
        if result.is_ok() {
            self.instrumentation.point_reads = self.instrumentation.point_reads.saturating_add(1);
            self.instrumentation.point_page_accesses = self
                .instrumentation
                .point_page_accesses
                .saturating_add(self.pager.read_page_calls().saturating_sub(before));
        }
        result
    }

    pub(super) fn get_uninstrumented(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        let Some(mut page_id) = self.pager.root_page_id() else {
            return Ok(None);
        };

        for _ in 0..MAX_TREE_HEIGHT {
            let page = self.pager.read_page(page_id)?;
            match page.kind() {
                PageKind::Leaf => {
                    let entries = self.decode_leaf(&page, None)?;
                    return match entries.binary_search_by(|entry| entry.key.as_slice().cmp(key)) {
                        Ok(index) => {
                            let stored = entries[index].value.clone();
                            self.load_value(&stored).map(Some)
                        }
                        Err(_) => Ok(None),
                    };
                }
                PageKind::Internal => {
                    let children = self.decode_internal(&page, None)?;
                    page_id = children[route_child(&children, key)?].page_id;
                }
                PageKind::Overflow => {
                    return Err(corruption(
                        0,
                        format!("lookup reached overflow page {page_id} as a tree node"),
                    ));
                }
            }
        }

        Err(corruption(
            0,
            format!("B+ tree traversal exceeded maximum height {MAX_TREE_HEIGHT}"),
        ))
    }

    /// Inserts or replaces one binary key/value pair and returns the previous value.
    ///
    /// Keys through 4 KiB and values through 1 MiB are accepted. Keys that cannot remain inline
    /// while still leaving room for an overflow-value descriptor are stored in checksummed overflow
    /// pages. All key/value overflow pages are durable before the replacement leaf and final root are
    /// published.
    pub fn put(&mut self, key: &[u8], value: &[u8]) -> Result<Option<Vec<u8>>> {
        validate_key_value(key, value)?;
        let data_write_before = self.pager.data_page_bytes_written();
        let previous = self.get_uninstrumented(key)?;
        self.refresh_reusable_pages()?;
        let stored_key = self.store_key(key)?;
        let stored_value = self.store_value(&stored_key, value)?;

        let replacements = match self.pager.root_page_id() {
            Some(root) => self.rewrite_insert(root, &stored_key, &stored_value, 0)?,
            None => {
                let entry = LeafEntry {
                    key: stored_key.clone(),
                    value: stored_value.clone(),
                };
                vec![self.commit_leaf(&[entry])?]
            }
        };

        let new_root = match replacements.as_slice() {
            [only] => only.page_id,
            [_, _] => self.commit_internal(&replacements)?.page_id,
            _ => {
                return Err(corruption(
                    0,
                    "insert rewrite returned an invalid number of root replacements",
                ));
            }
        };
        self.pager.set_root(Some(new_root))?;
        self.record_mutation(key.len().saturating_add(value.len()), data_write_before);
        Ok(previous)
    }

    fn rewrite_insert(
        &mut self,
        page_id: u64,
        key: &StoredKey,
        value: &StoredValue,
        depth: usize,
    ) -> Result<Vec<ChildRef>> {
        if depth >= MAX_TREE_HEIGHT {
            return Err(corruption(
                0,
                format!("B+ tree insert exceeded maximum height {MAX_TREE_HEIGHT}"),
            ));
        }

        let page = self.pager.read_page(page_id)?;
        match page.kind() {
            PageKind::Leaf => {
                let mut entries = self.decode_leaf(&page, None)?;
                match entries.binary_search_by(|entry| entry.key.as_slice().cmp(key.as_slice())) {
                    Ok(index) => {
                        entries[index].key = key.clone();
                        entries[index].value = value.clone();
                    }
                    Err(index) => entries.insert(
                        index,
                        LeafEntry {
                            key: key.clone(),
                            value: value.clone(),
                        },
                    ),
                }
                self.commit_leaf_level(&entries)
            }
            PageKind::Internal => {
                let mut children = self.decode_internal(&page, None)?;
                let child_index = route_child(&children, key.as_slice())?;
                let child_id = children[child_index].page_id;
                let replacements = self.rewrite_insert(child_id, key, value, depth + 1)?;
                children.splice(child_index..=child_index, replacements);
                self.commit_internal_level(&children)
            }
            PageKind::Overflow => Err(corruption(
                0,
                format!("insert traversal reached overflow page {page_id} as a tree node"),
            )),
        }
    }

    fn commit_leaf_level(&mut self, entries: &[LeafEntry]) -> Result<Vec<ChildRef>> {
        if entries.is_empty() {
            return Err(corruption(
                0,
                "attempted to persist an empty reachable leaf",
            ));
        }
        let cells = entries
            .iter()
            .map(encode_leaf_cell)
            .collect::<Result<Vec<_>>>()?;
        if cells_fit(&cells) {
            return Ok(vec![self.commit_leaf_with_cells(entries, &cells)?]);
        }

        let split = choose_split(&cells).ok_or_else(|| {
            BtreeError::InvalidInput(
                "leaf entries cannot be divided into two valid 4 KiB pages".to_owned(),
            )
        })?;
        let left = self.commit_leaf(&entries[..split])?;
        let right = self.commit_leaf(&entries[split..])?;
        Ok(vec![left, right])
    }

    fn commit_internal_level(&mut self, children: &[ChildRef]) -> Result<Vec<ChildRef>> {
        validate_child_refs(children)?;
        let cells = children
            .iter()
            .map(encode_internal_cell)
            .collect::<Result<Vec<_>>>()?;
        if cells_fit(&cells) {
            return Ok(vec![self.commit_internal_with_cells(children, &cells)?]);
        }

        let split = choose_split(&cells).ok_or_else(|| {
            BtreeError::InvalidInput(
                "internal separators cannot be divided into two valid 4 KiB pages".to_owned(),
            )
        })?;
        let left = self.commit_internal(&children[..split])?;
        let right = self.commit_internal(&children[split..])?;
        Ok(vec![left, right])
    }

    fn commit_leaf(&mut self, entries: &[LeafEntry]) -> Result<ChildRef> {
        let cells = entries
            .iter()
            .map(encode_leaf_cell)
            .collect::<Result<Vec<_>>>()?;
        if !cells_fit(&cells) {
            return Err(BtreeError::InvalidInput(
                "leaf entries exceed one 4 KiB page".to_owned(),
            ));
        }
        self.commit_leaf_with_cells(entries, &cells)
    }

    fn commit_leaf_with_cells(
        &mut self,
        entries: &[LeafEntry],
        cells: &[Vec<u8>],
    ) -> Result<ChildRef> {
        let first = entries.first().ok_or_else(|| {
            BtreeError::InvalidInput("cannot persist an empty leaf page".to_owned())
        })?;
        let (mut page, recycled) = self.prepare_tree_page(PageKind::Leaf)?;
        for cell in cells {
            page.insert_cell(cell)?;
        }
        let page_id = self.commit_tree_page(page, recycled)?;
        Ok(ChildRef {
            min_key: first.key.as_slice().to_vec(),
            page_id,
        })
    }

    fn commit_internal(&mut self, children: &[ChildRef]) -> Result<ChildRef> {
        validate_child_refs(children)?;
        let cells = children
            .iter()
            .map(encode_internal_cell)
            .collect::<Result<Vec<_>>>()?;
        if !cells_fit(&cells) {
            return Err(BtreeError::InvalidInput(
                "internal separators exceed one 4 KiB page".to_owned(),
            ));
        }
        self.commit_internal_with_cells(children, &cells)
    }

    fn commit_internal_with_cells(
        &mut self,
        children: &[ChildRef],
        preview_cells: &[Vec<u8>],
    ) -> Result<ChildRef> {
        let first = children.first().ok_or_else(|| {
            BtreeError::InvalidInput("cannot persist an empty internal page".to_owned())
        })?;
        if preview_cells.len() != children.len() || !cells_fit(preview_cells) {
            return Err(corruption(
                0,
                "internal cell preview disagrees with committed page shape",
            ));
        }

        let mut cells = Vec::with_capacity(children.len());
        for child in children {
            let stored_key = self.store_key(&child.min_key)?;
            cells.push(encode_internal_cell_stored(&stored_key, child.page_id)?);
        }
        if cells
            .iter()
            .zip(preview_cells)
            .any(|(actual, preview)| actual.len() != preview.len())
            || !cells_fit(&cells)
        {
            return Err(corruption(
                0,
                "materialized internal separator cells changed their previewed size",
            ));
        }

        let (mut page, recycled) = self.prepare_tree_page(PageKind::Internal)?;
        for cell in &cells {
            page.insert_cell(cell)?;
        }
        let page_id = self.commit_tree_page(page, recycled)?;
        Ok(ChildRef {
            min_key: first.min_key.clone(),
            page_id,
        })
    }

    fn validate_reachable_tree(&mut self) -> Result<()> {
        let Some(root) = self.pager.root_page_id() else {
            return Ok(());
        };
        let mut seen = BTreeSet::new();
        self.validate_subtree(root, 0, &mut seen)?;
        Ok(())
    }

    fn validate_subtree(
        &mut self,
        page_id: u64,
        depth: usize,
        seen: &mut BTreeSet<u64>,
    ) -> Result<SubtreeBounds> {
        if depth >= MAX_TREE_HEIGHT {
            return Err(corruption(
                0,
                format!("reachable tree exceeds maximum height {MAX_TREE_HEIGHT}"),
            ));
        }
        if !seen.insert(page_id) {
            return Err(corruption(
                0,
                format!(
                    "reachable B+ tree contains a cycle or duplicate page reference at {page_id}"
                ),
            ));
        }

        let page = self.pager.read_page(page_id)?;
        match page.kind() {
            PageKind::Leaf => {
                if page.right_sibling().is_some() {
                    return Err(corruption(
                        0,
                        format!("reachable tree leaf page {page_id} has a sibling pointer; point-tree v1 requires zero"),
                    ));
                }
                let entries = self.decode_leaf(&page, Some(seen))?;
                for entry in &entries {
                    self.validate_value_reachability(&entry.value, seen)?;
                }
                let first = entries.first().ok_or_else(|| {
                    corruption(0, format!("reachable leaf page {page_id} is empty"))
                })?;
                let last = entries.last().expect("non-empty leaf has last entry");
                Ok(SubtreeBounds {
                    min_key: first.key.as_slice().to_vec(),
                    max_key: last.key.as_slice().to_vec(),
                    height: 1,
                })
            }
            PageKind::Internal => {
                let children = self.decode_internal(&page, Some(seen))?;
                if children.len() < 2 {
                    return Err(corruption(
                        0,
                        format!("reachable internal page {page_id} has fewer than two children"),
                    ));
                }

                let mut child_bounds = Vec::with_capacity(children.len());
                for child in &children {
                    let bounds = self.validate_subtree(child.page_id, depth + 1, seen)?;
                    if bounds.min_key != child.min_key {
                        return Err(corruption(
                            0,
                            format!(
                                "internal page {page_id} separator does not equal child {} minimum key",
                                child.page_id
                            ),
                        ));
                    }
                    child_bounds.push(bounds);
                }

                let height = child_bounds[0].height;
                for bounds in &child_bounds[1..] {
                    if bounds.height != height {
                        return Err(corruption(
                            0,
                            format!("internal page {page_id} has children at different heights"),
                        ));
                    }
                }
                for pair in child_bounds.windows(2) {
                    if pair[0].max_key >= pair[1].min_key {
                        return Err(corruption(
                            0,
                            format!("internal page {page_id} has overlapping child key ranges"),
                        ));
                    }
                }

                Ok(SubtreeBounds {
                    min_key: child_bounds[0].min_key.clone(),
                    max_key: child_bounds
                        .last()
                        .expect("internal page has children")
                        .max_key
                        .clone(),
                    height: height + 1,
                })
            }
            PageKind::Overflow => Err(corruption(
                0,
                format!("tree edge references overflow page {page_id} as a B+ tree node"),
            )),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct LeafEntry {
    key: StoredKey,
    value: StoredValue,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum StoredKey {
    Inline(Vec<u8>),
    Overflow { bytes: Vec<u8>, first_page_id: u64 },
}

impl StoredKey {
    fn as_slice(&self) -> &[u8] {
        match self {
            Self::Inline(bytes) | Self::Overflow { bytes, .. } => bytes,
        }
    }

    fn encoded_payload_len(&self) -> usize {
        match self {
            Self::Inline(bytes) => bytes.len(),
            Self::Overflow { .. } => KEY_OVERFLOW_REF_LEN,
        }
    }

    fn encoded_field(&self) -> Result<u16> {
        let len = u16::try_from(self.as_slice().len()).map_err(|_| {
            BtreeError::InvalidInput("key length does not fit the on-page u16 field".to_owned())
        })?;
        if len > KEY_LENGTH_MASK {
            return Err(BtreeError::InvalidInput(
                "key length collides with the overflow marker bit".to_owned(),
            ));
        }
        Ok(match self {
            Self::Inline(_) => len,
            Self::Overflow { .. } => KEY_OVERFLOW_FLAG | len,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum StoredValue {
    Inline(Vec<u8>),
    Overflow { len: u32, first_page_id: u64 },
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ChildRef {
    min_key: Vec<u8>,
    page_id: u64,
}

#[derive(Debug)]
struct SubtreeBounds {
    min_key: Vec<u8>,
    max_key: Vec<u8>,
    height: usize,
}

fn validate_key(key: &[u8]) -> Result<()> {
    if key.len() > MAX_TREE_KEY_BYTES {
        return Err(BtreeError::InvalidInput(format!(
            "B+ tree key has {} bytes; maximum is {MAX_TREE_KEY_BYTES}",
            key.len()
        )));
    }
    Ok(())
}

fn validate_key_value(key: &[u8], value: &[u8]) -> Result<()> {
    validate_key(key)?;
    if value.len() > MAX_TREE_VALUE_BYTES {
        return Err(BtreeError::InvalidInput(format!(
            "B+ tree value has {} bytes; maximum is {MAX_TREE_VALUE_BYTES}",
            value.len()
        )));
    }
    let value_len = u32::try_from(value.len()).map_err(|_| {
        BtreeError::InvalidInput("value length does not fit the on-page u32 field".to_owned())
    })?;
    if value_len > VALUE_LENGTH_MASK {
        return Err(BtreeError::InvalidInput(
            "value length collides with the overflow marker bit".to_owned(),
        ));
    }
    Ok(())
}

fn encode_leaf_cell(entry: &LeafEntry) -> Result<Vec<u8>> {
    validate_key(entry.key.as_slice())?;
    let key_field = entry.key.encoded_field()?;
    let key_payload_len = entry.key.encoded_payload_len();
    let (value_field, value_payload_len) = match &entry.value {
        StoredValue::Inline(value) => {
            if value.len() > MAX_TREE_VALUE_BYTES {
                return Err(BtreeError::InvalidInput(format!(
                    "B+ tree value has {} bytes; maximum is {MAX_TREE_VALUE_BYTES}",
                    value.len()
                )));
            }
            let len = u32::try_from(value.len()).map_err(|_| {
                BtreeError::InvalidInput("inline value length does not fit u32".to_owned())
            })?;
            (len, value.len())
        }
        StoredValue::Overflow { len, first_page_id } => {
            if *len == 0 || usize::try_from(*len).unwrap_or(usize::MAX) > MAX_TREE_VALUE_BYTES {
                return Err(BtreeError::InvalidInput(
                    "overflow value length is outside supported bounds".to_owned(),
                ));
            }
            if *first_page_id < SUPERBLOCK_COUNT {
                return Err(BtreeError::InvalidInput(format!(
                    "overflow value page {first_page_id} overlaps mirrored superblocks"
                )));
            }
            (VALUE_OVERFLOW_FLAG | *len, OVERFLOW_VALUE_REF_LEN)
        }
    };
    let mut cell = Vec::with_capacity(LEAF_CELL_HEADER_LEN + key_payload_len + value_payload_len);
    cell.extend_from_slice(&key_field.to_le_bytes());
    cell.extend_from_slice(&value_field.to_le_bytes());
    match &entry.key {
        StoredKey::Inline(key) => cell.extend_from_slice(key),
        StoredKey::Overflow { first_page_id, .. } => {
            cell.extend_from_slice(&first_page_id.to_le_bytes());
        }
    }
    match &entry.value {
        StoredValue::Inline(value) => cell.extend_from_slice(value),
        StoredValue::Overflow { first_page_id, .. } => {
            cell.extend_from_slice(&first_page_id.to_le_bytes());
        }
    }
    Ok(cell)
}

fn encode_internal_cell(child: &ChildRef) -> Result<Vec<u8>> {
    validate_key(&child.min_key)?;
    if child.page_id < SUPERBLOCK_COUNT {
        return Err(BtreeError::InvalidInput(format!(
            "child page {} overlaps mirrored superblocks",
            child.page_id
        )));
    }
    let len = u16::try_from(child.min_key.len()).map_err(|_| {
        BtreeError::InvalidInput("separator length does not fit the on-page u16 field".to_owned())
    })?;
    let overflow = child.min_key.len() > MAX_INLINE_KEY_BYTES;
    let field = if overflow {
        KEY_OVERFLOW_FLAG | len
    } else {
        len
    };
    let payload_len = if overflow {
        KEY_OVERFLOW_REF_LEN
    } else {
        child.min_key.len()
    };
    let mut cell = Vec::with_capacity(INTERNAL_CELL_HEADER_LEN + payload_len);
    cell.extend_from_slice(&field.to_le_bytes());
    cell.extend_from_slice(&child.page_id.to_le_bytes());
    if overflow {
        cell.extend_from_slice(&0_u64.to_le_bytes());
    } else {
        cell.extend_from_slice(&child.min_key);
    }
    Ok(cell)
}

fn encode_internal_cell_stored(key: &StoredKey, page_id: u64) -> Result<Vec<u8>> {
    validate_key(key.as_slice())?;
    if page_id < SUPERBLOCK_COUNT {
        return Err(BtreeError::InvalidInput(format!(
            "child page {page_id} overlaps mirrored superblocks"
        )));
    }
    let field = key.encoded_field()?;
    let mut cell = Vec::with_capacity(INTERNAL_CELL_HEADER_LEN + key.encoded_payload_len());
    cell.extend_from_slice(&field.to_le_bytes());
    cell.extend_from_slice(&page_id.to_le_bytes());
    match key {
        StoredKey::Inline(bytes) => cell.extend_from_slice(bytes),
        StoredKey::Overflow { first_page_id, .. } => {
            cell.extend_from_slice(&first_page_id.to_le_bytes());
        }
    }
    Ok(cell)
}

impl BPlusTree {
    fn decode_leaf(
        &mut self,
        page: &Page,
        mut seen: Option<&mut BTreeSet<u64>>,
    ) -> Result<Vec<LeafEntry>> {
        if page.kind() != PageKind::Leaf {
            return Err(corruption(0, "attempted leaf decoding on a non-leaf page"));
        }
        let mut entries = Vec::with_capacity(usize::from(page.cell_count()));
        for index in 0..page.cell_count() {
            let cell = page.cell(index)?;
            if cell.len() < LEAF_CELL_HEADER_LEN {
                return Err(corruption(
                    0,
                    format!(
                        "leaf page {} slot {index} is shorter than its cell header",
                        page.page_id()
                    ),
                ));
            }
            let key_field = u16::from_le_bytes([cell[0], cell[1]]);
            let key_overflow = key_field & KEY_OVERFLOW_FLAG != 0;
            let key_len_u16 = key_field & KEY_LENGTH_MASK;
            let key_len = usize::from(key_len_u16);
            if key_len > MAX_TREE_KEY_BYTES || (key_overflow && key_len == 0) {
                return Err(corruption(
                    0,
                    format!(
                        "leaf page {} contains invalid key length {key_len}",
                        page.page_id()
                    ),
                ));
            }
            let key_payload_len = if key_overflow {
                KEY_OVERFLOW_REF_LEN
            } else {
                key_len
            };

            let value_field = u32::from_le_bytes([cell[2], cell[3], cell[4], cell[5]]);
            let value_overflow = value_field & VALUE_OVERFLOW_FLAG != 0;
            let value_len_u32 = value_field & VALUE_LENGTH_MASK;
            let value_len = usize::try_from(value_len_u32)
                .map_err(|_| corruption(0, "leaf value length does not fit usize"))?;
            if value_len > MAX_TREE_VALUE_BYTES || (value_overflow && value_len == 0) {
                return Err(corruption(
                    0,
                    format!(
                        "leaf page {} contains invalid value length {value_len}",
                        page.page_id()
                    ),
                ));
            }
            let value_payload_len = if value_overflow {
                OVERFLOW_VALUE_REF_LEN
            } else {
                value_len
            };
            let expected = LEAF_CELL_HEADER_LEN
                .checked_add(key_payload_len)
                .and_then(|length| length.checked_add(value_payload_len))
                .ok_or_else(|| corruption(0, "leaf cell decoded length overflowed usize"))?;
            if expected != cell.len() {
                return Err(corruption(
                    0,
                    format!(
                        "leaf page {} slot {index} encodes {expected} bytes but slot contains {}",
                        page.page_id(),
                        cell.len()
                    ),
                ));
            }

            let key_start = LEAF_CELL_HEADER_LEN;
            let key_end = key_start + key_payload_len;
            let key = if key_overflow {
                let bytes = &cell[key_start..key_end];
                let first_page_id = u64::from_le_bytes([
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                ]);
                self.load_key_blob(first_page_id, key_len_u16, seen.as_deref_mut())?
            } else {
                StoredKey::Inline(cell[key_start..key_end].to_vec())
            };

            let value = if value_overflow {
                let bytes = &cell[key_end..key_end + OVERFLOW_VALUE_REF_LEN];
                let first_page_id = u64::from_le_bytes([
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                ]);
                if first_page_id < SUPERBLOCK_COUNT {
                    return Err(corruption(
                        0,
                        format!(
                            "leaf page {} references invalid overflow value page {first_page_id}",
                            page.page_id()
                        ),
                    ));
                }
                StoredValue::Overflow {
                    len: value_len_u32,
                    first_page_id,
                }
            } else {
                StoredValue::Inline(cell[key_end..].to_vec())
            };
            entries.push(LeafEntry { key, value });
        }
        validate_leaf_order(&entries, page.page_id())?;
        Ok(entries)
    }

    fn decode_internal(
        &mut self,
        page: &Page,
        mut seen: Option<&mut BTreeSet<u64>>,
    ) -> Result<Vec<ChildRef>> {
        if page.kind() != PageKind::Internal {
            return Err(corruption(
                0,
                "attempted internal decoding on a non-internal page",
            ));
        }
        let mut children = Vec::with_capacity(usize::from(page.cell_count()));
        for index in 0..page.cell_count() {
            let cell = page.cell(index)?;
            if cell.len() < INTERNAL_CELL_HEADER_LEN {
                return Err(corruption(
                    0,
                    format!(
                        "internal page {} slot {index} is shorter than its cell header",
                        page.page_id()
                    ),
                ));
            }
            let key_field = u16::from_le_bytes([cell[0], cell[1]]);
            let key_overflow = key_field & KEY_OVERFLOW_FLAG != 0;
            let key_len_u16 = key_field & KEY_LENGTH_MASK;
            let key_len = usize::from(key_len_u16);
            if key_len > MAX_TREE_KEY_BYTES || (key_overflow && key_len == 0) {
                return Err(corruption(
                    0,
                    format!(
                        "internal page {} contains invalid separator length {key_len}",
                        page.page_id()
                    ),
                ));
            }
            let key_payload_len = if key_overflow {
                KEY_OVERFLOW_REF_LEN
            } else {
                key_len
            };
            let expected = INTERNAL_CELL_HEADER_LEN
                .checked_add(key_payload_len)
                .ok_or_else(|| corruption(0, "internal cell decoded length overflowed usize"))?;
            if expected != cell.len() {
                return Err(corruption(
                    0,
                    format!(
                        "internal page {} slot {index} encodes {expected} bytes but slot contains {}",
                        page.page_id(), cell.len()
                    ),
                ));
            }
            let page_id = u64::from_le_bytes([
                cell[2], cell[3], cell[4], cell[5], cell[6], cell[7], cell[8], cell[9],
            ]);
            if page_id < SUPERBLOCK_COUNT {
                return Err(corruption(
                    0,
                    format!(
                        "internal page {} references invalid child {page_id}",
                        page.page_id()
                    ),
                ));
            }
            let key = if key_overflow {
                let bytes = &cell[INTERNAL_CELL_HEADER_LEN..];
                let first_page_id = u64::from_le_bytes([
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                ]);
                self.load_key_blob(first_page_id, key_len_u16, seen.as_deref_mut())?
                    .as_slice()
                    .to_vec()
            } else {
                cell[INTERNAL_CELL_HEADER_LEN..].to_vec()
            };
            children.push(ChildRef {
                min_key: key,
                page_id,
            });
        }
        validate_child_refs(&children)?;
        Ok(children)
    }
}

fn validate_leaf_order(entries: &[LeafEntry], page_id: u64) -> Result<()> {
    for pair in entries.windows(2) {
        if pair[0].key.as_slice() >= pair[1].key.as_slice() {
            return Err(corruption(
                0,
                format!("leaf page {page_id} keys are not strictly increasing"),
            ));
        }
    }
    Ok(())
}

fn validate_child_refs(children: &[ChildRef]) -> Result<()> {
    if children.is_empty() {
        return Err(BtreeError::InvalidInput(
            "internal page requires at least one child".to_owned(),
        ));
    }
    for child in children {
        validate_key(&child.min_key)?;
        if child.page_id < SUPERBLOCK_COUNT {
            return Err(BtreeError::InvalidInput(format!(
                "internal child {} overlaps mirrored superblocks",
                child.page_id
            )));
        }
    }
    for pair in children.windows(2) {
        if pair[0].min_key >= pair[1].min_key {
            return Err(corruption(
                0,
                "internal separator keys are not strictly increasing",
            ));
        }
    }
    Ok(())
}

fn route_child(children: &[ChildRef], key: &[u8]) -> Result<usize> {
    validate_child_refs(children)?;
    match children.binary_search_by(|child| child.min_key.as_slice().cmp(key)) {
        Ok(index) => Ok(index),
        Err(0) => Ok(0),
        Err(index) => Ok(index - 1),
    }
}

fn cells_fit(cells: &[Vec<u8>]) -> bool {
    cells
        .iter()
        .try_fold(0_usize, |used, cell| {
            used.checked_add(SLOT_LEN)?.checked_add(cell.len())
        })
        .is_some_and(|used| used <= PAGE_BODY_CAPACITY)
}

fn choose_split(cells: &[Vec<u8>]) -> Option<usize> {
    if cells.len() < 2 {
        return None;
    }
    let sizes = cells
        .iter()
        .map(|cell| SLOT_LEN.checked_add(cell.len()))
        .collect::<Option<Vec<_>>>()?;
    let total = sizes
        .iter()
        .try_fold(0_usize, |sum, size| sum.checked_add(*size))?;
    let mut left = 0_usize;
    let mut best: Option<(usize, usize)> = None;
    for split in 1..cells.len() {
        left = left.checked_add(sizes[split - 1])?;
        let right = total.checked_sub(left)?;
        if left <= PAGE_BODY_CAPACITY && right <= PAGE_BODY_CAPACITY {
            let imbalance = left.abs_diff(right);
            if best.is_none_or(|(_, current)| imbalance < current) {
                best = Some((split, imbalance));
            }
        }
    }
    best.map(|(split, _)| split)
}

#[cfg(test)]
mod tests {
    use tempfile::tempdir;

    use super::{
        encode_leaf_cell, BPlusTree, LeafEntry, StoredKey, MAX_TREE_KEY_BYTES, MAX_TREE_VALUE_BYTES,
    };
    use crate::PageKind;

    #[test]
    fn binary_lookup_insert_update_and_reopen() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("tree.db");
        let mut tree = BPlusTree::create_new(&path, 8).expect("create tree");

        assert_eq!(tree.get(b"").expect("get empty key"), None);
        assert_eq!(tree.put(b"", b"zero").expect("insert empty key"), None);
        assert_eq!(
            tree.put(&[0xff, 0x00, 0x7f], b"binary")
                .expect("insert binary key"),
            None
        );
        assert_eq!(
            tree.get(b"").expect("read empty key"),
            Some(b"zero".to_vec())
        );
        assert_eq!(
            tree.put(b"", b"updated").expect("update empty key"),
            Some(b"zero".to_vec())
        );
        assert_eq!(tree.height().expect("height"), 1);
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 4).expect("reopen tree");
        assert_eq!(
            reopened.get(b"").expect("read updated value"),
            Some(b"updated".to_vec())
        );
        assert_eq!(
            reopened
                .get(&[0xff, 0x00, 0x7f])
                .expect("read binary value"),
            Some(b"binary".to_vec())
        );
        assert_eq!(reopened.get(b"missing").expect("read missing"), None);
    }

    #[test]
    fn root_and_non_root_splits_preserve_every_key() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("splits.db");
        let mut tree = BPlusTree::create_new(&path, 3).expect("create tree");
        let mut keys = Vec::new();

        for number in 0_u16..20 {
            let mut key = vec![b'k'; 510];
            key.extend_from_slice(&number.to_be_bytes());
            let value = vec![(number % 251) as u8; 900];
            tree.put(&key, &value).expect("insert split workload");
            keys.push((key, value));
        }

        assert!(tree.height().expect("height after splits") >= 3);
        for (key, value) in &keys {
            assert_eq!(
                tree.get(key).expect("lookup after splits"),
                Some(value.clone())
            );
        }
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 2).expect("reopen split tree");
        assert!(reopened.height().expect("reopened height") >= 3);
        for (key, value) in &keys {
            assert_eq!(
                reopened.get(key).expect("lookup after reopen"),
                Some(value.clone())
            );
        }
    }

    #[test]
    fn unpublished_shadow_page_does_not_change_committed_root() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("shadow.db");
        let mut tree = BPlusTree::create_new(&path, 2).expect("create tree");
        tree.put(b"key", b"old").expect("insert committed value");
        let committed_root = tree.root_page_id();

        let entry = LeafEntry {
            key: StoredKey::Inline(b"key".to_vec()),
            value: super::StoredValue::Inline(b"new-but-unpublished".to_vec()),
        };
        let cell = encode_leaf_cell(&entry).expect("encode shadow entry");
        let mut shadow = tree
            .pager
            .prepare_new_page(PageKind::Leaf)
            .expect("prepare shadow page");
        shadow.insert_cell(&cell).expect("pack shadow page");
        tree.pager
            .commit_new_page(shadow)
            .expect("commit unreachable shadow page");
        assert_eq!(tree.root_page_id(), committed_root);
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 2).expect("reopen tree");
        assert_eq!(reopened.root_page_id(), committed_root);
        assert_eq!(
            reopened.get(b"key").expect("read authoritative value"),
            Some(b"old".to_vec())
        );
    }

    #[test]
    fn four_kib_keys_with_prefix_adjacency_round_trip_split_and_reopen() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("long-keys.db");
        let mut tree = BPlusTree::create_new(&path, 8).expect("create tree");
        let mut keys = Vec::new();

        let prefix = vec![0x7a; MAX_TREE_KEY_BYTES - 1];
        for suffix in 0_u8..96 {
            let mut key = prefix.clone();
            key.push(suffix);
            let value = vec![suffix; 64];
            tree.put(&key, &value).expect("insert maximum key");
            keys.push((key, value));
        }
        let immediate_prefix = prefix.clone();
        tree.put(&immediate_prefix, b"prefix")
            .expect("insert prefix-adjacent key");

        assert!(tree.height().expect("height after long-key inserts") >= 2);
        assert_eq!(
            tree.get(&immediate_prefix).expect("prefix lookup"),
            Some(b"prefix".to_vec())
        );
        for (key, value) in &keys {
            assert_eq!(tree.get(key).expect("long-key lookup"), Some(value.clone()));
        }
        drop(tree);

        let mut reopened = BPlusTree::open(&path, 4).expect("reopen long-key tree");
        assert_eq!(
            reopened.get(&immediate_prefix).expect("reopened prefix"),
            Some(b"prefix".to_vec())
        );
        for (key, value) in &keys {
            assert_eq!(
                reopened.get(key).expect("reopened long-key lookup"),
                Some(value.clone())
            );
        }
        for (key, value) in keys.iter().take(95) {
            assert_eq!(
                reopened.delete(key).expect("delete long key"),
                Some(value.clone())
            );
        }
        assert_eq!(
            reopened.get(&keys[95].0).expect("surviving long key"),
            Some(keys[95].1.clone())
        );
    }

    #[test]
    fn oversized_key_or_value_is_rejected_before_writing() {
        let directory = tempdir().expect("temporary directory");
        let path = directory.path().join("bounds.db");
        let mut tree = BPlusTree::create_new(&path, 1).expect("create tree");
        let initial_pages = tree.data_page_count();

        let error = tree
            .put(&vec![0_u8; MAX_TREE_KEY_BYTES + 1], b"value")
            .expect_err("oversized key must fail");
        assert!(matches!(error, crate::BtreeError::InvalidInput(_)));
        assert_eq!(tree.data_page_count(), initial_pages);

        let error = tree
            .put(b"key", &vec![0_u8; MAX_TREE_VALUE_BYTES + 1])
            .expect_err("oversized value must fail");
        assert!(matches!(error, crate::BtreeError::InvalidInput(_)));
        assert_eq!(tree.data_page_count(), initial_pages);
    }
}
