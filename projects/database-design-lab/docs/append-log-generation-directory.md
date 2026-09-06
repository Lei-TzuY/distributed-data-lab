# Append-log generation directory v3

This document defines the retained generation-directory format used by append-log generation switching. It implements the no-rollback recovery law in `docs/append-log-generation-switch.md`, keeps append-log file format v1 and commit-marker format v2 unchanged, and adds durable generation-id reservations.

The verifier command is:

```text
db-lab-log-generation-verify --directory data/log-generations
```

Its current protocol identifier is `append_log_generation_directory_v3`.

Directory v2 is the frozen predecessor. A v2-shaped retained directory with no reservation files remains readable by the v3 verifier; it simply reports an empty reservation list. The protocol identifier changed because v3 accepts a new canonical retained namespace class and changes the allocation-frontier contract.

## Strict directory namespace

A generation directory is a real directory, not a symlink. Its namespace is closed and permits only canonical names of these forms:

```text
generation-00000000000000000001.log
commit-00000000000000000001.marker
staging-commit-00000000000000000002.marker
reserve-00000000000000000002.frontier
```

Generation ids are decimal nonzero `u64` values encoded as exactly 20 digits. A name that begins with a reserved prefix but is not canonical fails verification. Any unrelated directory entry also fails verification. At most 8,192 entries are scanned.

The four retained classes have distinct meanings:

- `generation-%020d.log`: an append-log v1 generation candidate;
- `commit-%020d.marker`: authoritative commit evidence;
- `staging-commit-%020d.marker`: non-authoritative publication residue;
- `reserve-%020d.frontier`: non-authoritative monotonic allocation evidence.

A generation log without a matching final marker is uncommitted. It may be a complete compact image, an in-progress candidate, or crash residue; the reader records its id but never promotes it merely because its bytes look valid.

A staging marker never carries authority. Its bytes are not decoded for selection. It remains visible only as crash/publication evidence.

A reservation also never carries authority. It must be a real regular file containing exactly zero bytes. Its canonical filename alone records that the generation id has already been allocated and must never be reused. Symlink, non-file, or nonempty reservation evidence fails closed.

## Allocation frontier

`highest_observed_generation` is the maximum id observed across generation logs, final markers, staging markers, and reservation files. `VerifiedGenerationDirectory::next_generation_id()` returns that maximum plus one using checked `u64` arithmetic.

The durable allocation primitive is documented in `docs/append-log-generation-reservations.md`. On Unix it creates and synchronizes a reservation while holding the cooperative generation writer lease before a candidate is constructed.

Reservations separate identity allocation from candidate lifetime. Once reservation N is durable, a future guarded cleanup may remove a confirmed-abandoned generation-N candidate or staging marker without making N reusable.

## Commit marker format v2

All integers are unsigned little-endian. The marker is exactly 64 bytes.

| Offset | Bytes | Meaning | Check |
| ---: | ---: | --- | --- |
| 0 | 8 | magic `DBLGCMT\0` | exact |
| 8 | 2 | marker format version | `2` |
| 10 | 2 | marker header length | `64` |
| 12 | 8 | generation id | nonzero, filename match |
| 20 | 2 | append-log format version | `1` |
| 22 | 2 | reserved | zero |
| 24 | 8 | committed-prefix byte length | at least append-log header |
| 32 | 4 | committed-prefix CRC-32/IEEE | recomputed |
| 36 | 4 | flags | zero |
| 40 | 8 | committed-prefix record count | replay match |
| 48 | 8 | committed-prefix next sequence | replay match |
| 56 | 4 | reserved | zero |
| 60 | 4 | marker CRC-32/IEEE | bytes 0-59 |

CRC is corruption detection, not authentication, a signature, authorship evidence, or malicious-collision protection.

Marker v1 did not bind the verified compacted base prefix and was never durably published by the writer protocol. The current reader intentionally requires marker v2.

## Committed-prefix proof

A committed generation remains appendable after publication, so its marker binds the complete compacted base prefix rather than the file's eventual EOF.

For the highest committed generation the reader:

1. runs read-only `LogEngine::verify` over the current generation file;
2. requires the current source to contain the marker-bound prefix extent;
3. streams exactly that prefix while recomputing CRC-32;
4. materializes the prefix only into a temporary verification file outside the generation directory;
5. runs the canonical append-log v1 verifier on that exact prefix;
6. requires the prefix itself to be complete and clean, with no recoverable tail;
7. requires record count and next sequence to match the marker;
8. requires the current full log to retain at least the complete marker-bound structurally valid prefix.

This lets a later incomplete final append be recoverable only when it begins at or after the proven committed base. A marker that binds an incomplete compact image fails closed even if its checksums match.

Generation-directory verification is read-only. It does not truncate, repair, rename, publish, reserve, or delete retained generation artifacts.

## Authoritative selection

Recovery selects the highest generation id that has a final `commit-%020d.marker`. It never chooses the highest valid-looking log, staging marker, or reservation.

For the selected highest marker the verifier requires:

1. a real regular 64-byte marker that fully validates;
2. the corresponding real regular generation log;
3. matching append-log format version;
4. successful current-log verification;
5. successful independent committed-prefix proof;
6. any recoverable tail to begin at or after the marker-bound prefix.

If the highest final marker is corrupt, its log is missing/corrupt, or its prefix proof fails, verification fails closed. It never falls back to a lower committed generation. Damage to historical lower generations therefore cannot silently roll current authority backward.

## Verification output

Successful JSON includes:

- `protocol = "append_log_generation_directory_v3"`;
- marker format version;
- authoritative generation and canonical log name;
- `highest_observed_generation` across all four namespace classes;
- `marker_generation_ids`;
- `staging_marker_generation_ids`;
- `reservation_generation_ids`;
- uncommitted generation-log ids;
- marker-bound `committed_prefix`;
- independent `committed_prefix_verification`;
- current full `log_verification`.

The staging, reservation, and uncommitted lists are descriptive retained evidence. Reservations affect allocation only; none of these classes changes authority without a final marker.

## Writer-side status

Unix currently provides:

- cooperative cross-process writer exclusion with guarded stale-lock recovery;
- durable marker-v2 publication;
- offline authoritative compact switching;
- generation-aware mutation routing;
- deterministic composed compact-switch fault injection;
- conservative cleanup of obsolete lower generations;
- durable generation-id reservation through `append_log_generation_reservation_unix_v1`.

Windows remains read/verification capable but does not claim equivalent parent-directory durability for marker publication, reservations, or cleanup.

## Remaining lifecycle boundary

The broad Phase 1 compaction milestone remains open. After v3 reservations, the next required integration is to make compact-switch candidate allocation use the durable reservation primitive before construction. Once all new candidates are reserved, guarded cleanup can reclaim confirmed-abandoned higher candidates/staging artifacts while retaining their reservation frontier evidence.

Legacy single-file migration/coexistence and Windows-equivalent durable publication also remain unresolved. Direct raw-path `LogEngine` writers remain outside cooperative generation-directory ownership and must be administratively quiesced during maintenance.
