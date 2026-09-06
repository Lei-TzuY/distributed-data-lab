# Append-log on-disk format v1

All integers are unsigned little-endian. Offsets below are relative to the containing header. Decoders
must use the fixed limits from common KV semantics before allocating any payload.

## File header (16 bytes)

| Offset | Bytes | Meaning | Check |
| ---: | ---: | --- | --- |
| 0 | 8 | magic `DBLABKV\0` | exact match |
| 8 | 2 | file version, currently `1` | unsupported versions rejected |
| 10 | 2 | header length, currently `16` | exact match |
| 12 | 4 | CRC-32 (IEEE) of bytes 0–11 | exact match |

An existing file shorter than 16 bytes, including an existing zero-length file, is corruption. Only a
file newly reserved with create-new semantics receives a fresh header. Initial header bytes are
followed by `sync_all`.

## Mutation record (32-byte header plus payload)

| Offset | Bytes | Meaning | Check |
| ---: | ---: | --- | --- |
| 0 | 4 | magic `KVLG` | exact match |
| 4 | 1 | record version, currently `1` | unsupported versions rejected |
| 5 | 1 | kind: `1` put, `2` delete | all other kinds rejected |
| 6 | 2 | flags | must be zero in v1 |
| 8 | 8 | sequence | begins at 1 and must increase by exactly 1 |
| 16 | 4 | key length | at most 4,096 |
| 20 | 4 | value length | at most 1,048,576; must be zero for delete |
| 24 | 4 | header CRC-32 | CRC of bytes 0–23 |
| 28 | 4 | record CRC-32 | CRC of bytes 0–27, then key bytes, then value bytes |
| 32 | key length | key payload | covered by record CRC |
| ... | value length | value payload; absent for delete | covered by record CRC |

CRC-32 is intentional corruption detection, not authentication and not a proof against malicious
collision construction. Header CRC is validated before declared lengths are used. The decoder then
checks limits, delete invariants, integer conversions, `key + value`, `header + payload`, and
`offset + total` with checked arithmetic. It confirms the declared extent exists before allocating
the bounded payload and validates the full-record CRC before applying the mutation.

Replay uses last complete record wins. A put replaces prior state. A delete removes prior state even
if the key was already missing; the tombstone remains a sequenced record. Sequence gaps, duplicates,
or reorderings fail validation.

## Incomplete final append policy

The scanner remembers the end of the last complete valid record. Bytes after it are recoverable only
when they are a prefix consistent with the next record:

- available magic/version/kind/flags/sequence bytes match what v1 requires;
- any fully available length fields are within limits and obey delete invariants;
- if the header CRC is fully available, it validates; and
- if the complete header is available, its checked declared end lies beyond physical EOF.

Read-only `verify` reports the record offset and available/required bytes and makes no change. Mutable
open/reopen truncates to the last complete boundary and calls `sync_all`. Arbitrary bytes, impossible
lengths, bad available header fields, a complete record with a bad record CRC, or corruption anywhere
before EOF fail closed and are never auto-truncated.

The policy treats one final record as the crash-recovery unit. It does not provide multi-record
transactions.

## Compact copies remain format v1

`db-lab-log-compact` can publish a new, non-destructive compact copy containing exactly one put record
per live key. The compact copy is not a new on-disk format: it starts with the same v1 file header and
its rewritten records start again at sequence 1. Historical record sequence identity is intentionally
not preserved because the output represents current logical state, not mutation history.

The command does not rewrite or replace the source file. It constructs and verifies a sibling staging
file first, then publishes that complete file under a fresh output name with no-overwrite hard-link
semantics. See `docs/append-log-compaction.md` for the publication and crash boundary.

There is still no in-place compaction or authoritative generation-switch protocol in v1. Selecting a
compact copy as the engine's live generation is a separate crash-recovery problem and remains outside
this file format contract.