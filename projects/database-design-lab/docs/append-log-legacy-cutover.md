# Legacy append-log pathname cutover

`db-lab-log-generation-cutover` is the Unix ownership variant after a successful legacy migration. Migration itself remains non-destructive; cutover retires the old pathname only after the operator has accepted the freshly imported generation directory. Windows uses the companion protocol documented in [`append-log-generation-cutover-windows.md`](append-log-generation-cutover-windows.md).

```text
db-lab-log-generation-cutover \
  --legacy-source data/legacy.db \
  --target-directory data/log-generations
```

The Unix operation protocol is `append_log_legacy_cutover_sentinel_unix_v1`. The pathname sentinel written at the former legacy path carries protocol `append_log_legacy_cutover_sentinel_v1` plus informational target/retained-path fields.

## Preconditions

This is an offline handoff. Raw-path legacy writers must be quiesced and old handles should be closed for the operation.

The target must still be the untouched result of the migration step:

- generation 1 is authoritative;
- its current verification is exactly the marker-bound committed-prefix verification, so no post-migration append has occurred;
- its live-key count and complete live entries equal the clean legacy source.

If the application has already started routing mutations into the generation directory, cutover fails rather than guessing whether the old pathname and target still describe the same migration boundary.

## Unix pathname replacement order

The successful path deliberately avoids any interval in which the legacy pathname is absent:

1. require the legacy pathname to resolve to a clean real append-log file;
2. capture and synchronize an exact temporary byte snapshot;
3. acquire the target generation writer lease and verify the untouched imported generation 1;
4. create-new and synchronize a same-directory cutover-sentinel staging file;
5. compare the live legacy source against the snapshot byte-for-byte;
6. synchronize the legacy file;
7. create a no-overwrite hard link named `<legacy>.retired-append-log-v1` to retain the exact legacy inode;
8. synchronize the legacy parent directory so the retained name is durable;
9. compare the legacy source against the snapshot again;
10. atomically rename the already-synchronized staging sentinel over the original legacy pathname;
11. synchronize the parent directory again before reporting durable cutover success;
12. require the published sentinel bytes to equal the staged bytes, require the retained legacy bytes still to equal the snapshot, and re-verify the generation target under the held writer lease.

The atomic rename in step 10 replaces one existing directory entry with another. The protocol never implements cutover as `rename old away` followed by `create replacement`, so another process does not get an intentional pathname-absent window in which to create a fresh database at the legacy name.

## Existing raw handles

Unix open file descriptors refer to the old inode rather than to the pathname lookup that originally found it. Because the retained hard link exists before pathname replacement, a pre-cutover raw `LogEngine` handle that violates the operational rule and writes after cutover can mutate only the retained legacy inode. It cannot mutate the sentinel now found at the old pathname and it cannot mutate the generation directory.

The integration suite exercises this deliberately: a raw handle is kept open across cutover, then appended after successful cutover. The retained legacy file changes while both the sentinel and authoritative generation bytes remain unchanged.

This isolation is not permission revocation. A process with filesystem access can still deliberately open the retained backup or a canonical generation file by its new/raw path. Normal application code must use `GenerationLogEngine`; the repository does not claim to sandbox a hostile process that can arbitrarily mutate repository files.

## Failure semantics

- Source or target validation failure changes no pathname.
- An existing retained-backup or staging path is never overwritten.
- Source drift before atomic replacement aborts cutover and best-effort removes pre-cutover staging/retained residue; the original legacy pathname remains an append log.
- If the sentinel pathname is visible but the parent-directory durability barrier fails, the operation returns `CutoverDurabilityUncertain`. The retained legacy inode and generation directory must both be preserved while the operator verifies recovery.
- If retained bytes change after pathname replacement but before the operation reports success, cutover returns `RetainedSourceChangedAfterCutover`. The sentinel remains installed and both retained legacy evidence and the generation target must be reconciled explicitly.

No automatic rollback rewrites the legacy pathname after atomic replacement. Once a visible sentinel may have been observed by another process, guessing an automatic reversal would create a new ownership race.

## Platform boundary

The protocol described in this file is Unix-only. It relies on same-filesystem hard-link retention, replacement rename semantics, and parent-directory `sync_all` durability barriers that this repository tests on Linux and macOS.

Windows uses a separate executable contract documented in [`append-log-generation-cutover-windows.md`](append-log-generation-cutover-windows.md): a synchronized retained byte-copy is published with no-overwrite `MoveFileExW(MOVEFILE_WRITE_THROUGH)`, then the legacy pathname is atomically replaced by the sentinel with `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`. The platform variants intentionally do not pretend that Unix hard-link/directory-sync semantics and Win32 write-through namespace semantics are interchangeable.

## Operational sequence

The intended Unix transition is:

1. stop legacy raw-path writers;
2. run `db-lab-log-generation-migrate`;
3. inspect/accept generation 1 while no routed mutations have begun;
4. run `db-lab-log-generation-cutover`;
5. configure the application to use `GenerationLogEngine` on the generation directory;
6. keep the retained legacy inode as rollback/audit evidence until an explicit retention policy permits removal.

Cutover does not delete the retained legacy file and does not alter append-log or generation-marker bytes.
