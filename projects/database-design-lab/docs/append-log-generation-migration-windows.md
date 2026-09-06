# Windows legacy append-log migration

`db-lab-log-generation-migrate` now supports an explicit non-destructive Windows import from one clean legacy append-log v1 file into a fresh generation directory.

The Windows protocol is `append_log_legacy_to_generation_migration_windows_v1`.

## Preconditions

- the legacy source must be a real regular file and a complete clean append-log v1 image;
- raw-path legacy writers must remain quiesced for the complete migration;
- the target generation-directory path must not already exist;
- source and target must remain on the same Windows volume for the audited write-through namespace moves used by this protocol.

A recoverable legacy tail is rejected. Migration never repairs or truncates the source implicitly.

## Retained-state order

The Windows path deliberately reuses the repository's already-audited Windows retained-entry primitives:

1. inspect the legacy source and capture an exact synchronized temporary snapshot;
2. byte-compare the live source with that snapshot;
3. create a fresh sibling staging directory and publish the empty target directory with no-overwrite `MoveFileExW(MOVEFILE_WRITE_THROUGH)`;
4. acquire the generation writer lease for the fresh target;
5. durably publish `reserve-00000000000000000001.frontier` with the existing Windows reservation primitive;
6. compact the immutable snapshot into `generation-00000000000000000001.log`; the Windows compact-copy path synchronizes the complete staging image and publishes the canonical name with the audited no-overwrite write-through move;
7. re-check the live legacy source byte-for-byte against the snapshot;
8. verify the imported generation reproduces the captured live state;
9. publish `commit-00000000000000000001.marker` with the same marker-v2 proof and Windows write-through marker path used by the authoritative compact switch;
10. run the shared generation-directory v3 verifier and require generation 1 plus reservation 1 to be retained;
11. inspect generation 1 again and byte-compare the legacy source with the original snapshot before reporting success.

The legacy source is never mutated or deleted by migration.

## Failure semantics

Before final marker publication, the legacy source remains authoritative. A partially constructed target contains only non-authoritative reservation/candidate evidence and must not be treated as cut over.

If source drift is observed before marker publication, migration stops and no stale marker is created. If source drift is observed after marker publication, both retained states are preserved and the command reports that cutover is not proven; it does not guess which side should win.

If a write-through target-directory or retained-entry transition reports an ambiguous failure after the destination becomes visible, the command fails closed and preserves the visible evidence for explicit verification.

## Ownership boundary

This is migration, not pathname retirement. A successful return proves that the fresh generation directory contains an authoritative snapshot matching the observed legacy bytes, but new callers can still deliberately open the old legacy pathname.

Unix already has a separate explicit pathname-cutover protocol. Windows pathname retirement remains a separate lifecycle step and should use the same conservative retained-evidence approach rather than pretending arbitrary preexisting file handles can be revoked.

Hosted Windows CI validates executable Win32 API ordering, Unicode/no-overwrite behavior, and retained-state invariants. It is not physical power-loss testing.
