# Verified operational timing analysis

`db-lab-batch-analyze` is the repository-defined descriptive analysis path for repeated Phase 4 operational timing evidence. It never treats an arbitrary JSON directory as trusted input: every invocation analyzes a private snapshot that must pass the same fail-closed batch archive verifier used by `db-lab-batch-verify`.

## Trust boundary

The analyzer accepts only archive formats that the shared verifier accepts. At present that means v6/v7 and contextual v10/v11. Frozen legacy v8/v9 comparison-failure sidecars are rejected because they do not carry a unique repeated-batch `pair_index` and therefore cannot be joined unambiguously to one failed ledger row.

The source archive must be a real directory rather than a symlink, and the shared verifier must accept the live source before any snapshot is created. This preserves the verifier's exact file-set and regular-file/symlink rules instead of allowing snapshotting to sanitize an otherwise-invalid source archive.

After live-source verification, the analyzer copies every admitted source entry through an opened regular-file handle into a private temporary snapshot, with the same bounded per-file size used for analysis JSON. The shared verifier validates that snapshot and requires its verification summary to match the live source; all descriptive analysis then reads only from those verified snapshot bytes. The snapshot is verified again after parsing.

Before returning a report, the live source must still pass the shared verifier with the same verification summary. The analyzer then enumerates the source and snapshot again and requires the file-name sets, file lengths, and every byte of every file to match. A timing-only change therefore fails even when it leaves repository revision, pair counts, and every field in `VerificationSummary` unchanged. The emitted report records `snapshot_protocol = "copy_verify_compare_v1"` so downstream evidence can identify this boundary explicitly.

This is still repository-side structural/provenance validation. It does not prove that human-supplied host labels are truthful and does not establish CPU affinity, thermal state, turbo policy, background-load control, filesystem/controller/device cache state, or a stable performance host.

## Analysis protocol

The emitted report records:

- `analysis_protocol = "verified_operational_timing_descriptive_v1"`;
- `snapshot_protocol = "copy_verify_compare_v1"`;
- `estimator = "empirical_nearest_rank_p50_p95_v1"`;
- `interpretation_boundary = "descriptive_only; performance claims require externally controlled pinned-host review"`.

No regression threshold is inferred automatically.

For every duration series, the analyzer reports the sample count, minimum, nearest-rank p50, nearest-rank p95, and maximum in nanoseconds. Percentiles are empirical nearest-rank statistics over the retained raw durations; no interpolation, smoothing, or normal-distribution assumption is applied.

## Denominator and failure separation

`batch.json` remains the authoritative denominator. Included, failed, and explicitly excluded pairs are never collapsed into a success-only count.

Timing evidence is divided into three non-overlapping interpretation classes:

1. `primary_complete_pairs`: successful timing samples from pairs whose complete two-repetition counterbalanced report was retained as `included`. These are the only samples placed in the primary success distribution.
2. `retained_failed_pair_evidence.completed_repetitions`: a fully completed first ordered comparison retained when the second repetition later failed. These samples remain useful diagnostic evidence but are not promoted into the primary complete-pair distribution.
3. `retained_failed_pair_evidence.failing_repetition_prefix`: successful operations observed before the failing ordered comparison returned an error, together with `failed_operations`, which summarizes the distinct failed-operation samples. Prefix successes and failed-operation durations stay separate from complete-pair successes.

Factory failures may have no engine-local sidecar because one or both fresh engines never existed. The analyzer does not fabricate timing evidence for those rows.

## Execution-order stratification

Every success and failure section is reported both as a combined summary and stratified by whole-run execution order:

- `left_then_right`;
- `right_then_left`.

The `left` and `right` identities remain stable engine identities regardless of which engine ran first. Keeping the order strata visible prevents an order/cache/scheduler effect from being silently hidden by an aggregate percentile.

## Successful operational samples

Successful REOPEN and synchronous compaction samples are read from the structured `*_samples` arrays. The analyzer also requires the backward-compatible `*_ns` duration projection to have the same length and exactly the same duration at every index. A mismatch is rejected rather than choosing one representation arbitrarily.

Each successful structured sample contributes:

- `duration_ns`;
- whether `measured_step_index` is missing;
- the architecture-specific `work.unit` category.

The analyzer validates that `units_examined` and `bytes_examined` are unsigned integers but does not normalize B+ tree page-access work against LSM record-version work. These architecture-specific units are not interchangeable physical-I/O units.

## Failed operational samples

Failed REOPEN and compaction samples are summarized separately. The report retains:

- duration distribution;
- stable error-class counts;
- work-unit counts when deterministic completed work exists;
- the number of samples whose `work` is intentionally absent;
- the number of samples without a measured step association.

A missing `work` value remains missing. Planned work or partially observed work is never substituted in order to make failed rows look comparable to successful rows.

## Usage

```text
db-lab-batch-analyze \
  --archive-dir evidence/mixed-42-batch \
  --expected-revision 0123456789abcdef0123456789abcdef01234567
```

For a publication-admitted archive, add `--require-publication`. That flag rejects otherwise-valid exploratory v6/v10 evidence before any descriptive statistics are produced.

The output is JSON so a reviewed downstream publication script can archive or transform the versioned descriptive report without reparsing raw evidence ad hoc.

## Immutable analysis bundles

`db-lab-batch-analysis-bundle` turns the descriptive report into a create-new, self-contained artifact rather than leaving provenance to an untracked stdout redirect. The bundle protocol is `verified_operational_timing_analysis_bundle_v1` and format version 1.

Create one from an existing verified archive:

```text
db-lab-batch-analysis-bundle create \
  --archive-dir evidence/mixed-42-batch \
  --bundle-dir evidence/mixed-42-analysis \
  --expected-revision 0123456789abcdef0123456789abcdef01234567
```

The destination must not already exist and must not be the source archive or a path nested inside it. Creation first verifies the live source, copies every admitted archive file byte-for-byte into `evidence/`, verifies the copied archive, runs the normal analyzer against that copied evidence, re-verifies source and copy, and finally requires the two file sets, lengths, and byte streams to still match. Any failure removes the partial bundle.

A successful bundle contains exactly:

- `analysis.json` — the versioned descriptive report recomputed from the bundled evidence;
- `evidence/` — the complete original repeated-batch archive, including its own `index.json` and any contextual failure sidecar;
- `index.json` — bundle format/protocol, analyzer/snapshot protocol, source archive format, repository revision, publication-admission state, and the exact bundled evidence file set.

Re-verify the artifact later with:

```text
db-lab-batch-analysis-bundle verify \
  --bundle-dir evidence/mixed-42-analysis \
  --expected-revision 0123456789abcdef0123456789abcdef01234567
```

Verification is semantic rather than a trust-on-first-use checksum shortcut: it enforces the exact top-level bundle shape, re-runs the shared raw archive verifier on `evidence/`, re-runs `db-lab-batch-analyze` logic against those bytes, and requires the recomputed JSON value to equal `analysis.json`. Publication-admitted state and source archive format must also agree with the bundle index. The bundled raw archive can independently be passed back to `db-lab-batch-verify` or `db-lab-batch-analyze`.

The bundle does not provide cryptographic authorship or turn uncontrolled timing into a benchmark. Its purpose is to keep the reviewed descriptive result and the exact re-analyzable evidence together under one create-new artifact boundary.

## Publication boundary

A repository-generated descriptive report or verified analysis bundle is not itself a publishable performance result. The remaining Phase 4 work is to collect repeated admitted evidence on a named controlled pinned host, review the retained denominator and order-stratified distributions, freeze any additional exclusion/estimator procedure, and only then consider regression thresholds. GitHub-hosted CI remains correctness/build validation and must not be promoted into that baseline.
