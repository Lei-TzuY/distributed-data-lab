# LSM SSTable, manifest, WAL, and L0/L1 compaction v5

This document specifies persistent sorted-table publication plus crash-safe WAL segmentation and
reclamation. Mutations are acknowledged by a checksummed WAL segment before entering memory. A frozen
MemTable is installed as an immutable indexed SSTable through an immutable manifest snapshot and mirrored
`CURRENT`; when that durable watermark reaches the active WAL tail, the manifest can atomically switch
the version set to a new empty WAL and reclaim older segments only after both CURRENT mirrors move.
SSTable v2 embeds a validated Bloom filter. Manifest v3 added each table's level and a
correctness-first overlapping-L0 / single-run-L1 compaction policy. Manifest v4 added an explicit
tombstone-GC watermark so a full-set compaction can discard deletion markers without losing the durable
sequence when the live result contains no SSTable entries. Manifest v5 keeps that GC field and adds a
persistent SSTable-id high watermark so table-less cleanup cannot make a historical or crash-ambiguous
canonical id eligible for reuse.

All integers are unsigned little-endian. The common 4,096-byte key and 1,048,576-byte value limits
remain authoritative. SSTables keep sequence numbers and explicit tombstones so reads can resolve
newer WAL/MemTable state without discarding deletion history prematurely.

## Directory state

A newly created engine contains:

```text
wal-0000000000000001.log
CURRENT
MANIFEST-0000000000000001
```

Published flushes add canonical immutable files:

```text
sst-0000000000000001.sst
MANIFEST-0000000000000002
sst-0000000000000002.sst
MANIFEST-0000000000000003
...
```

`CURRENT` determines the authoritative manifest. That manifest determines the authoritative WAL segment.
Canonically named SSTables, manifests, or WALs not selected by this chain are orphans from interrupted or
superseded publication and are not opened as authoritative state. Their ids still reserve numeric space,
so later allocation never overwrites a file whose durability outcome was ambiguous. On open, every
canonical SSTable filename raises the in-memory allocation floor even when the file is not authoritative;
the next Manifest v5 publication persists that observed high watermark before post-publication cleanup
may remove the orphan name. Unknown names and non-regular entries fail closed.

For compatibility with the earlier WAL/MemTable Phase 3 slice, an existing directory containing
exactly the canonical WAL and no version-set files opens as a legacy layout with durable sequence zero.
It is replayed completely. The first later flush creates the initial manifest/CURRENT state before
installing its SSTable. A partial version-set layout is never treated as legacy: CURRENT without a
manifest, manifests without CURRENT, or SSTables in an otherwise WAL-only directory fail closed.

As with the existing formats, file contents are synchronized but this implementation does not claim a
portable parent-directory fsync protocol. The experimental constitution's directory-entry durability
caveat therefore still applies to sudden system loss immediately after file creation.

## SSTable v1 and v2

Each `sst-%016d.sst` is immutable once created. SSTable v1 remains readable and consists of a 64-byte
header, sorted data records, a complete sorted index, and a 64-byte footer. New files are SSTable v2:
the same 64-byte header is followed by one canonical Bloom section, then data records, index, and footer.
The header's existing `data_offset` identifies the exact end of the Bloom section, so no sidecar file or
new manifest publication object is introduced. The complete SSTable is still written with `create_new`,
`write_all`, and `sync_all` before any manifest may reference it.

The header contains magic `DBLSMSST`, format version (`1` or `2`), table id, entry count,
data/index/footer offsets, reserved zero bytes, and a header CRC-32. Version 1 requires `data_offset = 64`.
Version 2 requires `data_offset > 64` and interprets bytes `[64, data_offset)` as exactly one Bloom
section. The footer must use the same SSTable version as the header. Existing record and index encodings
remain unchanged: each data record contains magic `SSTR`, record version, PUT or DELETE kind, sequence,
bounded key/value lengths, header CRC, key/value bytes, and record CRC; the full index stores each key,
kind, sequence, physical record offset, and its own checksum.

### Bloom section v1

The embedded Bloom filter is deterministic and is built over **every SSTable key**, including keys whose
latest entry is a tombstone. This is required because a false negative for a tombstone could otherwise
resurrect an older value from another table. The current canonical configuration is 10 bits per key
(minimum 64 bits), byte-rounded, with 7 double-hash probes. The hash algorithm id `1` denotes the stable
seeded 64-bit FNV/mixing routine implemented by this repository; Rust's process-dependent
`DefaultHasher` is intentionally not part of the persistent format.

| Offset | Bytes | Meaning | Validation |
| ---: | ---: | --- | --- |
| 0 | 8 | magic `DBLSMBLM` | exact match |
| 8 | 2 | Bloom format version | `1` |
| 10 | 2 | header length | `40` |
| 12 | 1 | hash algorithm id | `1` |
| 13 | 1 | probe count | `7` |
| 14 | 2 | flags | zero |
| 16 | 8 | bit count | exactly `max(keys * 10, 64)`, rounded to 8 bits |
| 24 | 8 | key count | exactly the SSTable entry count |
| 32 | 4 | payload bytes | exactly `bit_count / 8` |
| 36 | 4 | header CRC-32 | bytes 0–35 |
| 40 | variable | packed bit array | exact declared extent |
| tail | 4 | section CRC-32 | header + bit payload |

The SSTable's pre-footer whole-file CRC independently covers this entire Bloom section as well. Any bad
magic/version/parameter, noncanonical extent, key-count disagreement, header/section checksum failure,
or outer SSTable checksum failure is corruption.

Bloom results are never trusted before structural validation. On open, the engine first validates the
full SSTable records/index and then requires **every indexed key** to be Bloom-positive. A false negative
therefore fails closed rather than becoming a missing-key answer. Only after that proof may a point
`GET` skip a table when the key is outside the manifest bounds or the Bloom filter says negative. Range
scans do not consult Bloom filters.

For the frozen configuration, the standard independent-hash approximation
`(1 - exp(-7 / 10))^7` is about 0.82%. The deterministic regression inserts 10,000 fixed binary keys and
queries 50,000 disjoint fixed keys; the committed hash/filter semantics produce exactly 422 false
positives (0.844%) and are additionally gated below 2%. This is a reproducible correctness/configuration
fixture, not a production workload or performance claim.

Opening still validates that every index entry exactly describes the corresponding data record, both
sections have identical strictly increasing keys, header/footer/manifest metadata agree, no entry
sequence exceeds the durable watermark, and the manifest key bounds match the index. The implementation
keeps validated file bytes and the full index resident in memory; that correctness-first choice is not
yet realistic read-amplification evidence.

## Immutable manifest snapshots

A `MANIFEST-%016d` is a complete version-set snapshot, not an append log. Manifest v1 uses its original
64-byte header and remains readable with implicit WAL id 1 / first sequence 1. Manifest v2 introduced an
80-byte header containing manifest id, durable sequence, table count, descriptor-body length, authoritative
WAL id, WAL first sequence, reserved zeros, and header CRC. Manifest v3 keeps that 80-byte header and
changes only the SSTable descriptor body. Manifest v4 also stays 80 bytes: bytes 64–71 are
`tombstone_gc_sequence`, bytes 72–75 are zero, and bytes 76–79 are the header CRC.

Manifest v5 expands the header to 88 bytes while preserving every v4 field in place. Bytes 64–71 remain
`tombstone_gc_sequence`; bytes 72–79 are `table_id_high_watermark`; bytes 80–83 are zero; bytes 84–87
are the header CRC over bytes 0–83. The descriptor body therefore begins at offset 88 for v5 and at its
historical offset for every older version. V1–v3 imply a GC watermark of zero. V1–v4 do not encode an
allocation high watermark: a nonempty legacy manifest derives it from the largest active descriptor id,
while a table-less legacy manifest with durable history conservatively reserves ids through its durable
sequence. This may skip unused numbers, but it cannot reconstruct and therefore must not guess a smaller
historical allocation frontier. Every newly written manifest is v5.

Manifest v1/v2 descriptors have a 40-byte prefix and are interpreted as level 0 for compatibility.
Manifest v3/v4/v5 descriptors use a 48-byte prefix: table id, file bytes, entry count, durable sequence,
a u32 level, a zero u32 reserved field, smallest/largest-key lengths, key bounds, and descriptor CRC. The
implemented policy accepts only levels 0 and 1 and at most one L1 descriptor. Descriptors remain ordered
by strictly increasing table id and durable sequence. With a nonempty table set, the manifest-level
durable sequence must equal the newest descriptor's watermark. A table-less v4/v5 version may retain a
nonzero durable sequence only when `tombstone_gc_sequence == durable_sequence`; this records that a
complete compaction consumed every older physical version instead of pretending absent table bytes cover
live values. The GC watermark may never exceed the durable sequence, and an install may move neither
the durable nor GC watermark backward.

For v5, durable history requires a nonzero `table_id_high_watermark`, and every active descriptor id must
be at or below it. `open` additionally raises the in-memory watermark to the largest canonical SSTable id
observed in the directory, including unreferenced crash orphans. Flush, compaction, or WAL rotation then
carries the maximum forward into the next immutable v5 snapshot. In particular, table-less compaction may
clean up all SSTable filenames only after CURRENT publishes a manifest that remembers their allocation
frontier. No manifest is accepted merely because the WAL could reconstruct its data: a manifest selected
by CURRENT is authoritative persistent state, so checksum, level, descriptor, GC-watermark,
allocation-watermark, or empty-version invariant failure fails closed.

## Level policy and full-set compaction

Every normal MemTable flush is published as level 0. L0 tables may overlap arbitrarily; sequence numbers,
not key-range placement, decide which version wins. When four L0 descriptors are authoritative, the engine
synchronously compacts **all** authoritative SSTables (the existing L1 run, if present, plus every L0)
into at most one new level-1 SSTable. This deliberately simple policy means L1 is a single sorted
non-overlapping run rather than a production size-tiered or multi-run leveled design. A later flush again
creates L0 and may override L1 by sequence; four later L0 tables trigger another full-set rewrite.

Compaction overlays every input key by highest sequence and then drops entries whose newest version is a
tombstone. This is safe only under the currently implemented conjunction of constraints:

1. The compactor reads **every** SSTable named by the authoritative manifest, including the sole L1 run
   and every overlapping L0 table; no older disk run remains outside its input set.
2. The engine is caller-serialized and has no snapshots, transactions, concurrent compactions, or
   historical readers that can still require a deleted version.
3. WAL/MemTable entries not represented by the manifest have sequence numbers above
   `durable_sequence`, so they can override the compacted result but cannot reveal an older value hidden
   below a dropped tombstone.
4. The replacement Manifest v5 sets `tombstone_gc_sequence = durable_sequence` and preserves an
   SSTable-id high watermark at least as large as every active or canonically observed table id. Future
   flushes preserve both monotonic watermarks, and a later full-set compaction may advance them.

These conditions are an implementation proof for this deliberately narrow engine, not permission for a
future partial/multi-level compactor to drop tombstones. Such a compactor must establish its own
bottommost-level, snapshot, and replication/history constraints.

The crash-publication order is:

1. Keep the old manifest/SSTables authoritative while reading all compaction inputs.
2. If any live entries remain, create the replacement L1 SSTable at a fresh canonical id and `sync_all`
   it. If every newest entry is a tombstone, create no empty placeholder SSTable.
3. Create and `sync_all` a new Manifest v5 that references the optional L1 output, the unchanged active
   WAL, and the advanced GC watermark.
4. Publish the new manifest through the next CURRENT generation and `sync_data`.
5. Publish the **same manifest id** through the other CURRENT mirror at generation + 1 and `sync_data`.
6. Only after both mirrors are self-contained on the compacted version may the live engine drop old table
   handles and best-effort remove obsolete canonical SSTables and manifest snapshots.

Therefore interruption before the first CURRENT publication leaves the complete input version authoritative;
between the two CURRENT writes both complete versions still have their physical inputs; after the second
write either valid mirror selects the same compacted manifest, so old files are redundant. Deterministic
tests corrupt the newer CURRENT mirror after obsolete-file cleanup and require the older mirror to reopen
the single L1 run successfully. They also verify physical tombstone elision, reinsertion through a newer
L0 table, and a fully deleted database reopening from a zero-SSTable manifest without resurrecting old
values.

The compaction fault matrix records the four durable publication classes in order: replacement L1
SSTable, Manifest v5, first CURRENT slot, then mirror CURRENT slot. Each class is exercised under a
before-write error, a synchronized torn-output aftermath, and an after-sync reported error. Torn
SSTable/Manifest cases truncate the new immutable file to half its committed extent; torn CURRENT cases
overwrite half of the selected 4 KiB slot and synchronize the damaged bytes. The triggering live handle
is poisoned and must be reopened. Before the first CURRENT becomes fully durable, reopen must select the
complete four-L0 input version; an after-sync error from the first CURRENT and every mirror-stage error
select the complete one-L1 version. Every case rechecks all logical keys plus `verify`, so a structurally
mixed publication cannot pass merely because point reads happen to agree.

The tombstone-aware matrix requires the old four-L0 version to retain its physical delete marker and the
new L1 version to omit it while both return the same logical absence. A second matrix covers all-deleted
compaction, where there is no replacement-SSTable write class: every fault must reopen either four
tombstone L0 tables or a GC-covered table-less v5 manifest. A reported compaction failure occurs before
the later WAL-rotation step, so the authoritative WAL may retain records already covered by the manifest
watermark; replay skips them deterministically.

## Mirrored CURRENT publication

`CURRENT` is exactly 8,192 bytes: two independent 4 KiB slots. Each slot contains magic `DBLSMCUR`,
version 1, its physical slot id, generation, manifest id, reserved zeros, and a slot CRC. Both initial
slots name MANIFEST 1 at generation zero.

For each version-set install, generation increases by one and the inactive slot (`generation % 2`) is
overwritten and synchronized. If one slot is invalid, the other is sufficient. Two valid equal-generation
slots must agree on manifest id; two unequal valid generations must differ by exactly one. The newer
valid slot wins.

## Flush/install ordering

A frozen MemTable is published synchronously in this order:

1. The mutation records that formed the frozen table are already complete and synchronized in the WAL.
2. Create the next immutable SSTable under its final canonical id, write all bytes, and `sync_all`.
3. Create a new immutable manifest snapshot containing all previously published tables plus the new
   descriptor, write all bytes, and `sync_all`.
4. Overwrite the inactive CURRENT slot with generation + 1 and the new manifest id, then `sync_data`.
5. Only after step 4 succeeds does the live engine retire the frozen MemTable and expose the new
   version set as authoritative.

A failure during any flush/install step poisons the live engine because the caller cannot safely infer
which durable writes completed. Reopen is the recovery boundary.

## Legal interrupted-install states

If interruption occurs before CURRENT publication, the older CURRENT/manifest remains authoritative.
A complete or partial new SSTable/manifest is not referenced by that version set. Because the WAL still
retains the complete mutation history, reopen replays every sequence above the older manifest's durable
watermark and reconstructs the missing logical state.

If the new CURRENT slot is torn, its CRC fails and the prior mirrored slot wins; WAL replay again fills
the unpublished suffix. If the new CURRENT slot is fully durable, every SSTable and manifest it names
was synchronized first, so reopen may safely skip WAL sequences through the new durable watermark.

The deterministic recovery tests exercise a damaged latest CURRENT slot, referenced SSTable corruption,
referenced manifest corruption, canonical unreferenced orphans, published SSTable plus mutable WAL tail,
and 1 MiB value flush/reopen. This is a software/process interruption model under successful OS sync
ordering, not a claim about hardware that violates that contract.

## WAL rotation, mirror safety, and reads

Rotation is eligible only when the active WAL contains at least one record and its `next_sequence` is
exactly `durable_sequence + 1`; this proves no unflushed mutable suffix still depends on that segment. The
engine then creates and synchronizes a new empty canonical WAL whose first sequence is
`durable_sequence + 1`, writes/synchronizes a new Manifest v5 naming the new WAL and unchanged SSTable
set, and publishes that manifest through the next CURRENT generation.

At this point the older CURRENT mirror may still select the previous manifest/WAL, so the old WAL is not
yet reclaimable. The engine writes the **same new manifest id** into the other CURRENT slot at the next
generation and synchronizes it. Only after both valid mirrors select the new manifest does it swap/drop
the old WAL handle and best-effort remove every other canonical WAL segment. This ordering is also
Windows-safe because no obsolete WAL is removed while its live file handle is retained.

A crash before the first CURRENT update leaves the old manifest/WAL authoritative and the new files as
canonical orphans. A crash after the first CURRENT update but before mirror completion can select either
version, so both WALs remain present. After mirror completion, either CURRENT slot selects the same new
manifest/WAL and old segments are redundant. A replayed frozen MemTable followed by a newer mutable tail
specifically cannot trigger reclamation until later flushes advance the durable watermark through that
tail.

Point reads search mutable/frozen MemTables first and then authoritative SSTables newest-first. For SSTable
v2, manifest key bounds and the validated Bloom filter can reject a point lookup before index/data record
decoding. Ordered range scans ignore Bloom filters, merge sequence-tagged SSTable state and the active
WAL/MemTable tail, keep the newest version of each key, remove tombstones, and apply the common half-open
bounds/limit. The current L0/L1 compactor removes superseded physical table versions only after
double-mirror publication. It drops logical tombstones only at the documented full-set proof point.

## Amplification instrumentation

`LsmEngine` exposes resettable, process-local raw counters plus exact integer ratio pairs. They are
measurement evidence for this implementation, not device-level telemetry and not benchmark conclusions.
A logical `reopen` on the same engine handle preserves the counters; constructing a new engine handle
starts a new measurement window. Counter saturation cannot change database behavior.

The implemented ratios are intentionally explicit:

- **point read:** SSTables consulted by successful explicit `GET` calls / explicit `GET` calls. MemTable
  hits therefore contribute zero sorted-table consults, while an absent key may consult every current run.
  Internal lookups used to return a previous value from `PUT`/`DELETE` are not counted as user reads.
- **range read:** physical SSTable records decoded inside successful range scans / logical live records
  returned. Multiple physical versions of one logical key therefore remain visible in the numerator.
- **data write:** complete WAL mutation-record bytes + flush SSTable file bytes + compaction-output SSTable
  file bytes / acknowledged logical mutation bytes (`key + PUT value`, or key only for DELETE). Manifest,
  `CURRENT`, filesystem metadata, cache traffic, and device writeback are outside this deliberately narrow
  numerator. `compaction_input_sstable_bytes` is exposed separately as read-side compaction work.
- **sorted-table space:** bytes in SSTables referenced by the authoritative manifest / durable live
  key+value bytes represented by those SSTables. Unflushed WAL/MemTable state is excluded from both sides
  of this sorted-table ratio. A zero denominator is preserved as `0` rather than converted to NaN/infinity.

A hand-computable regression creates four two-entry L0 flushes, verifies that first-compaction input bytes
equal the sum of those flush outputs, and verifies that the surviving L1 file size equals compaction output
bytes. It then layers a two-entry L0 over an eight-entry L1: three point reads require exactly 5 SSTable
consults (`5/3`), while a full range decodes 10 physical versions and returns 9 logical keys (`10/9`).
The same suite checks physical SSTable bytes from directory metadata and exact WAL record framing. A
separate deterministic state machine drives two full-set compaction cycles with overwrites and deletes
against `db-storage-memory`, including range comparisons and reopen after each compacted version.

## Explicitly deferred

Generalized size-based/multi-run levels, snapshot-aware or replication-aware tombstone GC, block/cache
design, common cross-engine amplification counters, and device-level I/O attribution remain separate
evidence milestones. Bloom
filtering is part of SSTable v2; level metadata and full-set L0-to-L1 publication remain readable from
Manifest v3; the GC proof watermark and table-less durable state originated in Manifest v4; persistent
SSTable allocation history across orphan cleanup is Manifest v5. WAL segment rotation/reclamation remains
part of the implemented crash-state protocol. Parent-directory durability
remains subject to the repository-wide portable-fsync caveat.
