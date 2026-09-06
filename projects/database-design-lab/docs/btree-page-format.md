# B+ tree page-file format v1

Format v1 uses immutable checksummed 4,096-byte pages plus mirrored metadata. The executable tree
layer now defines binary leaf/internal cells, point lookup, copy-on-write insertion/update/deletion,
root/non-root split propagation, deletion redistribution/merge, root contraction, reachability-derived
page reuse, checksummed overflow chains for keys through 4 KiB and values through 1 MiB, the
common point-operation `KvEngine` contract, bounded half-open ordered range scans, and a deterministic
mutation fault matrix over durable data/metadata write classes. Physical file compaction and exhaustive
device/syscall failure modeling remain deferred. All integers are unsigned little-endian unless stated
otherwise.

## Commit model

Pages 0 and 1 are mirrored superblocks. Each valid superblock contains a monotonically increasing
`generation`, the committed total `page_count`, and an optional root page id. The valid copy with the
highest generation is authoritative. Equal-generation copies must describe identical metadata, and two
valid copies may differ by at most one generation because metadata writes alternate slots.

A new immutable data page is committed in this order:

1. Encode the complete checksummed page at physical page id `page_count`.
2. Append the 4 KiB page and call `sync_data`.
3. Encode `page_count + 1` and `generation + 1` into the inactive superblock slot.
4. Write the complete superblock page and call `sync_data`.
5. Only after both operations succeed does the live pager expose the new metadata generation.

If page append fails or the superblock update has an I/O error, the live pager is poisoned because the
commit outcome is ambiguous. It must be reopened. On reopen, the newest valid superblock decides the
committed extent. Up to one physical page beyond that extent is an interrupted allocation and is
truncated; those bytes were never committed by metadata. More than one trailing page fails closed.
If the selected superblock commits bytes that are physically missing, open fails closed.

## Copy-on-write tree mutation

A successful tree mutation does not modify any page reachable from the currently published root.
`PUT` performs:

1. Searches from the current root to the target leaf.
2. Encodes the new key inline when its descriptor can remain page-local; otherwise commits the key
   overflow chunks tail-to-head. Values use the same durable overflow-chain protocol when needed.
3. Builds a replacement leaf in memory with the key inserted or value replaced. Long exact-minimum
   keys used by internal cells are materialized through the same overflow mechanism before that parent.
4. If a tree page overflows, divides its sorted cells into two independently valid pages.
5. Commits replacement leaf/split pages as immutable allocations.
6. Rebuilds the parent with replacement child references; parent overflow recursively produces two
   replacement internal pages, propagating toward the root.
7. If propagation returns two top-level pages, commits a new two-child internal root.
8. Only after every overflow and replacement page has been synchronized and committed, publishes the
   final root page id with one more mirrored-superblock generation.

`DELETE` follows the same publication boundary. It first confirms the key exists, so deleting a
missing key is a true no-op with no page allocation or metadata generation. The changed leaf is copied
without the target entry. If that child becomes underfull, the parent combines it with an adjacent
sibling: when all encoded cells fit on one page the pair merges; otherwise cells are redistributed by
encoded byte size. Internal redistribution requires at least two children on each resulting sibling.
An empty child disappears from its parent, a one-child top-level internal page contracts to its only
child, and deleting the final leaf entry publishes `root = 0`. Replacement pages are synchronized
before the final root-pointer transition exactly as for `PUT`.

A crash before the final root publication leaves the old root authoritative. Overflow and tree pages
already committed during the earlier steps are
valid but unreachable copy-on-write history. A torn new-root superblock falls back to the prior valid
metadata generation, which still names the old root. A crash after the new root metadata is durable
finds every referenced replacement page already synchronized. This protocol therefore avoids repairing a torn reachable page in place. Committed pages from an
unpublished attempt remain outside the authoritative root and become candidates for the same
reachability-derived reuse mechanism as older COW history.

The deterministic test suite also constructs a committed-but-unpublished shadow page and verifies that
reopen continues to return the value reachable through the older root.

## Deterministic mutation fault matrix

The test-only pager injector records durable write events and can fail one selected event in three
controlled modes: before any bytes are written, after writing and synchronizing half of the 4 KiB image,
or after writing and synchronizing the complete image but before reporting success to the caller. The
last mode models an ambiguous acknowledgement: durable state may have advanced even though the API
returns an I/O error and poisons the live handle. Production builds do not carry the injector state.

| Durable write class | Representative coverage | Reopen invariant after injected error |
| --- | --- | --- |
| Appended data page | overflow, leaf, internal | old logical root remains authoritative; a partial tail is truncated, while a fully committed unpublished page is merely unreachable |
| Allocation superblock | page-count publication | a torn copy falls back to the prior mirror; a fully synchronized copy may commit extra unreachable allocation history, but not a new logical root |
| Recycled data page | overflow, leaf, internal | old root never references the orphan; even a torn recycled image is later proven safe to overwrite completely and reuse |
| Final root superblock | root install or root clear | before/torn write reopens the old tree; only a complete synchronized root image followed by an injected error may reopen the complete new tree |

The matrix runs real file writes, `sync_data`, handle poisoning, drop, and reopen. It also covers deleting
the final key, where the only logical publication is `root = 0`: pre-write/torn metadata faults preserve
the key, while a post-sync reported error reopens the fully empty tree. These tests establish the legal
software fault states around the engine's durable-write protocol; they are not a claim that every
filesystem, controller cache, storage device, or power-loss behavior honors the operating-system sync
contract.

## Reachability-derived page reuse

Before each mutating tree operation, the currently published root is structurally validated while its
reachable page ids are collected. Every committed data-page id outside that set is an orphan from an
earlier successful COW publication (or an unpublished failed/shadow attempt) and is eligible for reuse.
The current root cannot reference such a page. Reachability includes every overflow page referenced by a live leaf key/value or internal exact-minimum
separator and rejects duplicate/cyclic overflow references. Point-tree v1 additionally requires
reachable leaf right-sibling fields to be zero so no hidden tree edge escapes the reachability walk.

A recycled page is rebuilt with the same physical page id, fully overwritten, and `sync_data` is called
before it may appear beneath a newly published root. Recycling does not change `page_count`: the extent
was already committed. If the recycled write tears or the process crashes before root publication, the
old root still does not reference that page, so reopen validates the old tree and the orphan id can be
reused again. If the new root becomes durable, every recycled page it references was synchronized first.
This derives free space from reachability instead of persisting a separate free-list, avoiding a second
metadata structure that would itself require atomic update rules. The physical file does not shrink;
compaction/truncation is a separate future concern.

## Superblocks: pages 0 and 1

The checksum is CRC-32 (IEEE) over bytes 0–4091. Bytes 4092–4095 store the checksum itself.

| Offset | Bytes | Meaning | v1 validation |
| ---: | ---: | --- | --- |
| 0 | 8 | magic `DBBPTRE\0` | exact match |
| 8 | 2 | format version | `1` |
| 10 | 2 | physical page size | `4096` |
| 12 | 1 | superblock slot id | equals physical slot 0 or 1 |
| 13 | 3 | reserved | all zero |
| 16 | 8 | metadata generation | monotonic across committed metadata writes |
| 24 | 8 | total committed page count | at least `2` |
| 32 | 8 | root page id | `0` for none, otherwise `2 <= root < page_count` |
| 40 | 4 | flags | zero |
| 44 | 4048 | reserved | all zero |
| 4092 | 4 | page CRC-32 | exact match |

A single invalid superblock is recoverable when the other copy validates. If neither copy validates,
open fails. The two copies are not a general journal: they protect the small committed metadata state
whose update follows already-synchronized immutable page allocation.

## Data-page header

Data page ids begin at 2. A page checksum again covers bytes 0–4091.

| Offset | Bytes | Meaning | v1 validation |
| ---: | ---: | --- | --- |
| 0 | 4 | magic `BTPG` | exact match |
| 4 | 1 | format version | `1` |
| 5 | 1 | kind | `1` leaf, `2` internal, `3` overflow |
| 6 | 2 | flags | zero |
| 8 | 8 | encoded physical page id | must equal its physical position |
| 16 | 8 | page generation | reserved as zero in v1 |
| 24 | 2 | lower free-space boundary | exactly header + slot-array bytes |
| 26 | 2 | upper free-space boundary | at most byte 4092 |
| 28 | 2 | cell count | defines exactly that many slots |
| 30 | 2 | reserved | zero |
| 32 | 8 | kind-specific page link | leaf: right sibling (point tree requires zero); overflow: next chunk page; internal: zero |
| 40 | variable | four-byte slots | `(u16 offset, u16 length)` |
| ... | variable | zeroed free space | `lower..upper` |
| ... | variable | packed cell payloads | contiguous through byte 4091 |
| 4092 | 4 | page CRC-32 | exact match |

The pager verifies checksum, magic, version, flags, physical page id, reserved fields, free-space
boundaries, slot count, nonzero cell lengths, payload extents, and exact non-overlapping packed payload
coverage before returning a page. Internal pages cannot carry a page link. Any nonzero leaf/overflow
link must avoid the mirrored superblocks and self-reference, and pager reads require the target to be
inside the committed page extent.

## Leaf cells

Reachable leaf cells are sorted in strictly increasing bytewise key order. Empty keys and empty values
are legal. Duplicate keys are not: `PUT` replaces the value while rebuilding the copy-on-write path.

| Cell offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 2 | key descriptor |
| 2 | 4 | value descriptor |
| 6 | variable | inline key bytes or one `u64` key-overflow head |
| ... | variable | inline value bytes or one `u64` value-overflow head |

The key descriptor's high bit is the key-overflow flag and its low 15 bits are the logical key length.
Keys through 4,096 bytes are accepted. Keys up to 4,034 bytes remain inline; the conservative threshold
ensures an inline key still leaves room for a value-overflow descriptor in an otherwise empty leaf.
Longer keys store exactly one `u64` overflow-head page id in the cell. Because 4,096 is far below the
15-bit marker boundary, old inline v1 key encodings remain unambiguous.

The value descriptor's high bit is the overflow flag; the low 31 bits encode the logical value length. When
the high bit is clear, exactly that many value bytes follow the key, preserving compatibility with the
previous inline v1 encoding. When the high bit is set, the logical length must be `1..=1,048,576` and
exactly eight bytes follow the key containing the first overflow page id. Because the common value limit
is only 1 MiB, the marker bit cannot collide with a previously legal inline length. New readers can open
older inline-only v1 files; older binaries intentionally reject kind-3 pages in files that use overflow.

The tree therefore accepts the complete common point size contract: 4 KiB keys and 1 MiB values.

## Overflow key/value pages

An overflow page has kind `3`, exactly one non-empty slotted cell, and uses header bytes 32–39 as an
optional next-page id. The single cell is raw key or value data; with one four-byte slot, canonical payload
capacity is 4,048 bytes. A blob is divided with `rchunks(4048)` and pages are committed tail-to-head,
therefore every nonzero next link names a page that was already synchronized before its predecessor.
The leaf is committed only after the full chain exists, and the new root is published only after the
leaf and all rewritten ancestors are durable.

Reopen derives the exact expected page count from the logical key/value length, requires the first chunk to
hold the remainder and every later chunk to hold exactly 4,048 bytes, requires exactly one cell per
overflow page, rejects short/long chains, kind mismatches, cycles, duplicate references, invalid links,
and total-length mismatches. Overflow pages are added to the same reachable-page set used by COW space
reuse, so a live key or value chain can never be selected as an orphan. A failed/unpublished replacement
chain is unreachable and may be recycled by a later mutation. A 4 KiB key uses at most two overflow
pages; the 1 MiB value maximum uses at most 260.

## Internal cells

Every internal cell describes one child and the exact minimum key reachable in that child. Cells are
sorted by that minimum key. This representation deliberately stores the first child's minimum too, which keeps routing and
structural validation uniform. A long exact minimum uses the same high-bit key descriptor and overflow
blob format as a leaf key; routing compares reconstructed logical key bytes, so separator semantics do
not change when storage moves out of line.

| Cell offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 2 | separator/minimum-key descriptor |
| 2 | 8 | committed child page id |
| 10 | variable | inline exact minimum key or one `u64` key-overflow head |

Lookup chooses the child whose minimum key is the greatest key not greater than the search key; a
search smaller than the first separator descends into the first child and naturally returns missing at
the leaf. Reachable internal pages have at least two children. Reopen recursively verifies that each
stored separator equals the referenced child's actual minimum key, child ranges do not overlap, all
children have equal height, and no page is reached twice or through a cycle. Traversal depth is bounded
to fail closed on malicious/corrupt structures.

## Ordered range scans

`range_scan(start, end, limit)` returns at most `limit` live entries in ascending bytewise key order.
The lower bound is inclusive, the optional upper bound is exclusive, and `end = None` is unbounded.
Equal bounds and a zero limit return an empty result; an upper bound that sorts before the lower bound
is invalid input. Scans are read-only and do not change metadata generation or committed page count.

Format v1 deliberately continues to require zero leaf right-sibling fields. A scan instead descends
through internal children in separator order. For child `i`, the exact minimum of child `i + 1` is an
exclusive upper bound on every key in child `i`; if that next minimum is less than or equal to `start`,
child `i` can be skipped. Traversal stops globally when a child minimum reaches `end`, a leaf key reaches
`end`, or `limit` rows have been emitted. This preserves ordered semantics without introducing sibling
references that copy-on-write mutation and orphan-page reachability would also have to rewrite/track.
Values, including overflow-backed values, are materialized only for rows that survive the range bounds.

## Slotted-page behavior

`Page::insert_cell` stores one non-empty encoded cell. Slots grow upward from byte 40 while payloads
grow downward from byte 4092. An insertion is rejected before mutation when the slot plus payload cannot
fit. The page checksum is refreshed after each successful in-memory edit.

`Pager::prepare_new_page` assigns the next candidate physical id. `Pager::commit_new_page` persists that
page with the allocation ordering above. `Pager::read_page` validates before caching. The cache is
bounded and stores only immutable validated page images, so eviction has no dirty-page writeback
semantics.

## Explicitly deferred

Format/tree work still does not define physical file compaction/truncation, multi-operation
transactions, WAL-based in-place replacement, or concurrent writers. Fault injection for every
intermediate mutation write is also still required
before performance experiments; current evidence covers pager torn/truncated states plus the semantic
unpublished-shadow-root case.
