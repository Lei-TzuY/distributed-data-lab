# Windows legacy append-log pathname cutover

`db-lab-log-generation-cutover` supports a Windows-specific offline pathname retirement after a successful Windows legacy migration.

The Windows protocol is `append_log_legacy_cutover_sentinel_windows_v1`. The sentinel payload continues to use the shared `append_log_legacy_cutover_sentinel_v1` format.

## Preconditions

- the legacy source is still a clean append-log v1 file;
- the generation target is still the untouched Windows migration result: generation 1 is authoritative, reservation 1 and marker 1 are the only retained frontier evidence, and no post-migration routed mutation has occurred;
- all raw-path legacy writers and handles are quiesced and closed for the operation;
- the legacy pathname, retained sibling, and transient staging names reside on the same Windows volume.

The last condition matters because the protocol deliberately does not allow a copy/delete fallback for namespace authority changes.

## Why Windows differs from Unix

The Unix cutover retains the old append-log inode with a hard link before atomically replacing the pathname. The Windows path does not pretend that a newly created hard-link directory entry has the same audited durability contract.

Instead it first creates an exact independent byte copy of the legacy source, synchronizes that copy, and publishes the deterministic sibling retained path with the repository's audited no-overwrite `MoveFileExW(MOVEFILE_WRITE_THROUGH)` primitive. The original legacy pathname remains untouched during this step.

Only after retained rollback evidence exists and the source/target are re-verified does the protocol publish the sentinel with `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`. This gives the Windows path one atomic replacement call instead of a move-away / move-in sequence with a pathname-absent crash gap.

## Retained-state order

1. inspect the clean legacy source and capture a synchronized byte-for-byte snapshot;
2. acquire the generation writer lease and verify the untouched imported generation-1 target;
3. create and synchronize a sibling retained-copy staging file;
4. publish `<legacy>.retired-append-log-v1` with a no-overwrite write-through move, or accept an already-retained file only when it exactly equals the current snapshot;
5. construct and synchronize the JSON cutover sentinel in a unique sibling staging file;
6. byte-compare the live legacy source with the captured snapshot and re-verify the generation target;
7. atomically replace the legacy pathname with the sentinel using `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`;
8. require the pathname bytes to equal the staged sentinel, the retained copy to equal the snapshot, and the generation target to remain exactly unchanged.

After success, new raw `LogEngine::open` calls at the former legacy pathname fail because it contains the non-log sentinel. Generation-aware callers use the migrated target.

## Open-handle boundary

Unlike the Unix test contract, Windows cutover does not promise that an arbitrary preexisting raw file handle can remain open through replacement. File sharing flags can cause Win32 replacement to reject such a cutover. The supported contract therefore requires raw legacy handles to be closed first.

If a violating handle prevents replacement and the legacy pathname still contains the exact source snapshot, the command fails without claiming cutover. If the replacement call reports an error but the pathname no longer clearly contains the original snapshot, the result is reported as uncertain and the retained source plus generation target must be preserved for explicit inspection.

This is intentionally conservative: the protocol never turns Windows sharing-policy uncertainty into a guessed success.

## Retry and rollback evidence

A prior safe failure may leave `<legacy>.retired-append-log-v1`. A retry accepts it only when it is a real regular file whose bytes exactly match the current source snapshot. Different preexisting bytes fail closed and are never overwritten.

The retained copy is rollback evidence, not active authority. Successful cutover never modifies or deletes the generation target.

## Evidence boundary

Hosted Windows CI executes the real Win32 API calls and validates Unicode paths, no-overwrite retained evidence, target immutability, and sentinel replacement. It does not emulate sudden power removal, controller caches, or filesystem behavior outside the documented Win32 write-through contract.
