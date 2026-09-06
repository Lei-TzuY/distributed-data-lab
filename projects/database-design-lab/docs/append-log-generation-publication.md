# Append-log generation marker publication

`db-lab-log-generation-publish` is the standalone writer-side marker-v2 publication primitive for the current `append_log_generation_directory_v3` retained namespace.

```text
db-lab-log-generation-publish \
  --directory data/log-generations \
  --generation 2
```

The standalone publication protocol remains `append_log_generation_marker_publication_unix_v1` and is intentionally available only on Unix targets. The standalone command still fails before publication I/O on Windows because an arbitrary pre-existing clean generation file does not by itself prove that its canonical filename was durably published.

Windows marker authority is instead available only inside the composed generation compact-switch. That path first obtains a durable Windows reservation, constructs and publishes the compact candidate through the audited no-overwrite `MOVEFILE_WRITE_THROUGH` compact-output path, then performs the authority-changing source recheck under the shared writer lease before publishing the final marker through a second write-through move. Its marker protocol is `append_log_generation_marker_publication_windows_v1`.

## Standalone Unix preconditions

The directory must be a real directory using the closed generation-directory namespace. The requested generation id must be greater than zero and newer than every existing committed marker id. Its canonical `generation-%020d.log` must be a real regular file and must pass `LogEngine::verify` as a completely clean append-log v1 image: no recoverable tail and `file_bytes == valid_bytes`.

The publisher derives the marker-v2 `CommittedPrefix` from that exact clean image: byte extent, CRC-32/IEEE, record count, and next sequence. It immediately re-verifies that proof through the shared committed-prefix verifier before publication can continue.

The standalone tool assumes caller serialization and is wrapped by the shared generation writer lease. It actively re-verifies the generation after durability and staging work so mutation or replacement causes publication to fail instead of committing stale proof.

## Unix durability order

The successful standalone Unix path is intentionally ordered:

1. verify the requested generation as a clean append-log image;
2. derive and independently verify its marker-bound committed prefix;
3. `sync_all` the generation file;
4. `sync_all` the generation directory so the generation name precedes marker authority durably;
5. re-verify the exact generation and proof;
6. create-new `staging-commit-%020d.marker` in the same directory, write the 64-byte marker, and `sync_all` that staging file;
7. re-verify the generation again after staging I/O;
8. publish the final `commit-%020d.marker` with a no-overwrite hard link from the synchronized staging inode;
9. `sync_all` the generation directory again;
10. only after that directory barrier succeeds, decode the published marker and re-verify its committed prefix;
11. remove the staging name best-effort.

The final marker is not reported as durably committed until step 9 succeeds. No old generation is deleted by this command.

## Composed Windows authority order

The Windows compact-switch does not expose generic marker publication. Its supported order is deliberately narrower:

1. durably reserve a fresh generation id through `append_log_generation_reservation_windows_v1`;
2. construct a complete compact candidate and verify exact live-state equivalence;
3. synchronize the complete compact staging file and publish the fresh canonical generation name through the audited no-overwrite `MOVEFILE_WRITE_THROUGH` primitive;
4. acquire the shared generation writer lease for the authority-changing critical section;
5. re-verify the old authoritative generation and complete source state so a writer that ran during candidate construction cannot make stale compact state authoritative;
6. require the same-id durable reservation to remain retained and require the candidate to remain exactly equal to the just-published compact image;
7. derive and independently verify the marker-v2 committed-prefix proof;
8. create-new the canonical staging marker, write the marker bytes, and `sync_all` the staging file;
9. re-verify the exact compact candidate again;
10. publish the final marker with the audited no-overwrite `MOVEFILE_WRITE_THROUGH` primitive;
11. decode the retained marker, re-verify the committed prefix and full compact image, then run the shared generation-directory verifier and require the new generation to be authoritative.

If the Win32 move reports failure while the final marker is nevertheless visible, the operation returns `DurabilityUncertain` and preserves old authority/evidence rather than reporting success. If the final name is absent, the failed staging marker is removed best-effort and the operation returns an ordinary I/O failure.

The standalone `db-lab-log-generation-publish` command remains unsupported on Windows. This prevents a caller from promoting an arbitrary clean generation whose canonical-name durability was not established by the composed compact-switch.

## Crash states and staging markers

`staging-commit-%020d.marker` is a protocol-defined, non-authoritative crash residue. The v3 directory reader accepts canonical staging names, reports their generation ids, and never uses their contents for generation selection. Directory v3 additionally accepts zero-byte `reserve-%020d.frontier` allocation evidence; reservations are also non-authoritative and never select authority.

On Unix:

- Before the final hard link, a crash may leave a staging marker. It has no authority; the previously committed generation remains selected.
- After the final hard link but before the directory durability barrier completes, the final marker may be visible while persistence is uncertain. If the barrier returns an error, the publisher returns nonzero with a durability-uncertain error. The operator must preserve the old generation and use recovery/verification before any retry.
- After the final directory barrier succeeds, the final marker is committed-generation evidence under the Unix publication contract.

On Windows, the composed switch uses a write-through move rather than the Unix hard-link-plus-directory-sync sequence. A successful hosted-CI call proves that the audited Win32 API request and repository ordering executed successfully; it is not a physical power-loss experiment. A failed move with a visible final marker remains explicitly ambiguous and is reported as durability-uncertain.

Failure to remove a retained staging name after a successful publication does not revoke final-marker authority. Canonical staging evidence is always non-authoritative.

## No-overwrite and monotonicity

Final markers are never overwritten. The Unix path uses a no-overwrite hard link; the Windows composed path uses `MOVEFILE_WRITE_THROUGH` without `MOVEFILE_REPLACE_EXISTING` or copy fallback. In both cases the target marker must be absent and the generation must be newer than every existing committed marker id.

Higher uncommitted generation logs, staging markers, and reservation files remain non-authoritative. Selection is still governed only by the highest final `commit-%020d.marker`, as defined by `docs/append-log-generation-directory.md`.

## Platform boundary

Unix has a standalone marker publisher plus the composed compact-switch. Windows has durable reservations, write-through compact-output publication, and composed compact-switch marker authority, but intentionally does not expose the standalone publisher.

Hosted Windows CI validates Unicode path handling, no-overwrite Win32 API behavior, the reservation/candidate/lease/marker ordering, and retained-state verification. Neither hosted Windows CI nor hosted Unix CI is a physical power-loss emulator or a universal guarantee for every filesystem/controller stack.

## What this still does not complete

The authoritative compact-switch now has a concrete Unix path and a composed Windows path. The remaining broad lifecycle work is not marker encoding itself:

- Windows-equivalent guarded cleanup of obsolete lower generations and confirmed-abandoned reserved candidates/staging evidence;
- Windows legacy single-file migration/cutover semantics;
- any stronger ownership mechanism that makes deliberate direct raw-path `LogEngine` bypass impossible rather than explicitly unsupported;
- physical power-loss validation if the project wants evidence beyond API ordering and deterministic retained-state fault models.

Therefore the general Phase 1 compaction milestone remains open.
