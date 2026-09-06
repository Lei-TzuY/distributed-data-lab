# Controlled publication sessions

`db-lab-publication-session` binds controlled-host observations to one verified publication-admitted repeated-batch archive without mutating the frozen v7/v11 raw evidence schemas.

The original `controlled_publication_session_v1` / format 1 remains readable as a frozen legacy binding. New creation uses `controlled_publication_session_v2` / format 2, which requires two verified passing `linux_controlled_host_preflight_v1` snapshots: one captured before the publication archive's recorded completion time and one captured at or after it.

A session is an evidence-binding artifact, not a benchmark result, cryptographic signature, proof of operator truthfulness, continuous hardware telemetry, or statistical regression gate.

## Why v2 exists

The repeated-batch archive already has its own immutable versioned contract and `publication_warm_v1` admission. Host-control snapshots have a separate contract because machine-observable affinity/frequency/load controls are different evidence from benchmark traces and outcomes. Silently adding these observations to v7/v11 would redefine frozen formats.

Format 1 bound one passing host snapshot to one publication archive by host label, revision, and source format. That was useful, but it did not prove the snapshot was actually captured before the benchmark. A snapshot taken after collection could still have the same host label.

Format 2 closes that ordering hole by retaining two distinct snapshot files and requiring the archive's own `environment.recorded_unix_seconds` to be enclosed by them:

```text
preflight.recorded_unix_seconds
    <= archive.environment.recorded_unix_seconds
    <= postflight.recorded_unix_seconds
```

This proves temporal enclosure at one-second timestamp resolution. It does **not** prove that every control stayed unchanged continuously between the two observations.

## Session layouts

A current v2 session contains exactly:

```text
session/
  index.json
  host-preflight.json
  host-postflight.json
  evidence/
    index.json
    trace.json
    batch.json
    environment.json
    [comparison-failures.json for v11]
```

Its `index.json` records:

- session format and protocol;
- the required host-control snapshot protocol;
- the required publication admission protocol;
- the exact host label shared by both snapshots and the archive;
- preflight, archive, and postflight recording times;
- repository revision;
- source repeated-archive format version;
- the exact sorted evidence file set.

Only publication repeated formats v7 and v11 are admissible.

A retained v1 session still has exactly `index.json`, `host-preflight.json`, and `evidence/`. The verifier dispatches by `index.json.format_version` and preserves the original v1 validation rules rather than silently upgrading old artifacts.

## Create v2

Capture a passing host-control snapshot before the run, execute the publication batch, then capture another passing snapshot after the batch has produced its archive. The same `db-lab-host-preflight` command is used for both observations; the second retained file is treated as a postflight observation by the session protocol.

```text
db-lab-host-preflight \
  --output evidence/host-preflight-before.json \
  ...controlled-host arguments...

# Run db-lab-batch --admission publication-warm-v1 ...

db-lab-host-preflight \
  --output evidence/host-preflight-after.json \
  ...the same controlled-host policy...

db-lab-publication-session create \
  --host-preflight evidence/host-preflight-before.json \
  --host-postflight evidence/host-preflight-after.json \
  --archive-dir evidence/batch-publication-001 \
  --session-dir evidence/session-001 \
  --expected-revision 0123456789abcdef0123456789abcdef01234567
```

Creation is fail closed:

1. both source snapshots must be distinct regular files and each must pass the shared host-preflight verifier with `require_passed=true`;
2. the source archive must be a real directory and pass the shared batch verifier with publication required;
3. both snapshot host labels must exactly equal `environment.json.publication_admission.host_label`;
4. v2 requires `environment.recorded_unix_seconds > 0`;
5. the timestamp order must satisfy `preflight <= archive <= postflight` before any session directory is created;
6. the destination must not already exist and must not overlap the source archive;
7. both snapshots and every admitted raw archive file are copied with create-new writes into the fresh session;
8. all retained copies are re-verified and the host/time/revision/format binding is checked again;
9. all sources are re-verified after copying and compared byte-for-byte with the retained copies;
10. only then is the v2 session `index.json` written;
11. the completed session is immediately verified through the same public CLI path.

Any failure after destination creation removes the partial session directory. A temporal or host mismatch detected before creation leaves no session directory at all.

## Verify

```text
db-lab-publication-session verify \
  --session-dir evidence/session-001 \
  --expected-revision 0123456789abcdef0123456789abcdef01234567
```

Verification supports both frozen v1 and current v2 sessions.

For v2 it requires the exact four-entry top-level layout, a strict v2 index, two passing retained host-control snapshots, a publication-admitted retained batch archive, the exact indexed evidence file set, the expected repository revision when supplied, the indexed v7/v11 source format, one identical host label across all three evidence sources, exact timestamp equality with the index, and the temporal enclosure rule.

Changing only a snapshot host label, moving a postflight timestamp before the archive, changing the archive's publication host label, changing the indexed timestamps, or altering any inner archive contract invalidates the session.

For v1 the verifier intentionally preserves the original three-entry layout and one-snapshot binding semantics. v1 verification does not pretend that an old artifact had postflight evidence that was never recorded.

## What temporal enclosure means—and does not mean

The archive recording timestamp is produced after the measured repeated batch has completed and before its environment manifest is written. Requiring the preflight timestamp not to exceed that time prevents a post-hoc snapshot from being presented as a pre-run observation. Requiring a passing postflight at or after that time gives a second machine-observable endpoint around the collection.

Those endpoints still do not continuously observe thermals, scheduler interference, storage-controller cache state, frequency transitions, or every possible source of noise during the measured interval. Operator attestations remain statements rather than independently verified facts. A reviewer must still assess the real host, collection procedure, and distributions.

## CI and synthetic publication fixtures

Repository CI exercises v2 create/verify and v1 backward compatibility using verifier-compatible synthetic publication fixtures. CI also tampers host and timing bindings to prove fail-closed behavior. That establishes artifact semantics and portability only.

GitHub-hosted runner timing is never promoted to a performance baseline, and synthetic timing evidence is never publication data.

## Phase 4 boundary

The repository can now preserve and strongly verify raw repeated evidence, analyze it from a byte-stable snapshot, package analysis beside exact raw bytes, collect/re-verify host-control observations, retain old one-snapshot session bindings, and create new temporally enclosed two-snapshot publication sessions without redefining frozen archive formats.

The controlled-host roadmap item remains incomplete until a real named/pinned Linux host is configured and actual controlled measurements are collected there. Reviewed real distributions still come before any regression thresholds or performance gates.
