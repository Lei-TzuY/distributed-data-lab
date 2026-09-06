# Legacy append-log to generation-directory migration

`db-lab-log-generation-migrate` is the offline Unix migration path from the original one-file append-log v1 layout to the retained `append_log_generation_directory_v3` lifecycle.

```text
db-lab-log-generation-migrate \
  --source data/legacy.db \
  --target-directory data/log-generations
```

The migration protocol is `append_log_legacy_to_generation_migration_unix_v1`.

## Contract

The legacy source is never renamed, truncated, deleted, or opened mutably by migration. The target directory must not already exist. A successful migration creates a fresh generation directory whose generation 1 reproduces the exact live state captured from the retained legacy file and whose final marker is durably published through the normal Unix marker protocol.

This is an **offline cutover tool**. Every raw-path legacy writer must be quiesced for the whole operation. The generation writer lease can coordinate participants that know the new target directory, but it cannot stop an old process that still holds or reopens the legacy file path.

## Snapshot and cutover order

The successful Unix path is intentionally conservative:

1. require the legacy source to be a real regular file and a complete clean append-log v1 image; a recoverable tail must be repaired explicitly before migration;
2. inspect the complete source with live values;
3. copy the exact legacy bytes to a temporary snapshot, synchronize that snapshot, and require the snapshot inspection to equal the initial source inspection;
4. compare the live source and snapshot byte-for-byte before touching the target;
5. require a fresh target path, create the directory, synchronize the new directory, then synchronize its parent so the target directory name is durable before retained generation evidence is built;
6. compact-copy the immutable temporary snapshot into canonical `generation-00000000000000000001.log` inside the target;
7. acquire the target generation writer lease;
8. compare the live legacy source against the snapshot byte-for-byte again and reject drift before any final marker exists;
9. verify that generation 1 exactly reproduces the captured legacy live state;
10. durably publish generation 1 through the existing Unix marker-v2 publication protocol;
11. verify the new generation directory selects generation 1 and that its live state still equals the captured source;
12. compare the retained legacy source against the snapshot byte-for-byte once more before reporting success.

The import is intentionally a compact live-state image rather than a byte-for-byte copy of historical mutation records. Append-log file format v1 and logical state remain unchanged.

## Failure semantics

The source remains the recovery anchor for every failure before a successful return.

- Invalid or recoverable-tail source: target is not created.
- Source changes while the temporary snapshot is captured: target is not created.
- Target already exists: it is never overwritten.
- Failure after target creation but before final marker publication may leave a fresh target containing only non-authoritative candidate evidence. It must not be treated as migrated state.
- Source drift detected after candidate construction but before marker publication leaves generation 1 without a final marker. The legacy source remains authoritative.
- If the source changes after final marker publication, migration returns an explicit `SourceChangedAfterPublication` error. The target contains a committed snapshot, but the cutover is not proven; preserve both sides and reconcile explicitly rather than guessing which side should win.

A successful return does not delete the legacy source. Retaining it is deliberate rollback evidence and avoids making migration itself depend on a second destructive durability protocol.

## After migration

Do not begin routed mutations yet if the deployment wants the repository-enforced Unix legacy-path handoff. While generation 1 is still the untouched imported image, run `db-lab-log-generation-cutover` as documented in `docs/append-log-legacy-cutover.md`.

That explicit second step retains the old append-log inode under a no-overwrite sibling name and atomically replaces the former legacy pathname with a synchronized non-log sentinel. New `LogEngine::open` calls at the old pathname then fail rather than silently continuing the retired database. A preexisting raw file descriptor is isolated onto the retained inode and cannot mutate the generation target.

After successful cutover, configure the application to use `GenerationLogEngine` on the new generation directory. Normal routed mutations then participate in the shared writer lease, no-rollback authority selection, durable reservation, compact-switch, and cleanup protocols.

Migration remains deliberately non-destructive on its own. Deployments that do not run the explicit cutover retain the previous boundary: the old file remains a valid append log and code that deliberately bypasses the generation-aware path can keep mutating it.

## Platform boundary

Migration and explicit pathname cutover are Unix-only because successful retained-entry creation/replacement depends on parent-directory durability barriers and generation 1 uses the Unix durable marker publisher. Non-Unix targets fail before filesystem access.

Windows CI therefore validates fail-before-filesystem behavior rather than pretending a Windows migration or cutover durability guarantee exists.

## Remaining boundary

The migration-plus-cutover sequence closes normal legacy-path ownership handoff on Unix: new opens at the former legacy pathname are rejected and preexisting descriptors are isolated from generation authority. It does not sandbox a process with arbitrary filesystem access from deliberately opening the retained backup or canonical generation files directly.

The broad Phase 1 compaction milestone remains open primarily because Windows-equivalent retained-entry durability is still unsupported. Cross-platform completion must not be inferred from the Unix protocol.
