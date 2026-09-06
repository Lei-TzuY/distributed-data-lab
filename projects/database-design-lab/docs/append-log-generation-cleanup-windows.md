# Windows append-log obsolete-history retirement

`append_log_generation_cleanup_windows_v1` retires permanently obsolete committed append-log history from the strict generation-directory namespace on Windows without claiming durable physical deletion.

```text
db-lab-log-generation-cleanup-windows \
  --directory data/generations
```

The existing Unix `append_log_generation_cleanup_unix_v1` protocol is unchanged. Unix removes obsolete names and synchronizes the parent directory. The Windows companion instead uses the repository's audited no-overwrite `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` primitive to move obsolete bytes into deterministic sibling quarantine names.

## Eligible retained history

With current authoritative generation `N`, the Windows retirement plan includes:

- final `commit-%020d.marker` entries with ids `< N`;
- `generation-%020d.log` entries with ids `< N`;
- `staging-commit-%020d.marker` entries with ids `<= N`.

It deliberately does not touch:

- the authoritative generation or its marker;
- higher uncommitted generation candidates;
- higher staging-marker evidence;
- durable `reserve-%020d.frontier` allocation evidence.

Thus retirement does not choose authority and cannot lower the allocation frontier below current/future retained evidence.

## Durable namespace-retirement order

The command holds the shared generation writer lease for the complete operation.

Before the first move it validates the full source plan as real regular files and validates every deterministic sibling quarantine target as absent. A collision anywhere rejects the entire operation before any namespace mutation.

The successful order is:

1. verify and retain an exact witness of current authority;
2. build and prevalidate all lower-marker, obsolete-staging, and lower-generation moves;
3. re-verify the authority witness;
4. write-through move lower final markers and obsolete staging entries out of the strict generation directory;
5. re-verify the same authority;
6. write-through move lower generation logs out of the strict generation directory;
7. re-verify the same authority and require all planned source names to be absent.

Moving obsolete markers first is safe because the highest retained final marker remains the current authority. A lower generation log temporarily left without its old marker is uncommitted retained residue, never a rollback candidate.

## Quarantine and failure semantics

Quarantine files are deterministic siblings of the generation directory, such as:

```text
.<directory-name>.retired-commit-00000000000000000002.marker
.<directory-name>.retired-staging-commit-00000000000000000002.marker
.<directory-name>.retired-generation-00000000000000000002.log
```

They remain outside `append_log_generation_directory_v3`, so strict authority selection and namespace verification do not inspect them. They are retained physical evidence, not reclaimed disk space.

Moves are no-overwrite. If the Win32 write-through move reports an error and the observed source/target state is no longer exactly `source exists, target absent`, the operation returns `RetirementUncertain`; the operator must preserve both paths and re-verify rather than guessing which namespace transition became durable.

## Boundary

This closes Windows namespace-retirement parity for obsolete committed history. It does not provide durable physical delete semantics for quarantine bytes, does not purge quarantine, and does not constitute physical power-loss validation. Hosted Windows CI validates executable ordering, Unicode path behavior, no-overwrite prevalidation, authority preservation, and future-evidence retention only.
