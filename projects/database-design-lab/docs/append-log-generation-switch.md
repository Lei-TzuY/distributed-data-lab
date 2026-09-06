# Append-log generation-switch recovery law

This document freezes the recovery law for authoritative append-log compaction switching. The executable oracle defines which generation recovery may select after interruption. Concrete retained bytes are defined by `docs/append-log-generation-directory.md`; marker publication by `docs/append-log-generation-publication.md`; durable allocation reservations by `docs/append-log-generation-reservations.md`; the composed offline switch by `docs/append-log-offline-compact-switch.md`; routed mutation and writer exclusion by `docs/append-log-generation-routing.md`; cleanup by `docs/append-log-generation-cleanup.md`; and legacy import by `docs/append-log-legacy-migration.md`.

The executable oracle lives in `crates/db-storage-log/tests/generation_switch_model.rs`.

## Scope and terminology

A **generation** is a candidate append-log v1 file plus commit metadata that can make that generation authoritative. Generation ids are monotonically increasing logical identifiers. The legacy one-file append-log API remains available, while the generation-aware wrapper owns directory-level authority selection.

The model classifies a generation log as one of:

- `clean`: complete append-log v1 evidence accepted by read-only verification;
- `recoverable_tail`: the canonical incomplete-final-append case, which mutable open may repair only when commit metadata proves that the complete compacted base prefix precedes that tail;
- `missing`;
- `corrupt`: anything else rejected by v1 verification.

A generation is **committed** only when its final marker is authoritative under the publication/recovery contract. A higher generation file without a final marker is an orphan candidate, never inferred authority. A durable `reserve-%020d.frontier` records only that an id was allocated; reservations never select authority.

## Authoritative selection rule

Recovery MUST select the highest generation id with a final `commit-%020d.marker`. Directory enumeration order is irrelevant. Generation logs, staging markers, and reservation files never become authoritative without a final marker.

For that selected generation:

- `clean` -> open it directly;
- `recoverable_tail` with a marker-bound complete base prefix -> remain authoritative and delegate only the later final-append repair to append-log v1;
- `recoverable_tail` without proof of the committed base prefix -> fail closed;
- `missing` -> fail closed;
- `corrupt` -> fail closed.

Recovery MUST NOT fall back to a lower committed generation when the highest committed generation is missing or corrupt. A committed generation may already have accepted later synchronized mutations; fallback could silently acknowledge less state than was previously durable.

## Uncommitted generations and reservations

Higher ids without final markers never override the last committed generation, whether their candidate files are absent, incomplete, corrupt, or fully valid. They remain crash orphans until guarded cleanup proves they may be removed.

Directory v3 adds durable zero-byte reservations. A reservation contributes to the monotonic allocation frontier but not to authority. Once reservation N is durably retained, guarded cleanup may remove a proven-abandoned candidate/staging artifact for N without making N reusable.

## Required writer order

Every generation switch must preserve this authority order:

1. retain the old committed generation;
2. durably reserve a never-reused generation id;
3. construct that generation without making it authoritative;
4. make the complete candidate image and canonical candidate name durable under the platform contract;
5. verify the durable candidate image;
6. capture the exact verified base-prefix byte length, CRC-32, record count, and next sequence;
7. re-check old authority and source state while holding the cooperative writer lease for the authority-changing critical section;
8. durably publish the v2 final marker binding that generation id and complete base prefix;
9. verify that the shared reader now selects exactly the new generation and that the candidate is unchanged;
10. only after final-marker authority is established may old-generation cleanup become eligible.

The Unix switch implements this with file/directory synchronization and no-overwrite hard-link marker publication. The Windows switch implements durable reservation and candidate-name publication using the audited no-replace `MOVEFILE_WRITE_THROUGH` primitive, then privately publishes the final marker with the same audited primitive after the lease-held recheck. The generic standalone marker publisher remains intentionally Unix-only on Windows because arbitrary pre-existing generation-name durability is not proven by that API.

Invalid writer orders include publishing the marker before the candidate name/image has the required platform durability, publishing a marker whose prefix proof does not describe the verified compact image, or deleting/corrupting old authority before the new marker has been established. These states fail closed rather than invoking recovery heuristics.

## Crash-state table

| Crash point | Old generation | New generation | Recovery |
| --- | --- | --- | --- |
| before reservation | committed + clean | absent | old |
| after reservation, before candidate exists | committed + clean | reservation-only | old |
| during candidate construction | committed + clean | reserved + uncommitted + incomplete/corrupt | old |
| after candidate is complete/durably named | committed + clean | reserved + uncommitted + clean | old |
| Unix final marker link before directory barrier | committed + clean | final marker may survive or be lost | old or new; verify retained state |
| Windows write-through final-marker operation reports failure but final name is visible | committed + clean | visibility/durability ambiguous | report durability-uncertain; preserve evidence and verify before retry |
| after final marker is established | committed + clean | committed + clean | new |
| marker exists but base-prefix proof fails | committed + clean | committed + unproven state | fail closed |
| during old cleanup | missing/damaged old | committed + clean new | new |
| after old cleanup | absent old | committed + clean new | new |

A committed new generation with a canonical recoverable final append selects the new generation only when its marker proves the complete compacted base prefix is intact and the incomplete bytes follow that prefix. A committed new generation that is missing or corrupt never causes re-selection of old authority.

## Implemented repository contracts

`append_log_generation_directory_v3` provides strict canonical namespace parsing, marker-v2 decoding, generation/format binding, committed-prefix byte/CRC/record/sequence proof, source-read-only prefix verification, highest-marker/no-rollback selection, and durable reservation-frontier interpretation.

Generation reservation is implemented on Unix and Windows. `append_log_generation_reservation_unix_v1` uses create-new reservation plus file/directory synchronization. `append_log_generation_reservation_windows_v1` uses a synchronized sibling staging file and the audited no-replace `MOVEFILE_WRITE_THROUGH` publication primitive. Both run under the shared writer lease and retain the reservation permanently as allocation-frontier evidence.

`append_log_generation_marker_publication_unix_v1` provides the standalone Unix marker writer: generation-file and parent-directory synchronization, same-directory synced staging marker, no-overwrite hard-link final publication, final directory synchronization, and retained proof re-verification.

`append_log_generation_marker_publication_windows_v1` is deliberately private to the composed Windows compact-switch. It is reached only after the same operation has obtained a durable reservation and successfully published the compact candidate's canonical name through the audited Windows compact-output path. The standalone Windows marker publisher remains unsupported.

`append_log_offline_generation_compact_switch_unix_v2` composes durable reservation, source selection, exact live-state compaction, stale-source detection, lease-held publication, and final authority/image verification. A deterministic fault matrix covers the retained-state boundaries and requires exact old-or-new recovery without claiming physical power-loss emulation.

`append_log_offline_generation_compact_switch_windows_v1` follows the same logical authority law with Windows durability primitives: Windows reservation, write-through compact candidate publication, lease-held old-authority/source recheck, synchronized staging marker, write-through final-marker publication, marker/prefix re-verification, and shared directory verification. Unicode-path integration exercises the actual CLI and Win32 path conversion.

`GenerationLogEngine` adds generation-aware normal mutation routing. A handle adopts a newly published higher final marker before its next operation and refuses marker regression or malformed higher authority rather than continuing on a stale generation.

`append_log_generation_writer_lock_v1` adds cooperative cross-process exclusion. Routed operations acquire the create-new sibling lease around authority refresh and mutation. Compact-switch construction runs outside the lease, then the authority-changing source recheck through publication/final verification runs under the same lease. Guarded stale-lock recovery never infers liveness from PID or age.

`append_log_generation_cleanup_unix_v1` conservatively removes obsolete lower retained history under the writer lease with directory durability barriers and repeated authority verification. `append_log_generation_orphan_retire_unix_v2` uses durable reservation evidence plus exact fingerprints/operator confirmation to retire a proven-abandoned higher candidate/staging artifact without making its id reusable.

`append_log_legacy_to_generation_migration_unix_v1` provides offline import from a clean legacy one-file append log into a fresh generation directory while retaining and byte-rechecking the source. Unix cutover tooling can explicitly redirect the legacy pathname while retaining the old inode for diagnosis/recovery.

The writer lock deliberately lives outside the retained generation namespace, so transient coordination does not alter recovery evidence or generation-id allocation. Deliberate raw-path `LogEngine` users remain outside this cooperative ownership contract.

## Evidence boundary

Hosted CI validates exact retained bytes, no-rollback recovery, reservation monotonicity, cooperative exclusion, deterministic composed interruption states, Unix cleanup/orphan/migration/cutover, and audited Windows `MOVEFILE_WRITE_THROUGH` call ordering including Unicode/no-overwrite behavior. It does not emulate sudden power loss, controller caches, every filesystem implementation, or an adversarial process that intentionally bypasses the protocol.

The Unix pre-directory-sync hard-link case explicitly permits old or new depending on which namespace entry survives. The Windows implementation similarly treats a failed write-through final-marker call with a visible final name as durability-uncertain rather than inventing a successful commit.

## What remains before broad Phase 1 compaction completion

The Unix lifecycle is substantially complete. Windows now has durable generation reservations, durable compact-output publication, and an authoritative composed compact-switch with final-marker publication.

The remaining material boundaries are:

- Windows-equivalent guarded cleanup of obsolete lower history and abandoned reserved candidates/staging evidence;
- Windows legacy single-file migration/cutover semantics;
- stronger ownership if the project wants deliberate direct raw-path legacy/generation writes to be impossible rather than explicitly unsupported;
- physical power-loss validation if the project wants evidence stronger than audited API ordering and deterministic retained-state fault models;
- a later crate-layering decision if generation-directory lifecycle support should move below `db-cli` into a dedicated storage-layer API.

The broad roadmap `Compaction` item remains intentionally open until these cross-platform and ownership boundaries are resolved.
