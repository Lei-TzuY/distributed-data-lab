# Repeated batch archive verification

`db-lab-batch-verify` performs fail-closed structural and provenance verification of immutable `db-lab-batch` evidence. It never opens engine state and never rewrites an archive.

## Supported evidence

The verifier strongly accepts the current unambiguous repeated formats:

- v6 — exploratory batch without captured comparison-failure sidecars;
- v7 — `publication_warm_v1` batch without captured comparison-failure sidecars;
- v10 — exploratory batch with pair-indexed comparison-failure sidecars;
- v11 — `publication_warm_v1` batch with pair-indexed comparison-failure sidecars.

Frozen v8/v9 archives are recognized but rejected as strongly unverifiable. Their v1 sidecar records pair order and repetition but not the unique repeated-batch `pair_index`; because outer pair order alternates and later repeats, a verifier cannot always join one v8/v9 sidecar to exactly one `batch.json` row without guessing. Those versions remain historical rather than being silently reinterpreted.

## Verification boundary

A successful verification proves repository-defined internal consistency. The verifier checks:

- `index.json` format, exact file list, execution/attempt protocols, admission protocol, and failure-sidecar protocol;
- the physical directory contains exactly `index.json` plus indexed regular files, with no symlinks, nested directories, or unindexed extras;
- `environment.json` agrees with the index and batch on format version, revision, pair seed/count, protocols, and stable engine layout;
- `trace.json` decodes as a real validated `ExperimentTrace` and equals the trace nested in `batch.json`;
- declared included/failed/excluded counts equal the actual attempt rows and sum exactly to `requested_pairs`;
- pair indices are contiguous and pair order matches the recorded seed/alternation policy;
- each disposition has the correct report/failure/exclusion shape, and included reports retain the expected AB/BA execution order;
- publication v7/v11 archives retain the fixed warm-only admission policy and required host/storage/filesystem/build/analysis/noise metadata;
- v10/v11 sidecars carry unique pair context, join to a failed comparison row, do not duplicate pair indices, preserve repetition/order provenance, and agree with the ledger on stable error class/message.

The verifier deliberately does **not** certify that human-supplied hardware labels are truthful, that CPU affinity/turbo/thermals/background load were controlled, or that timing was collected on a pinned host. Those are experimental-environment responsibilities, not properties that can be inferred from JSON.

## Usage

```text
db-lab-batch-verify \
  --archive-dir evidence/mixed-42-batch \
  --expected-revision 0123456789abcdef0123456789abcdef01234567
```

Add `--require-publication` when an automated publication pipeline must reject otherwise-valid exploratory v6/v10 evidence.

On success the command prints a compact JSON summary with format, revision, disposition counts, publication status, and sidecar count. Any inconsistency returns a non-zero exit status.
