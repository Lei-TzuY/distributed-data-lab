# Shared Phase 4 experiment traces

Phase 4 compares B+ tree and LSM behavior only after both engines receive the exact same logical input.
`db-core` therefore owns a separate versioned `ExperimentTrace` schema instead of changing the Phase 1
correctness-workload format. Correctness workloads remain focused on PUT/GET/DELETE/REOPEN regression;
experiment traces add ordered range scans and an explicit setup/measurement boundary.

## Trace structure

`ExperimentTrace` v1 contains:

- `profile`: one stable experiment family;
- `seed`: the recorded SplitMix64 seed;
- `generator`: all stable-generator parameters;
- `setup_steps`: deterministic state-building operations excluded from amplification counters;
- `measured_steps`: the exact operations included in the measurement window.

The runner validates every key/value/range against the common contract, executes all setup steps, calls
`reset_amplification()` exactly once, and then executes every measured step in order. `REOPEN` may occur in
the measured window. B+ tree and LSM preserve their process-local measurement window across the common
same-handle reopen operation.

Trace format v1 accepts canonical generated traces only. Validation regenerates the declared seed/config
and requires the setup and measured vectors to match exactly; removing generator metadata or editing steps
without changing the metadata fails closed. A future hand-authored/custom family needs an explicit schema
and profile rather than borrowing a generated profile label.

Resource bounds are part of trace validity. A trace contains at most 1,000,000 total steps, 64 MiB of
combined encoded key/value payload, and range limits no greater than 1,000,000 rows. Generation checks a
conservative profile-specific payload upper bound before allocating step vectors. Runners also cap the
cumulative key/value payload produced by setup or measured outcomes at 64 MiB per phase; only the common
measured vector is retained in a report. These are defensive limits, not recommended experiment sizes.

Generated keys are fixed-width eight-byte **big-endian** unsigned ids. Numeric id order therefore equals
bytewise key order, making generated range boundaries architecture-independent. Values are fixed-size byte
strings filled by the specified SplitMix64 stream. The experiment generator has its own SplitMix64
implementation so future changes to the Phase 1 correctness generator cannot silently change Phase 4 traces.
Committed golden fingerprints cover every profile under one fixed configuration. Changing a generator rule
requires a trace-format revision and continued validation support for archived older traces.

## Stable profiles

### `point_read`

Setup writes every id in `[0, key_space)`. Measured operations are GET only, plus optional REOPEN steps.
Each GET is deterministically selected as 80% hit / 20% miss. Hits read ids in the seeded key space; misses
read ids in `[key_space, 2 * key_space)`. Setup writes are outside the amplification window.

### `range_scan`

Setup writes every id in `[0, key_space)`. Every measured operation selects one start id uniformly from the
key space and requests `[start, min(start + range_limit, key_space))` with `limit = range_limit`. The generator
requires `0 < range_limit <= key_space`.

### `sequential_write`

Setup is empty. Measured operations PUT distinct ids `0, 1, ... operations - 1` in ascending bytewise order.
The generator requires `key_space >= operations`; this prevents the sequential profile from silently turning
into an overwrite workload.

### `random_write`

Setup is empty. Every measured operation PUTs one uniformly selected id from `[0, key_space)`. Repeated ids
are intentional and expose overwrite/update behavior under a stable random stream.

### `mixed`

Setup writes every even id in `[0, key_space)`, leaving odd ids absent so the measured phase contains both
existing and missing keys. Each measured logical operation uses this fixed distribution:

- 40% PUT;
- 30% GET;
- 15% RANGE_SCAN;
- 15% DELETE.

PUT/GET/DELETE ids are uniformly selected from the key space. Range generation matches the `range_scan`
profile. Optional REOPEN steps are inserted after every configured number of measured logical operations and
do not change the configured logical-operation count.

## Comparison runners

`compare_experiment_trace` first applies `validate_experiment_compatibility`. Logical model,
caller-serialization contract, persistence class, standalone distribution, ordered-range support, and key/value
limits must match. Storage architecture and crash-recovery mechanism are deliberately allowed to differ.

The original comparison runner executes both candidates in lockstep. Each setup action is compared before the
next action starts. Only after all setup outcomes agree are both instrumentation windows reset. Measured actions
are likewise executed and checked in lockstep; the first mismatch fails the experiment without emitting
amplification evidence. A successful `ExperimentComparisonReport` stores the full trace once, the proven common
measured-outcome vector once, and per-engine capabilities plus the exact common `AmplificationReport` and
operational report. Successful REOPEN/compaction samples carry the exact measured step index plus deterministic
bytes/record-or-page work without issuing extra measurement I/O. Read-work units remain architecture-specific
(`btree_page_access`, `lsm_sstable_consult`, and `lsm_sstable_version_decoded`) and must not be interpreted as
interchangeable device I/O.

`compare_experiment_trace_ordered` adds explicit whole-run execution order without changing left/right engine
identity. With `left_then_right`, the left engine completes setup, reset, and the complete measured window before
the right engine starts; `right_then_left` does the reverse. The second engine is still checked against the
first engine's exact setup and measured outcomes at the same indices, so order control does not weaken logical
correctness.

`compare_experiment_trace_counterbalanced` composes exactly two ordered comparisons into one fresh pair: one
`left_then_right` and one `right_then_left`. Callers provide engine factories, and each factory is invoked once
per repetition, requiring four fresh instances total. The pair fails closed if left/right capabilities or the
proven measured logical outcomes change between repetitions. Pair sequencing itself is explicit provenance:
`left_then_right_first` means AB then BA; `right_then_left_first` means BA then AB. Both raw repetitions are
retained and no duration aggregation or performance claim is performed by the core runner.

## CLI

Generate a trace:

```text
db-lab experiment-generate \
  --profile mixed \
  --seed 42 \
  --operations 1000 \
  --key-space 4096 \
  --value-bytes 128 \
  --range-limit 16 \
  --reopen-every 250 \
  --output mixed-42.json
```

Run it against fresh candidates and write a self-contained lockstep report:

```text
db-lab experiment-compare \
  --trace mixed-42.json \
  --btree-path btree-42.db \
  --lsm-path lsm-42 \
  --btree-cache-pages 64 \
  --output mixed-42-report.json
```

The three output/storage paths must be distinct and must not already exist. This prevents an experiment from
silently inheriting old engine state or overwriting prior evidence.

For publication-oriented order control, archive one fresh AB/BA pair directly:

```text
db-lab experiment-archive-counterbalanced \
  --trace mixed-42.json \
  --first-btree-path btree-42-a.db \
  --first-lsm-path lsm-42-a \
  --second-btree-path btree-42-b.db \
  --second-lsm-path lsm-42-b \
  --pair-order left-then-right-first \
  --btree-cache-pages 64 \
  --revision 0123456789abcdef0123456789abcdef01234567 \
  --archive-dir evidence/mixed-42-abba \
  --cache-state warm \
  --host-label lab-host-a \
  --filesystem ext4 \
  --storage-device nvme-model
```

All four engine targets plus the archive directory must be pairwise distinct and absent before execution. This
preflight occurs before either repetition is created, preventing the second half of a pair from silently reusing
state or overwriting the first half.

A methodological exclusion uses the same immutable publication boundary and the same planned target names, but
adds `--exclude-reason`. The reason must contain 1..=4096 UTF-8 bytes after trimming. After trace/revision/path
preflight succeeds, no engine is created and no measured operation executes. The command writes a format-v3
`attempt.json` with `status = "excluded"` and the reason, plus the trace/environment/index. For example:

```text
db-lab experiment-archive-counterbalanced \
  --trace mixed-42.json \
  --first-btree-path btree-42-a.db \
  --first-lsm-path lsm-42-a \
  --second-btree-path btree-42-b.db \
  --second-lsm-path lsm-42-b \
  --revision 0123456789abcdef0123456789abcdef01234567 \
  --archive-dir evidence/mixed-42-excluded \
  --cache-state warm \
  --exclude-reason "host load exceeded the frozen admission threshold"
```

If execution itself fails after preflight, the command also writes a format-v3 non-success attempt archive before
returning the original database error with a non-zero exit status. `attempt.json` records `status = "failed"`,
the stable `ErrorClass`, and the full error message. Partially created engine targets are intentionally left in
place as forensic state; only a partially written evidence archive is removed if archive serialization fails.

## Scope boundary

The trace and comparison layer now establishes canonical bounded logical inputs, explicit setup/measurement
boundaries, lockstep outcome equality, optional whole-run order control, fresh AB/BA counterbalanced pairs,
shared structural amplification reporting, deterministic measured-step association for successful
recovery/compaction work samples, and immutable run-level accounting for failed or explicitly excluded
counterbalanced attempts. It does **not** establish a fair latency benchmark by itself. An enforced
cache/filesystem preparation protocol and controlled-host pinning remain separate Phase 4 work.

## Evidence archives

`db-lab experiment-archive` remains the backward-compatible single-comparison publication boundary. It consumes
one existing trace, creates fresh B+ tree and LSM targets, runs the exact shared lockstep comparison, and then
creates a new format-v1 archive directory containing four JSON files: `trace.json`, `comparison.json`,
`environment.json`, and `index.json`. The caller must provide `--revision`; the archive also records the db-lab
package version, target OS/arch, build profile, best-effort Rust compiler version, B+ tree cache capacity, a
declared cache state, and optional host/filesystem/storage labels. Existing archive paths are rejected and a
failed multi-file write removes the partial archive directory. Do not put credentials, serial numbers, or other
secrets into labels/notes.

A successful `db-lab experiment-archive-counterbalanced` creates format-v2 evidence rather than changing v1
semantics. It requires four fresh engine targets, runs one complete comparison in each whole-engine order, and
writes `trace.json`, `counterbalanced.json`, `environment.json`, and `index.json`. The counterbalanced report
retains both raw ordered comparisons. The environment repeats `pair_order`, records
`execution_protocol = "fresh_counterbalanced_ab_ba"`, and carries the same source/build/host/cache metadata as
v1. The v2 index names the counterbalanced payload explicitly, so archived evidence is self-describing without
inferring methodology from filenames.

A non-success invocation of the same counterbalanced command creates format-v3 attempt evidence instead. The
archive contains `trace.json`, `attempt.json`, `environment.json`, and `index.json`; the index records
`attempt_protocol = "record_non_success_counterbalanced_attempts"`. An explicit exclusion records its bounded
reason and does not instantiate engines. An execution failure records the common error class and message and
still propagates the original error after the immutable archive is written. Thus a dataset can count successful
v2 runs together with failed/excluded v3 attempts instead of silently conditioning on survivors.

None of these archive formats makes timings comparable by itself. `cold_best_effort` is only a declaration, not
proof that kernel/device caches were flushed. A controlled cache/filesystem procedure and controlled-host pinning
remain mandatory before latency claims or regression thresholds.
