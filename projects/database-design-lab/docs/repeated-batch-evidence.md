# Repeated counterbalanced batch evidence

`db-lab-batch` is the immutable archive path for repeated Phase 4 experiments. It turns the reusable
`db-core` repeated-batch ledger into a portable evidence bundle without discarding unsuccessful or deliberately
excluded pairs.

## Execution and retention contract

One invocation receives a validated experiment trace, a recorded `pair_seed`, and `--pairs N`. The seed's low
bit selects the first pair's outer order and later pair indices alternate that order deterministically. Every
included pair still contains one left-then-right and one right-then-left whole-engine execution.

Every fresh engine instance is created beneath a new `--engine-root` using the stable layout
`pair-{pair_index:06}/repetition-{repetition_index}/{btree.db|lsm}`. `--engine-root` and `--archive-dir` must both
be absent before the invocation and may not be nested inside each other.

Pairs may be excluded before engine creation with repeated `--exclude-pair INDEX=REASON` arguments. Reasons are
trimmed, bounded to 4 KiB, unique by pair index, and retained verbatim in the batch ledger. Runtime or factory
failures are retained and do not prevent later pairs from running. The command writes the complete archive
before returning a non-zero status for retained failed pairs, so automation observes the failure without losing
the denominator or diagnostic evidence.

## Stable normal formats v6 and v7

When no ordered comparison failure sidecar exists, the archive shape remains unchanged:

- format v6: default `--admission exploratory`;
- format v7: `--admission publication-warm-v1`.

Both write the same immutable data-file set:

- `trace.json` — exact validated input trace;
- `batch.json` — complete `CounterbalancedExperimentBatchReport`, including every requested pair;
- `environment.json` — source revision, pair seed/count, engine layout, build/target/rustc identity, declared
  environment metadata, B+ tree cache capacity, timestamp, notes, and publication admission when applicable;
- `index.json` — format version, repository revision, protocol identifiers, and exact archive file list.

The stable batch protocols are
`execution_protocol = "fresh_counterbalanced_repeated_batch_v1"` and
`attempt_protocol = "retain_all_requested_pairs_v1"`. Existing paths are never overwritten. Partial archive
directories are removed if serialization or durable file creation fails.

Exploratory v6 omits `publication_admission` and `admission_protocol`. Publication v7 requires a release build,
`--cache-state warm`, a Rust host target triple, complete host/CPU/memory/storage/filesystem/mount metadata,
optimization flags, analysis-script version, noise budget, and at least one non-excluded pair. Its admission
record freezes `admission_protocol = "publication_warm_v1"`, `cache_policy = "trace_induced_warm"`,
`durability_mode = "synced_single_operation"`, `pair_order_policy = "pair_seed_low_bit_then_alternate"`, the
requested pair count, and two ordered comparisons per included pair. `cold_best_effort` is not admitted as a
claim of cold kernel/device caches.

## Captured comparison-failure formats

A factory failure can occur before both fresh engines exist; it therefore remains only in `batch.json` and must
not fabricate engine-local operational telemetry. By contrast, once both engines exist and an ordered comparison
starts, the captured runner snapshots both `OperationalTimingReport` values before the instances are dropped.

Formats v8/v9 were the first immutable sidecar format and are permanently frozen. They used
`comparison_failure_protocol = "ordered_comparison_failure_sidecar_v1"` and retained pair order, failed
repetition, any completed first repetition, the failing ordered run, error identity, and engine timing reports.
They did **not** include the repeated-batch `pair_index`. Because outer pair order alternates and later repeats,
that legacy sidecar cannot always be joined unambiguously to one failed `batch.json` row from its pair order
alone. The format is documented rather than silently redefined.

New captured archives therefore use:

- format v10: exploratory repeated batch with contextual comparison-failure evidence;
- format v11: `publication_warm_v1` repeated batch with contextual comparison-failure evidence;
- `comparison_failure_protocol = "ordered_comparison_failure_sidecar_v2"`.

v10/v11 add `comparison-failures.json` to the normal file set. Each sidecar entry contains:

- `context.pair_index` and `context.pair_order`, exactly identifying the failed requested pair;
- nested pair-level failure evidence whose `pair_order` must match the context;
- `repetition_index` for the failed ordered comparison;
- `completed_first` when repetition zero completed before repetition one failed;
- failing whole-run execution order, stable error class/message, and both engines' operational timing reports;
- failed REOPEN/compaction samples with measured-step and duration evidence, and deterministic work only when the
  engine can prove completed work without guessing.

`environment.json` and `index.json` record the sidecar protocol only for contextual failure archives. Normal v6
and v7 outputs continue to omit those fields and the sidecar file.

## Publication example

```text
db-lab-batch \
  --trace mixed-42.json \
  --engine-root runs/mixed-42-engines \
  --archive-dir evidence/mixed-42-batch \
  --pair-seed 42 \
  --pairs 20 \
  --revision 0123456789abcdef0123456789abcdef01234567 \
  --admission publication-warm-v1 \
  --cache-state warm \
  --host-label perf-host-01 \
  --host-cpu "CPU model / pinned topology" \
  --host-memory "64 GiB / fixed channels" \
  --storage-device "NVMe model" \
  --filesystem ext4 \
  --mount-options "rw,noatime" \
  --optimization-flags "--release; RUSTFLAGS=-C target-cpu=native" \
  --analysis-script-version analysis@abc123 \
  --noise-budget host-noise-budget-v1
```

## Methodology boundary

Repository-side admission and immutable provenance are prerequisites, not performance claims. They do not
independently verify human-supplied hardware/mount labels, pin CPU affinity, disable turbo, control thermals or
background load, or establish a stable device/controller cache state. Hosted CI remains correctness/build
validation only. A publishable latency distribution still requires controlled-host collection and a reviewed,
versioned analysis procedure.
