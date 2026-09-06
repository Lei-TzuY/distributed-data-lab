# Windows reserved append-log orphan retirement

`append_log_generation_orphan_retire_windows_v1` removes an explicitly abandoned, durably reserved generation candidate from the authoritative generation-directory namespace without pretending that Windows durable deletion has been solved.

The companion command is:

```text
db-lab-log-generation-orphan-retire-windows \
  --directory data/generations \
  --generation 7 \
  --expected-authority 4 \
  --expected-orphan-bytes <bytes> \
  --expected-orphan-crc32 <crc32> \
  [--expected-staging-bytes <bytes> --expected-staging-crc32 <crc32>] \
  --confirm-generation-builder-stopped
```

Use the existing read-only `db-lab-log-generation-orphan inspect` command first. Retirement requires the exact inspected authority, orphan fingerprint, and optional staging fingerprint plus explicit operator confirmation that the candidate builder has stopped.

## Why retirement is a move, not a delete

The repository currently has one audited Windows retained-entry primitive: a no-overwrite `MoveFileExW` call with `MOVEFILE_WRITE_THROUGH`. It does not yet claim a corresponding durable delete-entry primitive.

Therefore Windows orphan retirement performs a write-through namespace move instead of `remove_file`:

- `generation-%020d.log` moves to a deterministic sibling quarantine file outside the strict generation directory;
- an optional `staging-commit-%020d.marker` moves to a deterministic sibling quarantine file first;
- `reserve-%020d.frontier` is never moved or deleted;
- no final commit marker is created or changed.

The quarantine names are siblings of the generation directory and include the generation-directory basename and retired generation id. They are not part of `append_log_generation_directory_v3`, so normal authority selection and namespace verification no longer see the retired candidate or staging marker.

The retained quarantine bytes are deliberate evidence. Physical disk-space reclamation remains a separate maintenance problem until the project has a defensible Windows deletion-durability contract.

## Ordering and failure semantics

The successful Windows path:

1. acquires the shared generation writer lease;
2. re-runs the existing reserved-orphan inspection under that lease;
3. requires exact authority and fingerprint equality with the operator-supplied inspection;
4. requires every deterministic quarantine target to be absent;
5. write-through moves optional staging evidence out of the generation namespace;
6. re-verifies authority and the orphan candidate fingerprint;
7. write-through moves the abandoned generation candidate out of the generation namespace;
8. re-verifies quarantine fingerprints, source-name absence, current authority, durable reservation retention, and allocation-frontier retention.

Quarantine publication is no-overwrite. Existing quarantine evidence blocks retirement rather than being replaced.

If Win32 reports a write-through move failure and the source/target state is no longer the simple `source exists, target absent` case, the operation returns `RetirementUncertain`. The operator must preserve both paths and re-inspect before retrying. The implementation never guesses which namespace transition became durable.

## Safety boundary

This remains guarded operator cleanup. The code does not infer candidate-builder liveness from PID, timestamps, age, or marker absence. `--confirm-generation-builder-stopped` is load-bearing.

The shared writer lease excludes cooperating generation-aware writers while retirement validates and moves retained evidence. Direct raw-path filesystem mutation remains outside that cooperative contract.

Hosted Windows CI validates API ordering, Unicode paths, no-overwrite behavior, retained bytes, authority preservation, and reservation/frontier preservation. It is not physical power-loss testing.

## Relationship to Unix orphan retirement

The existing `append_log_generation_orphan_retire_unix_v2` protocol is unchanged and continues to remove the abandoned staging/candidate names followed by parent-directory synchronization.

The Windows companion intentionally uses a different protocol name and retained-byte outcome because the project has not established equivalent durable deletion semantics. Both protocols preserve the same logical invariants: current authority does not change, the reserved id can never be reused, and the abandoned candidate no longer appears as a generation-directory candidate.
