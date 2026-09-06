//! Checksummed copy-on-write B+ tree storage for the database laboratory.
//!
//! Fixed 4 KiB slotted pages, mirrored superblocks, synchronized immutable allocation, and a bounded
//! validated-page cache form the physical layer. The tree layer adds binary point lookup, insertion/
//! update/deletion, split/rebalance/root contraction, reachability-derived page reuse, and checksummed
//! overflow chains for keys through 4 KiB and values through 1 MiB. Mutations synchronize key/value
//! overflow pages and replacement tree pages before atomically publishing a new root. The tree now
//! implements the common `KvEngine` point contract plus bounded half-open ordered scans by walking
//! internal children in key order. Deterministic mutation fault tests inject pre-write, torn-half-
//! write, and post-sync errors across appended/recycled data pages plus allocation/root superblocks;
//! physical file compaction and exhaustive device/syscall failure modeling remain deferred.

use std::collections::{BTreeMap, VecDeque};
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use thiserror::Error;

mod tree;

pub use tree::{BPlusTree, BtreeInstrumentation, MAX_TREE_KEY_BYTES, MAX_TREE_VALUE_BYTES};

/// Fixed physical page size for B+ tree format v1.
pub const PAGE_SIZE: usize = 4096;
/// Number of mirrored metadata pages at the beginning of every page file.
pub const SUPERBLOCK_COUNT: u64 = 2;

const PAGE_SIZE_U64: u64 = PAGE_SIZE as u64;
const FORMAT_VERSION: u16 = 1;
const SUPERBLOCK_MAGIC: [u8; 8] = *b"DBBPTRE\0";
const PAGE_MAGIC: [u8; 4] = *b"BTPG";
const CHECKSUM_OFFSET: usize = PAGE_SIZE - 4;
const DATA_HEADER_LEN: usize = 40;
const SLOT_LEN: usize = 4;
const ROOT_NONE: u64 = 0;
const KIND_LEAF: u8 = 1;
const KIND_INTERNAL: u8 = 2;
const KIND_OVERFLOW: u8 = 3;

/// Result type returned by the B+ tree page/pager foundation.
pub type Result<T> = std::result::Result<T, BtreeError>;

/// Errors produced while validating or persisting B+ tree pages.
#[derive(Debug, Error)]
pub enum BtreeError {
    /// The caller requested an invalid page operation.
    #[error("invalid input: {0}")]
    InvalidInput(String),
    /// An operating-system I/O operation failed.
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
    /// Persistent bytes violate the page-file format.
    #[error("corrupt B+ tree storage at byte offset {offset}: {reason}")]
    Corruption {
        /// Absolute byte offset associated with the validation failure.
        offset: u64,
        /// Human-readable validation failure.
        reason: String,
    },
    /// A checksummed page uses a format version this build does not understand.
    #[error("unsupported B+ tree format version {found}; this build supports version {supported}")]
    UnsupportedVersion {
        /// Version found in the encoded page.
        found: u64,
        /// Version supported by this build.
        supported: u64,
    },
    /// A write had an ambiguous outcome and the pager must be reopened.
    #[error("pager is poisoned by a previous write failure; reopen it before continuing")]
    Poisoned,
}

/// Physical role of a data page.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PageKind {
    /// Leaf page containing sorted key/value cells or overflow-value references.
    Leaf,
    /// Internal page containing separator/child cells.
    Internal,
    /// Overflow page containing one chunk of a large value and an optional next-page link.
    Overflow,
}

impl PageKind {
    const fn encoded(self) -> u8 {
        match self {
            Self::Leaf => KIND_LEAF,
            Self::Internal => KIND_INTERNAL,
            Self::Overflow => KIND_OVERFLOW,
        }
    }

    fn decode(encoded: u8, offset: u64) -> Result<Self> {
        match encoded {
            KIND_LEAF => Ok(Self::Leaf),
            KIND_INTERNAL => Ok(Self::Internal),
            KIND_OVERFLOW => Ok(Self::Overflow),
            _ => Err(corruption(offset, format!("unknown page kind {encoded}"))),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DurableWriteKind {
    AppendPage(PageKind),
    RecycledPage(PageKind),
    AllocationSuperblock,
    RootSuperblock,
}

#[cfg(test)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FaultMode {
    BeforeWrite,
    TornWrite,
    AfterSync,
}

#[cfg(test)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct FaultSpec {
    event_index: usize,
    mode: FaultMode,
}

#[cfg(test)]
fn injected_fault(kind: DurableWriteKind, mode: FaultMode) -> BtreeError {
    BtreeError::Io(io::Error::other(format!(
        "injected durable-write fault at {kind:?} with mode {mode:?}"
    )))
}

/// One validated 4 KiB slotted page value.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Page {
    bytes: [u8; PAGE_SIZE],
    page_id: u64,
    kind: PageKind,
}

impl Page {
    fn new(page_id: u64, kind: PageKind) -> Result<Self> {
        if page_id < SUPERBLOCK_COUNT {
            return Err(BtreeError::InvalidInput(format!(
                "data page id {page_id} overlaps mirrored superblocks"
            )));
        }

        let mut bytes = [0_u8; PAGE_SIZE];
        bytes[..4].copy_from_slice(&PAGE_MAGIC);
        bytes[4] = FORMAT_VERSION as u8;
        bytes[5] = kind.encoded();
        bytes[6..8].copy_from_slice(&0_u16.to_le_bytes());
        bytes[8..16].copy_from_slice(&page_id.to_le_bytes());
        bytes[16..24].copy_from_slice(&0_u64.to_le_bytes());
        bytes[24..26].copy_from_slice(&(DATA_HEADER_LEN as u16).to_le_bytes());
        bytes[26..28].copy_from_slice(&(CHECKSUM_OFFSET as u16).to_le_bytes());
        bytes[28..30].copy_from_slice(&0_u16.to_le_bytes());
        bytes[30..32].copy_from_slice(&0_u16.to_le_bytes());
        bytes[32..40].copy_from_slice(&0_u64.to_le_bytes());
        refresh_checksum(&mut bytes);

        Ok(Self {
            bytes,
            page_id,
            kind,
        })
    }

    fn decode(bytes: [u8; PAGE_SIZE], expected_page_id: u64) -> Result<Self> {
        let kind = validate_page_bytes(&bytes, expected_page_id)?;
        Ok(Self {
            bytes,
            page_id: expected_page_id,
            kind,
        })
    }

    /// Returns the physical page id encoded in this page.
    #[must_use]
    pub const fn page_id(&self) -> u64 {
        self.page_id
    }

    /// Returns whether this is a leaf or internal page.
    #[must_use]
    pub const fn kind(&self) -> PageKind {
        self.kind
    }

    /// Number of occupied slots.
    #[must_use]
    pub fn cell_count(&self) -> u16 {
        read_u16(&self.bytes[28..30])
    }

    /// Bytes still available between the slot array and packed cell payloads.
    #[must_use]
    pub fn free_bytes(&self) -> usize {
        let lower = usize::from(read_u16(&self.bytes[24..26]));
        let upper = usize::from(read_u16(&self.bytes[26..28]));
        upper.saturating_sub(lower)
    }

    fn page_link(&self) -> Option<u64> {
        match read_u64(&self.bytes[32..40]) {
            ROOT_NONE => None,
            page_id => Some(page_id),
        }
    }

    /// Optional right sibling used only by tree leaf pages.
    #[must_use]
    pub fn right_sibling(&self) -> Option<u64> {
        if self.kind == PageKind::Leaf {
            self.page_link()
        } else {
            None
        }
    }

    /// Optional next page used only by overflow-value pages.
    #[must_use]
    pub fn overflow_next(&self) -> Option<u64> {
        if self.kind == PageKind::Overflow {
            self.page_link()
        } else {
            None
        }
    }

    /// Sets a leaf's right-sibling pointer before the page is committed.
    pub fn set_right_sibling(&mut self, sibling: Option<u64>) -> Result<()> {
        self.validate()?;
        if self.kind != PageKind::Leaf {
            return Err(BtreeError::InvalidInput(
                "right sibling is only defined for leaf pages".to_owned(),
            ));
        }
        let encoded = sibling.unwrap_or(ROOT_NONE);
        if encoded != ROOT_NONE && encoded < SUPERBLOCK_COUNT {
            return Err(BtreeError::InvalidInput(format!(
                "right sibling {encoded} overlaps mirrored superblocks"
            )));
        }
        if encoded == self.page_id {
            return Err(BtreeError::InvalidInput(
                "leaf cannot point to itself as right sibling".to_owned(),
            ));
        }
        self.bytes[32..40].copy_from_slice(&encoded.to_le_bytes());
        refresh_checksum(&mut self.bytes);
        Ok(())
    }

    /// Sets an overflow page's next-page link before the page is committed.
    pub fn set_overflow_next(&mut self, next: Option<u64>) -> Result<()> {
        self.validate()?;
        if self.kind != PageKind::Overflow {
            return Err(BtreeError::InvalidInput(
                "overflow next-page link is only defined for overflow pages".to_owned(),
            ));
        }
        let encoded = next.unwrap_or(ROOT_NONE);
        if encoded != ROOT_NONE && encoded < SUPERBLOCK_COUNT {
            return Err(BtreeError::InvalidInput(format!(
                "overflow next page {encoded} overlaps mirrored superblocks"
            )));
        }
        if encoded == self.page_id {
            return Err(BtreeError::InvalidInput(
                "overflow page cannot point to itself".to_owned(),
            ));
        }
        self.bytes[32..40].copy_from_slice(&encoded.to_le_bytes());
        refresh_checksum(&mut self.bytes);
        Ok(())
    }

    /// Inserts one opaque non-empty cell into the slotted-page body.
    ///
    /// The page layer deliberately does not interpret keys or values yet. Cells are packed from the
    /// end of the page downward while four-byte `(offset, length)` slots grow upward.
    pub fn insert_cell(&mut self, cell: &[u8]) -> Result<u16> {
        self.validate()?;
        if cell.is_empty() {
            return Err(BtreeError::InvalidInput(
                "slotted-page cells must be non-empty".to_owned(),
            ));
        }
        let cell_len = u16::try_from(cell.len()).map_err(|_| {
            BtreeError::InvalidInput(format!(
                "cell has {} bytes; length does not fit u16",
                cell.len()
            ))
        })?;
        let needed = SLOT_LEN
            .checked_add(cell.len())
            .ok_or_else(|| BtreeError::InvalidInput("cell extent overflowed usize".to_owned()))?;
        let available = self.free_bytes();
        if needed > available {
            return Err(BtreeError::InvalidInput(format!(
                "page {} has {available} free bytes; cell requires {needed}",
                self.page_id
            )));
        }

        let lower = usize::from(read_u16(&self.bytes[24..26]));
        let upper = usize::from(read_u16(&self.bytes[26..28]));
        let count = self.cell_count();
        let next_count = count.checked_add(1).ok_or_else(|| {
            BtreeError::InvalidInput("slotted-page cell counter exhausted".to_owned())
        })?;
        let new_upper = upper
            .checked_sub(cell.len())
            .ok_or_else(|| BtreeError::InvalidInput("cell underflowed page payload".to_owned()))?;
        let new_lower = lower
            .checked_add(SLOT_LEN)
            .ok_or_else(|| BtreeError::InvalidInput("slot array overflowed usize".to_owned()))?;

        self.bytes[new_upper..upper].copy_from_slice(cell);
        self.bytes[lower..lower + 2].copy_from_slice(&(new_upper as u16).to_le_bytes());
        self.bytes[lower + 2..lower + 4].copy_from_slice(&cell_len.to_le_bytes());
        self.bytes[24..26].copy_from_slice(&(new_lower as u16).to_le_bytes());
        self.bytes[26..28].copy_from_slice(&(new_upper as u16).to_le_bytes());
        self.bytes[28..30].copy_from_slice(&next_count.to_le_bytes());
        refresh_checksum(&mut self.bytes);
        self.validate()?;
        Ok(count)
    }

    /// Returns an inserted cell by slot index.
    pub fn cell(&self, index: u16) -> Result<&[u8]> {
        self.validate()?;
        if index >= self.cell_count() {
            return Err(BtreeError::InvalidInput(format!(
                "cell index {index} is outside page {} with {} cells",
                self.page_id,
                self.cell_count()
            )));
        }
        let slot = DATA_HEADER_LEN + usize::from(index) * SLOT_LEN;
        let offset = usize::from(read_u16(&self.bytes[slot..slot + 2]));
        let length = usize::from(read_u16(&self.bytes[slot + 2..slot + 4]));
        Ok(&self.bytes[offset..offset + length])
    }

    fn validate(&self) -> Result<()> {
        let decoded_kind = validate_page_bytes(&self.bytes, self.page_id)?;
        if decoded_kind != self.kind {
            return Err(corruption(
                page_offset(self.page_id)?,
                "decoded page kind differs from in-memory page metadata",
            ));
        }
        Ok(())
    }
}

/// A trailing physical extent discarded because no valid superblock committed it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecoveredAllocation {
    /// Page id the uncommitted bytes would have occupied.
    pub page_id: u64,
    /// Number of physical bytes discarded.
    pub available_bytes: u64,
}

/// File-backed pager with mirrored metadata and a bounded cache of validated page images.
pub struct Pager {
    path: PathBuf,
    file: File,
    active: Superblock,
    cache: PageCache,
    recovered_allocation: Option<RecoveredAllocation>,
    read_page_calls: u64,
    data_page_bytes_written: u64,
    poisoned: bool,
    #[cfg(test)]
    fault_spec: Option<FaultSpec>,
    #[cfg(test)]
    fault_trace: Vec<DurableWriteKind>,
}

impl std::fmt::Debug for Pager {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Pager")
            .field("path", &self.path)
            .field("active_superblock", &self.active.slot)
            .field("generation", &self.active.generation)
            .field("page_count", &self.active.page_count)
            .field("root_page_id", &self.root_page_id())
            .field("cached_pages", &self.cache.len())
            .field("recovered_allocation", &self.recovered_allocation)
            .field("poisoned", &self.poisoned)
            .finish()
    }
}

impl Pager {
    /// Creates a new page file with two synchronized superblock copies.
    ///
    /// `cache_capacity` must be greater than zero. Existing paths are never overwritten.
    pub fn create_new(path: impl AsRef<Path>, cache_capacity: usize) -> Result<Self> {
        validate_cache_capacity(cache_capacity)?;
        let path = path.as_ref().to_path_buf();
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .open(&path)?;
        let first = Superblock::initial(0);
        let second = Superblock::initial(1);
        file.write_all(&first.encode())?;
        file.write_all(&second.encode())?;
        file.sync_all()?;

        Ok(Self {
            path,
            file,
            active: first,
            cache: PageCache::new(cache_capacity),
            recovered_allocation: None,
            read_page_calls: 0,
            data_page_bytes_written: 0,
            poisoned: false,
            #[cfg(test)]
            fault_spec: None,
            #[cfg(test)]
            fault_trace: Vec::new(),
        })
    }

    /// Opens and validates an existing page file.
    ///
    /// The newest valid mirrored superblock is authoritative. At most one trailing page of bytes
    /// beyond its committed `page_count` is an interrupted allocation and is truncated. Missing
    /// committed bytes, more than one trailing page, or invalid committed pages fail closed.
    pub fn open(path: impl AsRef<Path>, cache_capacity: usize) -> Result<Self> {
        validate_cache_capacity(cache_capacity)?;
        let path = path.as_ref().to_path_buf();
        let mut file = OpenOptions::new().read(true).write(true).open(&path)?;
        let physical_bytes = file.metadata()?.len();
        let minimum = SUPERBLOCK_COUNT
            .checked_mul(PAGE_SIZE_U64)
            .ok_or_else(|| corruption(0, "minimum file extent overflowed u64"))?;
        if physical_bytes < minimum {
            return Err(corruption(
                0,
                format!("page file has {physical_bytes} bytes; two superblocks require {minimum}"),
            ));
        }

        let first = read_superblock(&mut file, 0);
        let second = read_superblock(&mut file, 1);
        let active = choose_superblock(first, second)?;
        let committed_bytes = committed_file_bytes(active.page_count)?;
        if physical_bytes < committed_bytes {
            return Err(corruption(
                physical_bytes,
                format!(
                    "file ends before committed extent: superblock commits {committed_bytes} bytes"
                ),
            ));
        }

        let recovered_allocation = if physical_bytes > committed_bytes {
            let extra = physical_bytes - committed_bytes;
            if extra > PAGE_SIZE_U64 {
                return Err(corruption(
                    committed_bytes,
                    format!(
                        "found {extra} bytes beyond committed extent; one interrupted page is the maximum recoverable tail"
                    ),
                ));
            }
            file.set_len(committed_bytes)?;
            file.sync_all()?;
            Some(RecoveredAllocation {
                page_id: active.page_count,
                available_bytes: extra,
            })
        } else {
            None
        };

        Ok(Self {
            path,
            file,
            active,
            cache: PageCache::new(cache_capacity),
            recovered_allocation,
            read_page_calls: 0,
            data_page_bytes_written: 0,
            poisoned: false,
            #[cfg(test)]
            fault_spec: None,
            #[cfg(test)]
            fault_trace: Vec::new(),
        })
    }

    /// Path backing this pager.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Number of committed data pages, excluding the two superblocks.
    #[must_use]
    pub fn data_page_count(&self) -> u64 {
        self.active.page_count - SUPERBLOCK_COUNT
    }

    /// Current committed root page id, if a tree root has been installed.
    #[must_use]
    pub fn root_page_id(&self) -> Option<u64> {
        match self.active.root_page_id {
            ROOT_NONE => None,
            page_id => Some(page_id),
        }
    }

    /// Superblock generation selected during open or the most recent metadata commit.
    #[must_use]
    pub const fn generation(&self) -> u64 {
        self.active.generation
    }

    /// Number of validated pages currently retained by the bounded cache.
    #[must_use]
    pub fn cached_pages(&self) -> usize {
        self.cache.len()
    }

    /// Interrupted allocation repaired by the most recent `open`, if any.
    #[must_use]
    pub const fn recovered_allocation(&self) -> Option<RecoveredAllocation> {
        self.recovered_allocation
    }

    /// Creates an uncommitted empty page value using the next physical page id.
    ///
    /// Multiple prepared values may exist, but only the one whose id still equals the next committed
    /// id can be passed to `commit_new_page`.
    pub fn prepare_new_page(&self, kind: PageKind) -> Result<Page> {
        self.ensure_usable()?;
        Page::new(self.active.page_count, kind)
    }

    /// Appends and commits one prepared immutable page.
    ///
    /// The page bytes are written and synchronized first. Only then is a newer mirrored superblock
    /// written and synchronized with `page_count + 1`. An I/O failure poisons the handle because the
    /// commit outcome may be ambiguous; reopening chooses the newest valid superblock and either
    /// accepts or discards the trailing allocation.
    pub fn commit_new_page(&mut self, page: Page) -> Result<u64> {
        self.ensure_usable()?;
        page.validate()?;
        let expected_page_id = self.active.page_count;
        if page.page_id != expected_page_id {
            return Err(BtreeError::InvalidInput(format!(
                "prepared page id {} is stale; next committed page id is {expected_page_id}",
                page.page_id
            )));
        }

        let next_generation = self.active.generation.checked_add(1).ok_or_else(|| {
            corruption(0, "superblock generation exhausted before page allocation")
        })?;
        let next_page_count = self.active.page_count.checked_add(1).ok_or_else(|| {
            corruption(
                0,
                "superblock page counter exhausted before page allocation",
            )
        })?;
        let next = Superblock {
            slot: alternate_slot(self.active.slot),
            generation: next_generation,
            page_count: next_page_count,
            root_page_id: self.active.root_page_id,
        };

        let expected_bytes = committed_file_bytes(self.active.page_count)?;
        let physical_bytes = self.file.metadata()?.len();
        if physical_bytes != expected_bytes {
            return Err(corruption(
                physical_bytes.min(expected_bytes),
                format!(
                    "live pager physical extent is {physical_bytes} bytes; expected {expected_bytes} before allocation"
                ),
            ));
        }

        if let Err(error) = self.write_durable_bytes(
            expected_bytes,
            &page.bytes,
            DurableWriteKind::AppendPage(page.kind()),
        ) {
            self.poisoned = true;
            return Err(error);
        }
        if let Err(error) =
            self.write_superblock_durable(next, DurableWriteKind::AllocationSuperblock)
        {
            self.poisoned = true;
            return Err(error);
        }

        self.active = next;
        let page_id = page.page_id;
        self.cache.insert(page);
        Ok(page_id)
    }

    fn commit_recycled_page(&mut self, page: Page) -> Result<u64> {
        self.ensure_usable()?;
        page.validate()?;
        self.validate_committed_page_id(page.page_id)?;
        let page_id = page.page_id;
        let offset = page_offset(page_id)?;
        if let Err(error) = self.write_durable_bytes(
            offset,
            &page.bytes,
            DurableWriteKind::RecycledPage(page.kind()),
        ) {
            self.poisoned = true;
            return Err(error);
        }
        self.cache.insert(page);
        Ok(page_id)
    }

    fn write_superblock_durable(
        &mut self,
        superblock: Superblock,
        kind: DurableWriteKind,
    ) -> Result<()> {
        let offset = u64::from(superblock.slot)
            .checked_mul(PAGE_SIZE_U64)
            .ok_or_else(|| corruption(0, "superblock offset overflowed u64"))?;
        self.write_durable_bytes(offset, &superblock.encode(), kind)
    }

    fn write_durable_bytes(
        &mut self,
        offset: u64,
        bytes: &[u8],
        kind: DurableWriteKind,
    ) -> Result<()> {
        #[cfg(test)]
        {
            let event_index = self.fault_trace.len();
            self.fault_trace.push(kind);
            if let Some(spec) = self.fault_spec {
                if spec.event_index == event_index {
                    match spec.mode {
                        FaultMode::BeforeWrite => {
                            return Err(injected_fault(kind, spec.mode));
                        }
                        FaultMode::TornWrite => {
                            let prefix = (bytes.len() / 2).max(1);
                            self.file.seek(SeekFrom::Start(offset))?;
                            self.file.write_all(&bytes[..prefix])?;
                            self.file.sync_data()?;
                            return Err(injected_fault(kind, spec.mode));
                        }
                        FaultMode::AfterSync => {
                            self.file.seek(SeekFrom::Start(offset))?;
                            self.file.write_all(bytes)?;
                            self.file.sync_data()?;
                            return Err(injected_fault(kind, spec.mode));
                        }
                    }
                }
            }
        }

        self.file.seek(SeekFrom::Start(offset))?;
        self.file.write_all(bytes)?;
        self.file.sync_data()?;
        if matches!(
            kind,
            DurableWriteKind::AppendPage(_) | DurableWriteKind::RecycledPage(_)
        ) {
            self.data_page_bytes_written = self
                .data_page_bytes_written
                .saturating_add(u64::try_from(bytes.len()).unwrap_or(u64::MAX));
        }
        Ok(())
    }

    #[cfg(test)]
    fn begin_fault_trace_for_test(&mut self) {
        self.fault_spec = None;
        self.fault_trace.clear();
    }

    #[cfg(test)]
    fn inject_fault_for_test(&mut self, event_index: usize, mode: FaultMode) {
        self.fault_spec = Some(FaultSpec { event_index, mode });
        self.fault_trace.clear();
    }

    #[cfg(test)]
    fn fault_trace_for_test(&self) -> &[DurableWriteKind] {
        &self.fault_trace
    }

    /// Process-local logical page-access count used by higher-level instrumentation snapshots.
    pub(crate) const fn read_page_calls(&self) -> u64 {
        self.read_page_calls
    }

    /// Process-local successfully synchronized data-page bytes written since this pager was opened.
    pub(crate) const fn data_page_bytes_written(&self) -> u64 {
        self.data_page_bytes_written
    }

    /// Reads and validates one committed data page.
    pub fn read_page(&mut self, page_id: u64) -> Result<Page> {
        self.ensure_usable()?;
        self.validate_committed_page_id(page_id)?;
        self.read_page_calls = self.read_page_calls.saturating_add(1);
        if let Some(page) = self.cache.get(page_id) {
            return Ok(page);
        }

        let offset = page_offset(page_id)?;
        let mut bytes = [0_u8; PAGE_SIZE];
        self.file.seek(SeekFrom::Start(offset))?;
        self.file.read_exact(&mut bytes)?;
        let page = Page::decode(bytes, page_id)?;
        if let Some(link) = page.page_link() {
            if link >= self.active.page_count {
                return Err(corruption(
                    offset + 32,
                    format!(
                        "page {page_id} points to uncommitted linked page {link}; committed page_count is {}",
                        self.active.page_count
                    ),
                ));
            }
        }
        self.cache.insert(page.clone());
        Ok(page)
    }

    /// Atomically installs or clears the committed root pointer using the inactive superblock slot.
    ///
    /// A non-empty root must already be a committed, structurally valid data page.
    pub fn set_root(&mut self, root_page_id: Option<u64>) -> Result<()> {
        self.ensure_usable()?;
        let encoded = match root_page_id {
            Some(page_id) => {
                self.validate_committed_page_id(page_id)?;
                self.read_page(page_id)?;
                page_id
            }
            None => ROOT_NONE,
        };
        if encoded == self.active.root_page_id {
            return Ok(());
        }

        let next = Superblock {
            slot: alternate_slot(self.active.slot),
            generation: self.active.generation.checked_add(1).ok_or_else(|| {
                corruption(0, "superblock generation exhausted before root update")
            })?,
            page_count: self.active.page_count,
            root_page_id: encoded,
        };
        if let Err(error) = self.write_superblock_durable(next, DurableWriteKind::RootSuperblock) {
            self.poisoned = true;
            return Err(error);
        }
        self.active = next;
        Ok(())
    }

    fn validate_committed_page_id(&self, page_id: u64) -> Result<()> {
        if page_id < SUPERBLOCK_COUNT || page_id >= self.active.page_count {
            return Err(BtreeError::InvalidInput(format!(
                "page id {page_id} is outside committed data range {}..{}",
                SUPERBLOCK_COUNT, self.active.page_count
            )));
        }
        Ok(())
    }

    fn ensure_usable(&self) -> Result<()> {
        if self.poisoned {
            Err(BtreeError::Poisoned)
        } else {
            Ok(())
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Superblock {
    slot: u8,
    generation: u64,
    page_count: u64,
    root_page_id: u64,
}

impl Superblock {
    const fn initial(slot: u8) -> Self {
        Self {
            slot,
            generation: 0,
            page_count: SUPERBLOCK_COUNT,
            root_page_id: ROOT_NONE,
        }
    }

    fn encode(self) -> [u8; PAGE_SIZE] {
        let mut bytes = [0_u8; PAGE_SIZE];
        bytes[..8].copy_from_slice(&SUPERBLOCK_MAGIC);
        bytes[8..10].copy_from_slice(&FORMAT_VERSION.to_le_bytes());
        bytes[10..12].copy_from_slice(&(PAGE_SIZE as u16).to_le_bytes());
        bytes[12] = self.slot;
        bytes[16..24].copy_from_slice(&self.generation.to_le_bytes());
        bytes[24..32].copy_from_slice(&self.page_count.to_le_bytes());
        bytes[32..40].copy_from_slice(&self.root_page_id.to_le_bytes());
        bytes[40..44].copy_from_slice(&0_u32.to_le_bytes());
        refresh_checksum(&mut bytes);
        bytes
    }

    fn decode(bytes: &[u8; PAGE_SIZE], expected_slot: u8) -> Result<Self> {
        let base = u64::from(expected_slot) * PAGE_SIZE_U64;
        validate_checksum(bytes, base)?;
        if bytes[..8] != SUPERBLOCK_MAGIC {
            return Err(corruption(base, "superblock magic mismatch"));
        }
        let version = read_u16(&bytes[8..10]);
        if version != FORMAT_VERSION {
            return Err(BtreeError::UnsupportedVersion {
                found: u64::from(version),
                supported: u64::from(FORMAT_VERSION),
            });
        }
        let page_size = read_u16(&bytes[10..12]);
        if usize::from(page_size) != PAGE_SIZE {
            return Err(corruption(
                base + 10,
                format!("encoded page size is {page_size}; expected {PAGE_SIZE}"),
            ));
        }
        if bytes[12] != expected_slot {
            return Err(corruption(
                base + 12,
                format!(
                    "superblock slot id is {}; physical slot is {expected_slot}",
                    bytes[12]
                ),
            ));
        }
        if bytes[13..16].iter().any(|byte| *byte != 0) {
            return Err(corruption(base + 13, "nonzero reserved superblock bytes"));
        }
        let flags = read_u32(&bytes[40..44]);
        if flags != 0 {
            return Err(corruption(
                base + 40,
                format!("unsupported superblock flags {flags:#010x}"),
            ));
        }
        if bytes[44..CHECKSUM_OFFSET].iter().any(|byte| *byte != 0) {
            return Err(corruption(base + 44, "nonzero reserved superblock payload"));
        }

        let generation = read_u64(&bytes[16..24]);
        let page_count = read_u64(&bytes[24..32]);
        let root_page_id = read_u64(&bytes[32..40]);
        if page_count < SUPERBLOCK_COUNT {
            return Err(corruption(
                base + 24,
                format!("page_count {page_count} is smaller than the superblock prefix"),
            ));
        }
        if root_page_id != ROOT_NONE
            && (root_page_id < SUPERBLOCK_COUNT || root_page_id >= page_count)
        {
            return Err(corruption(
                base + 32,
                format!(
                    "root page id {root_page_id} is outside committed data range {}..{page_count}",
                    SUPERBLOCK_COUNT
                ),
            ));
        }

        Ok(Self {
            slot: expected_slot,
            generation,
            page_count,
            root_page_id,
        })
    }
}

#[derive(Debug)]
struct PageCache {
    capacity: usize,
    pages: BTreeMap<u64, Page>,
    order: VecDeque<u64>,
}

impl PageCache {
    fn new(capacity: usize) -> Self {
        Self {
            capacity,
            pages: BTreeMap::new(),
            order: VecDeque::new(),
        }
    }

    fn len(&self) -> usize {
        self.pages.len()
    }

    fn get(&mut self, page_id: u64) -> Option<Page> {
        let page = self.pages.get(&page_id)?.clone();
        self.touch(page_id);
        Some(page)
    }

    fn insert(&mut self, page: Page) {
        let page_id = page.page_id;
        if self.pages.insert(page_id, page).is_some() {
            self.touch(page_id);
            return;
        }
        if self.pages.len() > self.capacity {
            if let Some(evicted) = self.order.pop_front() {
                self.pages.remove(&evicted);
            }
        }
        self.order.push_back(page_id);
    }

    fn touch(&mut self, page_id: u64) {
        if let Some(position) = self
            .order
            .iter()
            .position(|candidate| *candidate == page_id)
        {
            self.order.remove(position);
        }
        self.order.push_back(page_id);
    }
}

fn validate_page_bytes(bytes: &[u8; PAGE_SIZE], expected_page_id: u64) -> Result<PageKind> {
    let base = page_offset(expected_page_id)?;
    validate_checksum(bytes, base)?;
    if bytes[..4] != PAGE_MAGIC {
        return Err(corruption(base, "data-page magic mismatch"));
    }
    if bytes[4] != FORMAT_VERSION as u8 {
        return Err(BtreeError::UnsupportedVersion {
            found: u64::from(bytes[4]),
            supported: u64::from(FORMAT_VERSION),
        });
    }
    let kind = PageKind::decode(bytes[5], base + 5)?;
    let flags = read_u16(&bytes[6..8]);
    if flags != 0 {
        return Err(corruption(
            base + 6,
            format!("unsupported data-page flags {flags:#06x}"),
        ));
    }
    let encoded_page_id = read_u64(&bytes[8..16]);
    if encoded_page_id != expected_page_id {
        return Err(corruption(
            base + 8,
            format!("page header id is {encoded_page_id}; physical page id is {expected_page_id}"),
        ));
    }
    let page_generation = read_u64(&bytes[16..24]);
    if page_generation != 0 {
        return Err(corruption(
            base + 16,
            format!("page generation {page_generation} is reserved in format v1"),
        ));
    }

    let lower = usize::from(read_u16(&bytes[24..26]));
    let upper = usize::from(read_u16(&bytes[26..28]));
    let cell_count = usize::from(read_u16(&bytes[28..30]));
    if read_u16(&bytes[30..32]) != 0 {
        return Err(corruption(base + 30, "nonzero reserved page-header bytes"));
    }
    let expected_lower = DATA_HEADER_LEN
        .checked_add(
            cell_count
                .checked_mul(SLOT_LEN)
                .ok_or_else(|| corruption(base + 28, "slot-array length overflowed usize"))?,
        )
        .ok_or_else(|| corruption(base + 28, "slot-array end overflowed usize"))?;
    if lower != expected_lower {
        return Err(corruption(
            base + 24,
            format!(
                "lower free-space boundary is {lower}; {cell_count} slots require {expected_lower}"
            ),
        ));
    }
    if lower > upper || upper > CHECKSUM_OFFSET {
        return Err(corruption(
            base + 24,
            format!(
                "invalid free-space boundaries lower={lower}, upper={upper}, payload_end={CHECKSUM_OFFSET}"
            ),
        ));
    }
    if bytes[lower..upper].iter().any(|byte| *byte != 0) {
        return Err(corruption(
            base + u64::try_from(lower).unwrap_or(0),
            "nonzero bytes in declared free-space region",
        ));
    }

    let sibling = read_u64(&bytes[32..40]);
    if kind == PageKind::Internal && sibling != ROOT_NONE {
        return Err(corruption(
            base + 32,
            "internal page must not encode a leaf right-sibling pointer",
        ));
    }
    if sibling != ROOT_NONE && sibling < SUPERBLOCK_COUNT {
        return Err(corruption(
            base + 32,
            format!("right sibling {sibling} overlaps mirrored superblocks"),
        ));
    }
    if sibling == expected_page_id {
        return Err(corruption(base + 32, "leaf right sibling points to itself"));
    }

    let mut ranges = Vec::with_capacity(cell_count);
    for index in 0..cell_count {
        let slot = DATA_HEADER_LEN + index * SLOT_LEN;
        let start = usize::from(read_u16(&bytes[slot..slot + 2]));
        let length = usize::from(read_u16(&bytes[slot + 2..slot + 4]));
        if length == 0 {
            return Err(corruption(
                base + u64::try_from(slot + 2).unwrap_or(0),
                format!("slot {index} has zero-length cell"),
            ));
        }
        let end = start.checked_add(length).ok_or_else(|| {
            corruption(
                base + u64::try_from(slot).unwrap_or(0),
                "cell extent overflowed usize",
            )
        })?;
        if start < upper || end > CHECKSUM_OFFSET {
            return Err(corruption(
                base + u64::try_from(slot).unwrap_or(0),
                format!(
                    "slot {index} cell range {start}..{end} lies outside packed payload {upper}..{CHECKSUM_OFFSET}"
                ),
            ));
        }
        ranges.push((start, end));
    }
    ranges.sort_unstable_by_key(|range| range.0);
    let mut expected_start = upper;
    for (start, end) in ranges {
        if start != expected_start {
            return Err(corruption(
                base + u64::try_from(start.min(CHECKSUM_OFFSET)).unwrap_or(0),
                format!(
                    "packed cell payload has a gap or overlap at byte {start}; expected next cell at {expected_start}"
                ),
            ));
        }
        expected_start = end;
    }
    if expected_start != CHECKSUM_OFFSET {
        return Err(corruption(
            base + u64::try_from(expected_start.min(CHECKSUM_OFFSET)).unwrap_or(0),
            format!("packed cell payload ends at {expected_start}; expected {CHECKSUM_OFFSET}"),
        ));
    }

    Ok(kind)
}

fn choose_superblock(first: Result<Superblock>, second: Result<Superblock>) -> Result<Superblock> {
    match (first, second) {
        (Ok(first), Ok(second)) => {
            let generation_gap = first.generation.abs_diff(second.generation);
            if generation_gap > 1 {
                return Err(corruption(
                    0,
                    format!(
                        "mirrored superblock generations differ by {generation_gap}: slot0={}, slot1={}",
                        first.generation, second.generation
                    ),
                ));
            }
            if first.generation == second.generation
                && (first.page_count != second.page_count
                    || first.root_page_id != second.root_page_id)
            {
                return Err(corruption(
                    0,
                    "equal-generation superblocks disagree on committed metadata",
                ));
            }
            if second.generation > first.generation {
                Ok(second)
            } else {
                Ok(first)
            }
        }
        (Ok(valid), Err(_)) | (Err(_), Ok(valid)) => Ok(valid),
        (Err(first_error), Err(second_error)) => Err(corruption(
            0,
            format!(
                "both mirrored superblocks are invalid: slot0=({first_error}); slot1=({second_error})"
            ),
        )),
    }
}

fn read_superblock(file: &mut File, slot: u8) -> Result<Superblock> {
    let offset = u64::from(slot)
        .checked_mul(PAGE_SIZE_U64)
        .ok_or_else(|| corruption(0, "superblock offset overflowed u64"))?;
    let mut bytes = [0_u8; PAGE_SIZE];
    file.seek(SeekFrom::Start(offset))?;
    file.read_exact(&mut bytes)?;
    Superblock::decode(&bytes, slot)
}

fn alternate_slot(slot: u8) -> u8 {
    if slot == 0 {
        1
    } else {
        0
    }
}

fn validate_cache_capacity(cache_capacity: usize) -> Result<()> {
    if cache_capacity == 0 {
        Err(BtreeError::InvalidInput(
            "pager cache capacity must be greater than zero".to_owned(),
        ))
    } else {
        Ok(())
    }
}

fn committed_file_bytes(page_count: u64) -> Result<u64> {
    page_count
        .checked_mul(PAGE_SIZE_U64)
        .ok_or_else(|| corruption(24, "committed file extent overflowed u64"))
}

fn page_offset(page_id: u64) -> Result<u64> {
    page_id
        .checked_mul(PAGE_SIZE_U64)
        .ok_or_else(|| corruption(0, "page offset overflowed u64"))
}

fn refresh_checksum(bytes: &mut [u8; PAGE_SIZE]) {
    let checksum = crc32fast::hash(&bytes[..CHECKSUM_OFFSET]);
    bytes[CHECKSUM_OFFSET..].copy_from_slice(&checksum.to_le_bytes());
}

fn validate_checksum(bytes: &[u8; PAGE_SIZE], base: u64) -> Result<()> {
    let expected = read_u32(&bytes[CHECKSUM_OFFSET..]);
    let actual = crc32fast::hash(&bytes[..CHECKSUM_OFFSET]);
    if expected != actual {
        return Err(corruption(
            base + CHECKSUM_OFFSET as u64,
            format!("page checksum mismatch: expected {expected:08x}, computed {actual:08x}"),
        ));
    }
    Ok(())
}

fn read_u16(bytes: &[u8]) -> u16 {
    u16::from_le_bytes([bytes[0], bytes[1]])
}

fn read_u32(bytes: &[u8]) -> u32 {
    u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

fn read_u64(bytes: &[u8]) -> u64 {
    u64::from_le_bytes([
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
    ])
}

fn corruption(offset: u64, reason: impl Into<String>) -> BtreeError {
    BtreeError::Corruption {
        offset,
        reason: reason.into(),
    }
}

#[cfg(test)]
mod tests {
    use std::fs::{self, OpenOptions};
    use std::io::{Seek, SeekFrom, Write};

    use tempfile::tempdir;

    use super::{BtreeError, PageKind, Pager, PAGE_SIZE, PAGE_SIZE_U64, SUPERBLOCK_COUNT};

    #[test]
    fn slotted_cells_round_trip_through_reopen() {
        let directory = tempdir().expect("create temporary directory");
        let path = directory.path().join("tree.db");
        let mut pager = Pager::create_new(&path, 2).expect("create pager");
        assert_eq!(pager.data_page_count(), 0);
        assert_eq!(pager.root_page_id(), None);

        let mut leaf = pager
            .prepare_new_page(PageKind::Leaf)
            .expect("prepare leaf");
        assert_eq!(leaf.page_id(), SUPERBLOCK_COUNT);
        assert_eq!(leaf.insert_cell(b"\x00first").expect("insert first"), 0);
        assert_eq!(
            leaf.insert_cell(&[0xff, 0x00, 0x7f])
                .expect("insert binary"),
            1
        );
        let page_id = pager.commit_new_page(leaf).expect("commit leaf");
        pager.set_root(Some(page_id)).expect("install root");
        assert_eq!(pager.data_page_count(), 1);
        assert_eq!(pager.root_page_id(), Some(page_id));
        drop(pager);

        let mut reopened = Pager::open(&path, 2).expect("reopen pager");
        assert_eq!(reopened.root_page_id(), Some(page_id));
        let leaf = reopened.read_page(page_id).expect("read committed leaf");
        assert_eq!(leaf.kind(), PageKind::Leaf);
        assert_eq!(leaf.cell_count(), 2);
        assert_eq!(leaf.cell(0).expect("first cell"), b"\x00first");
        assert_eq!(leaf.cell(1).expect("second cell"), &[0xff, 0x00, 0x7f]);
    }

    #[test]
    fn bounded_cache_evicts_old_validated_pages() {
        let directory = tempdir().expect("create temporary directory");
        let path = directory.path().join("cache.db");
        let mut pager = Pager::create_new(&path, 2).expect("create pager");
        let mut ids = Vec::new();
        for byte in [1_u8, 2, 3] {
            let mut page = pager
                .prepare_new_page(PageKind::Leaf)
                .expect("prepare page");
            page.insert_cell(&[byte]).expect("insert cell");
            ids.push(pager.commit_new_page(page).expect("commit page"));
        }
        assert_eq!(pager.cached_pages(), 2);
        pager.read_page(ids[0]).expect("reload evicted page");
        assert_eq!(pager.cached_pages(), 2);
    }

    #[test]
    fn page_checksum_corruption_fails_on_read() {
        let directory = tempdir().expect("create temporary directory");
        let path = directory.path().join("checksum.db");
        let page_id = {
            let mut pager = Pager::create_new(&path, 1).expect("create pager");
            let mut page = pager
                .prepare_new_page(PageKind::Leaf)
                .expect("prepare page");
            page.insert_cell(b"payload").expect("insert cell");
            pager.commit_new_page(page).expect("commit page")
        };

        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(&path)
            .expect("open raw file");
        file.seek(SeekFrom::Start(page_id * PAGE_SIZE_U64 + 100))
            .expect("seek corrupt byte");
        file.write_all(&[0x80]).expect("corrupt page body");
        file.sync_all().expect("sync corruption fixture");
        drop(file);

        let mut pager = Pager::open(&path, 1).expect("metadata remains valid");
        let error = pager
            .read_page(page_id)
            .expect_err("corrupt committed page must fail");
        assert!(matches!(error, BtreeError::Corruption { .. }));
    }

    #[test]
    fn missing_committed_page_bytes_fail_open() {
        let directory = tempdir().expect("create temporary directory");
        let path = directory.path().join("truncated.db");
        {
            let mut pager = Pager::create_new(&path, 1).expect("create pager");
            let page = pager
                .prepare_new_page(PageKind::Leaf)
                .expect("prepare page");
            pager.commit_new_page(page).expect("commit page");
        }
        let bytes = fs::metadata(&path).expect("metadata").len();
        fs::OpenOptions::new()
            .write(true)
            .open(&path)
            .expect("open for truncate")
            .set_len(bytes - 1)
            .expect("truncate committed page");

        let error = Pager::open(&path, 1).expect_err("committed truncation must fail");
        assert!(matches!(error, BtreeError::Corruption { .. }));
    }

    #[test]
    fn trailing_uncommitted_extent_is_recovered() {
        let directory = tempdir().expect("create temporary directory");
        let path = directory.path().join("tail.db");
        drop(Pager::create_new(&path, 1).expect("create pager"));
        let committed_bytes = SUPERBLOCK_COUNT * PAGE_SIZE_U64;

        let mut file = OpenOptions::new()
            .append(true)
            .open(&path)
            .expect("open for interrupted allocation");
        file.write_all(b"partial-new-page")
            .expect("append uncommitted bytes");
        file.sync_all().expect("sync tail fixture");
        drop(file);

        let pager = Pager::open(&path, 1).expect("recover uncommitted tail");
        let recovery = pager
            .recovered_allocation()
            .expect("recovery must be reported");
        assert_eq!(recovery.page_id, SUPERBLOCK_COUNT);
        assert_eq!(recovery.available_bytes, 16);
        assert_eq!(
            fs::metadata(&path).expect("metadata").len(),
            committed_bytes
        );
    }

    #[test]
    fn torn_newer_superblock_rolls_back_uncommitted_page() {
        let directory = tempdir().expect("create temporary directory");
        let path = directory.path().join("mirrored.db");
        {
            let mut pager = Pager::create_new(&path, 1).expect("create pager");
            let mut page = pager
                .prepare_new_page(PageKind::Leaf)
                .expect("prepare page");
            page.insert_cell(b"committed-by-new-slot")
                .expect("insert cell");
            pager.commit_new_page(page).expect("commit page");
            assert_eq!(pager.generation(), 1);
        }

        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(&path)
            .expect("open raw file");
        file.seek(SeekFrom::Start(PAGE_SIZE_U64 + 100))
            .expect("seek newer superblock");
        file.write_all(&[0x55]).expect("tear newer superblock");
        file.sync_all().expect("sync torn metadata fixture");
        drop(file);

        let pager = Pager::open(&path, 1).expect("fall back to older superblock");
        assert_eq!(pager.generation(), 0);
        assert_eq!(pager.data_page_count(), 0);
        let recovery = pager
            .recovered_allocation()
            .expect("orphan page must be discarded");
        assert_eq!(recovery.available_bytes, PAGE_SIZE_U64);
        assert_eq!(
            fs::metadata(&path).expect("metadata after recovery").len(),
            SUPERBLOCK_COUNT * PAGE_SIZE_U64
        );
    }

    #[test]
    fn both_corrupt_superblocks_fail_closed() {
        let directory = tempdir().expect("create temporary directory");
        let path = directory.path().join("no-metadata.db");
        drop(Pager::create_new(&path, 1).expect("create pager"));
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(&path)
            .expect("open raw file");
        for slot in 0..2_u64 {
            file.seek(SeekFrom::Start(slot * PAGE_SIZE_U64 + 64))
                .expect("seek superblock");
            file.write_all(&[0xa5]).expect("corrupt superblock");
        }
        file.sync_all().expect("sync corruption fixture");
        drop(file);

        let error = Pager::open(&path, 1).expect_err("no valid metadata copy must fail");
        assert!(matches!(error, BtreeError::Corruption { .. }));
    }

    #[test]
    fn page_overfill_is_rejected_without_corrupting_existing_cells() {
        let mut page = super::Page::new(SUPERBLOCK_COUNT, PageKind::Leaf).expect("new page");
        page.insert_cell(&vec![0x11; PAGE_SIZE / 2])
            .expect("first large cell fits");
        let before = page.clone();
        let error = page
            .insert_cell(&vec![0x22; PAGE_SIZE / 2])
            .expect_err("second large cell must not fit");
        assert!(matches!(error, BtreeError::InvalidInput(_)));
        assert_eq!(page, before);
        assert_eq!(page.cell_count(), 1);
    }
}
