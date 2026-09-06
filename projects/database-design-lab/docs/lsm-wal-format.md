# LSM WAL and MemTable foundation v1

This document specifies the WAL and in-memory half of Phase 3. The WAL byte format remains version 1,
but WALs are canonical numbered segments selected by the manifest version set. Indexed SSTables,
immutable manifests, mirrored `CURRENT`, flush/WAL publication, SSTable v2 Bloom filters, and the
Manifest-v5 L0/L1 compaction, tombstone-GC, and SSTable-allocation protocol is specified in
`docs/lsm-sstable-manifest-format.md`. The WAL
record bytes do not change for compaction. The engine remains correctness evidence, not yet a candidate
for B+ tree versus LSM performance claims.

All encoded integers are unsigned little-endian. Keys and values follow the common 4,096-byte and
1,048,576-byte limits. Empty keys and PUT values are valid; a DELETE is distinguished by record kind,
not by an empty value.

## Directory layout

WAL files use the canonical name `wal-%016d.log`. A new engine starts with WAL id 1 and first sequence
1; later rotations allocate ids above every canonical WAL id observed in the directory, including
unreferenced crash orphans. Manifest v2 through v5 identify exactly one authoritative WAL by id and
first sequence. Manifest v5 independently persists the SSTable allocation high watermark; it does not
change WAL ids or WAL record bytes.
Canonical non-authoritative WALs may exist after an interrupted rotation and are ignored for replay while
still reserving their numeric ids.

New engines also contain `CURRENT` and an immutable manifest snapshot; published flushes add canonical
SSTable/manifest files as specified in `docs/lsm-sstable-manifest-format.md`. An exact legacy WAL-only
directory containing only `wal-0000000000000001.log` remains readable with implicit WAL id/first sequence
1. Partial version-set layouts do not receive that compatibility treatment. Unknown entries and
non-regular files still fail closed. `create_new` reserves the directory name and rejects every existing
path. File contents are synchronized, but there is still no portable parent-directory fsync claim.

## WAL header

The active WAL begins with a 40-byte header. The CRC is IEEE CRC-32.

| Offset | Bytes | Meaning | v1 validation |
| ---: | ---: | --- | --- |
| 0 | 8 | magic `DBLSMWAL` | exact match |
| 8 | 2 | format version | `1` |
| 10 | 2 | header length | `40` |
| 12 | 4 | flags | zero |
| 16 | 8 | WAL id | nonzero and exactly the manifest-selected/canonical filename id |
| 24 | 8 | first sequence | nonzero and exactly the manifest-selected first sequence |
| 32 | 4 | reserved | zero |
| 36 | 4 | header CRC-32 | checksum of bytes 0–35 |

Unknown versions, flags, ids, nonzero reserved fields, bad checksums, and truncated headers are rejected.

## Mutation record

Each complete mutation consists of a 32-byte header followed immediately by `key || value`. DELETE has
no value bytes. Sequence numbers are globally contiguous within a segment beginning at the first sequence
declared by its validated WAL header/manifest binding.

| Offset | Bytes | Meaning | v1 validation |
| ---: | ---: | --- | --- |
| 0 | 4 | magic `LSMR` | exact match |
| 4 | 1 | record version | `1` |
| 5 | 1 | kind | `1` PUT, `2` DELETE |
| 6 | 2 | flags | zero |
| 8 | 8 | sequence | exactly the next expected sequence |
| 16 | 4 | key length | at most 4,096 |
| 20 | 4 | value length | at most 1,048,576; zero for DELETE |
| 24 | 4 | header CRC-32 | checksum of bytes 0–23 |
| 28 | 4 | record CRC-32 | checksum of bytes 0–27 plus key/value payload |
| 32 | variable | key then value | exact validated lengths |

Decoding validates the fixed header and bounded lengths before allocating the payload. Every addition
used to derive payload size or physical record end uses checked arithmetic. A complete checksum failure,
unknown kind/version/flag, impossible length, or sequence discontinuity is corruption even at EOF.

## Commit and recovery rule

A mutation is acknowledged in this order:

1. validate the common key/value bounds;
2. encode the next contiguous WAL record;
3. seek to WAL EOF, `write_all` the record, and call `sync_data`;
4. apply the same sequence/key/value or tombstone to the mutable MemTable; and
5. return the previous logical value.

An I/O error makes the outcome ambiguous and poisons the live engine until reopen. A complete record
may replay even if its caller observed an error; a partial final record does not replay.

Read-only `verify` reports but does not repair a final incomplete record. Mutating `open` truncates back
to the previous complete record boundary and calls `sync_all` only when the bytes present are a canonical
prefix of the next expected record: magic, version, kind, flags, sequence, any available bounded lengths,
and a complete available header checksum must all agree. Once a complete header is present, it also
reports the required full record size. Unrecognized trailing bytes and complete checksum failures fail
closed rather than being discarded.

The deterministic tests cut the final record at magic, version, kind, flags, sequence, length, checksum,
and payload boundaries; they also exercise bit flips, absurd declared lengths, unexplained tails, and
sequence gaps. This is a process-interruption byte-prefix model under the operating system's sync
contract, not evidence about devices or controller caches that violate successful sync ordering.

## MemTable state

Each MemTable is an ordered map from binary key to `(sequence, value-or-tombstone)`. Point reads search
the mutable table and then immutable tables newest-first. Ordered scans merge the sequence-tagged table
contents into the newest visible version of every key, remove tombstones, and return the common bounded
half-open bytewise range.

After each replayed or live mutation, a mutable table whose deterministic resident estimate reaches
64 KiB is frozen into the immutable list and replaced by an empty mutable table. The estimate is
`24 + key length + value length` bytes per latest entry in that table; replacing a key subtracts its old
estimate first. This is a structure threshold, not a measured allocator byte count or a published memory
amplification metric. Replaying the ordered WAL with the same fixed threshold reconstructs the same
boundaries.

Immutable here means no later mutation changes that in-memory table. Frozen tables are flushed
synchronously and retired only after SSTable + manifest + CURRENT publication succeeds. If a newer
mutable suffix still exists in the active WAL, that WAL remains authoritative and cannot be reclaimed.
Only when the manifest durable watermark reaches the segment tail may rotation publish a new empty WAL
at `durable_sequence + 1`; reclamation then waits until both CURRENT mirrors name the new manifest. There
is no background worker or concurrent mutation contract; one caller and one process must serialize access.

## Explicitly deferred

The following still require focused design plus executable evidence: block/cache layout, levels and
overlap policy, tombstone dropping rules, compaction and obsolete-SSTable/manifest deletion,
compaction fault injection, read/write/space amplification instrumentation, and performance comparisons.
SSTable/Manifest v2/CURRENT bytes, flush crash states, and WAL rotation/reclamation are specified in
`docs/lsm-sstable-manifest-format.md`.
