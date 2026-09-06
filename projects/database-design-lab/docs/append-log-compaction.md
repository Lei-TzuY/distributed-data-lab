# Append-log compact-copy publication

`db-lab-log-compact` builds a non-destructive compact copy of an append-log v1 file without changing the source bytes or the append-log file format.

The protocol identifier reported by the command remains `append_log_compact_copy_v1`.

This primitive is also reused by the generation-directory compact-switch implementation. It is intentionally separate from authority selection: producing a complete compact file does not by itself make that file authoritative.

## Command

```text
db-lab-log-compact \
  --source data/history.db \
  --output data/history.compacted.db
```

Both paths are file paths. The source must already be a clean, fully verifiable append-log v1 file. The output and deterministic sibling staging path must not exist.

For an output named `history.compacted.db`, the staging name is:

```text
.history.compacted.db.compacting
```

## State transformation

The command performs a read-only replay of the source with values included, then writes exactly one `put` record for every live key in bytewise key order. Deleted keys, overwritten historical versions, and tombstones are not copied.

The compacted file is still an ordinary append-log v1 file:

- the same 16-byte v1 file header is used;
- compacted records start at sequence 1 and remain contiguous;
- one live key becomes one put record;
- an empty logical database becomes a header-only valid v1 file;
- the result can immediately be opened, appended to, verified, inspected, and reopened by the existing engine.

Sequence numbers are local to the new compacted file. The compact copy preserves current logical state, not historical mutation identity.

## Fail-closed construction

The command performs these common steps before platform-specific publication:

1. require the source to be a real regular file rather than a symlink;
2. require the output and deterministic staging paths not to exist;
3. run read-only `LogEngine::verify` on the source;
4. reject a source with a recoverable incomplete final append instead of silently repairing it;
5. run read-only `LogEngine::inspect(..., true)` and require the verification report to match the initial verification;
6. create the staging file exclusively with `LogEngine::create_new`;
7. append one live-state put per inspected entry through the normal durable engine API;
8. inspect the staging file and require its complete live entries to equal the source live entries exactly, with no recoverable tail and exactly one record per live key;
9. inspect the source again and require it to be unchanged at the semantic/verification level during construction.

The source is never opened through mutable `LogEngine::open`, so compact-copy construction does not perform append-tail repair as a side effect.

## Publication by platform

### Unix and other non-Windows targets

The original publication contract remains unchanged: create the fresh output name with `hard_link(staging, output)`, verify the published image, then remove the extra staging name best-effort.

This is a no-overwrite publication primitive, not by itself a claim that the output directory entry is durably persisted. The Unix generation-marker publisher supplies the later generation-file and parent-directory durability barriers before commit authority can be published.

### Windows

Windows uses the repository's audited `move_no_replace_write_through` primitive instead of hard-link publication:

1. reopen the complete verified staging file and call `sync_all()`;
2. close that handle;
3. move the sibling staging name to the fresh output name using `MoveFileExW` with **only** `MOVEFILE_WRITE_THROUGH`;
4. do not permit replacement and do not permit cross-volume copy/delete fallback;
5. re-open and inspect the canonical output and require it to equal the previously verified staging image.

Because staging and output are siblings, the move is same-volume by construction. Successful Windows publication consumes the staging name rather than leaving a second hard-link name.

If the Win32 move returns an error and the canonical output is already visible, the operation returns `PublicationUncertain` and preserves the visible output as non-authoritative evidence. It does not guess whether the namespace transition was durably completed and does not delete that evidence. If the output is absent, the staging file is cleaned best-effort and the operation fails normally.

Hosted Windows CI executes this API/order contract, including Unicode paths and no-overwrite behavior. It is not physical power-loss testing.

## Crash and failure properties

The source remains authoritative throughout standalone compact-copy publication.

Before publication, interruption can leave only the hidden staging file. The requested output path does not exist, so a partially constructed compact image cannot be confused with successful output.

After successful publication, the canonical output is a complete image that was already verified before its name became visible and is verified again afterward. Unix may temporarily retain both output and staging hard-link names; Windows write-through move leaves only the output name.

A pre-existing output or staging path is never overwritten or silently reclaimed.

The compact-copy primitive detects source changes across its verification/replay boundary, but it is not itself the generation writer-exclusion protocol. Generation-aware maintenance composes it with durable reservations, the shared writer lease, marker publication, and recovery verification.

## Report

Successful stdout is JSON containing:

- `protocol = "append_log_compact_copy_v1"`;
- file format version;
- source byte and record counts;
- live-key count;
- compacted byte and record counts;
- bytes reclaimed in the compact copy;
- `staging_retained`, which can be true on the hard-link publication path if unlinking the extra staging name fails.

`reclaimed_bytes` is descriptive physical-byte reduction between the clean source file and compact copy. It is not a performance claim.

## Relationship to generation compaction

Unix generation compaction now has a retained generation-directory recovery contract, marker-bound prefix proof, durable reservations, cooperative writer exclusion, reservation-before-build compact switching, routed mutation adoption, composed fault tests, obsolete-history cleanup, guarded orphan retirement, and legacy migration/cutover tooling.

Windows now has the audited no-overwrite `MOVEFILE_WRITE_THROUGH` namespace primitive, durable generation reservations, and write-through compact-output publication. These pieces deliberately do not yet make Windows compact-switch authoritative: durable final commit-marker publication and the remaining Windows retained-entry lifecycle operations must be implemented before the broad Phase 1 compaction milestone can be considered complete.
