# Amplification evidence methodology

This document freezes the first common reporting contract used before any B+ tree versus LSM performance
claim. The goal is reproducible structural evidence, not a synthetic promise that unlike storage engines
perform the same physical I/O operations.

## Common report shape

`db_core::AmplificationReport` stores exact integer numerator/denominator pairs. Ratios are never rounded
inside the engine and a zero denominator is preserved. Both persistent comparison candidates implement
`AmplificationInstrumented`, whose reset operation changes only process-local counters and whose report
operation does not change logical database state.

The report contains four fields:

| Field | Numerator | Denominator |
| --- | --- | --- |
| `point_read` | structural read work, with an explicit `ReadWorkUnit` | successful explicit `GET` calls |
| `range_read` | structural read work, with an explicit `ReadWorkUnit` | logical records returned by successful range scans |
| `data_write_bytes_per_logical_byte` | documented engine data-path bytes written | acknowledged logical mutation bytes |
| `primary_structure_bytes_per_live_byte` | documented retained primary-structure bytes | live logical key + value bytes represented by that structure |

PUT contributes key + value bytes to the logical write denominator. DELETE contributes key bytes even
when the key is missing, because the successful call is still part of the requested mutation trace.
Internal previous-value lookups used to implement PUT/DELETE return values are never counted as explicit
user point reads.

## Read amplification is structural and unit-tagged

A raw structural numerator is meaningful only together with its `ReadWorkUnit`:

- B+ tree point and range reads use `btree_page_access`: one logical access to a validated 4 KiB data page,
  including a page served from the bounded cache. Overflow-key and overflow-value pages count because the
  logical operation must traverse them.
- LSM point reads use `lsm_sstable_consult`: one SSTable considered before its bounds/Bloom/index path
  establishes hit or miss.
- LSM range reads use `lsm_sstable_version_decoded`: one physical SSTable record version decoded while
  resolving newest visible state.

Therefore `4 btree_page_access / GET` and `4 lsm_sstable_consult / GET` are **not** four units of the same
physical resource. The common schema makes this mismatch machine-visible instead of silently calling both
numbers "read I/O amplification". Device bytes read, cache misses, system calls, page-cache behavior, and
hardware counters belong to a later controlled-host measurement layer.

## B+ tree byte accounting

B+ tree data-write bytes count successfully synchronized leaf, internal, key-overflow, and value-overflow
page images produced during the measurement window. Mirrored allocation/root superblock writes are excluded
from this data-path numerator. Failed mutations are not added to the logical denominator; durable page work
from an ambiguous failed operation is likewise not retroactively assigned to a later successful mutation.

Primary-structure bytes are `committed_data_pages * 4096`. This intentionally includes unreachable COW
history still retained in the page file because those bytes remain allocated storage and are available for
later orphan reuse. The two fixed mirrored superblocks are excluded. The live-byte denominator is rebuilt
from the authoritative root without incrementing public read counters.

Hand-computable regression evidence includes a two-key one-leaf file: after COW history creates two retained
data pages, one recycled-leaf overwrite plus one missing DELETE produces exactly 4096 data-write bytes over
four logical mutation bytes. One explicit point lookup touches one page; a full two-row range touches one
page and returns two records. An 8192-byte value occupies three 4048-byte overflow chunks, so point/range
retrieval each requires exactly one leaf plus three overflow page accesses.

## LSM byte accounting

LSM data-write bytes are complete WAL mutation records plus immutable SSTable bytes produced by MemTable
flushes and compaction outputs. Manifest snapshots, mirrored `CURRENT`, filesystem metadata, cache traffic,
and device writeback are excluded. The separate raw counter for compaction-input SSTable bytes remains
available as architecture-specific evidence but is not added again to the write-output numerator.

Primary-structure bytes are authoritative SSTable bytes divided by durable live key + value bytes represented
by those SSTables. Unflushed WAL/MemTable state is excluded from both sides. Existing hand-computable tests
prove WAL framing, flush/output byte totals, first full-set compaction input, `5/3` point SSTable consults,
and `10/9` decoded range versions per logical result on a layered L0/L1 state.

## Shared trace and measurement-window protocol

Phase 4 generated experiments use `ExperimentTrace` v1 rather than changing the Phase 1 correctness-workload
schema. Every trace stores its profile, SplitMix64 seed, complete generator config, deterministic `setup_steps`,
and exact `measured_steps`. The runner validates all common bounds, executes setup on each fresh candidate,
resets process-local amplification exactly once, and only then enters the measured window. Optional REOPEN
actions may occur inside that window and preserve the existing same-handle counters.

The stable profiles are point-read, range-scan, sequential-write, random-write, and mixed. Generated keys are
eight-byte big-endian ids so numeric order is exactly bytewise order. Point reads use a deterministic 80/20
hit/miss split after a fully seeded setup. Range scans use `[start, min(start + range_limit, key_space))`.
Sequential writes use distinct ascending ids; random writes uniformly select the configured key space; mixed
traces seed even ids and then use 40% PUT, 30% GET, 15% range, and 15% DELETE. Exact rules live in
`docs/experiment-traces.md`.

`compare_experiment_trace` refuses to emit comparison evidence unless setup and measured logical outcomes
match exactly in lockstep. A successful report stores the full trace once, the common measured outcomes once,
and per-engine capabilities plus raw numerator/denominator amplification evidence. This proves identical
logical input and output; it still does not turn unlike structural read units into device-I/O measurements.

## Capability preflight

`validate_experiment_compatibility` rejects a comparison when engines disagree on the common logical model,
caller/concurrency contract, persistence class, distribution mode, ordered-range capability, or key/value
limits. Storage architecture and crash-recovery mechanism are intentionally allowed to differ: those are the
physical design choices the experiment exists to compare. A range-bearing experiment can additionally
require ordered-range support explicitly.

This preflight does not establish performance fairness by itself. Every future benchmark must still record
the exact trace, engine settings, binary revision, operating system, filesystem/device context, cache state,
and raw counters. No latency or device-level conclusion should be published from the structural ratios in
this document alone.

## Recovery and compaction operational samples

Shared experiment evidence also carries `OperationalTimingReport`. The original `reopen_ns` and
`compaction_stall_ns` vectors remain as backward-compatible duration projections. New `reopen_samples` and
`compaction_stall_samples` pair the same integer `std::time::Instant` duration with the zero-based measured
trace-step index that triggered it and deterministic data-path work. The runner sets the index immediately
before each measured action and clears it immediately afterward, including error returns; regression tests
pin the emitted REOPEN indices to the exact measured trace positions.

Operational work is architecture-specific and explicitly unit-tagged:

- B+ tree REOPEN uses `btree_page_access`. `units_examined` is the logical validated-data-page accesses already
  performed by `BPlusTree::open` during reachable-tree validation and reuse discovery; `bytes_examined` is that
  count times 4096. Mirrored superblock metadata is excluded and no extra reads are performed for telemetry.
- LSM REOPEN uses `lsm_record_version`. Units are complete active-WAL records plus authoritative SSTable record
  versions. Bytes are the original WAL extent scanned during open—including a structurally recoverable tail
  before truncation—plus authoritative SSTable file bytes. CURRENT, manifest, and directory metadata are
  excluded. `SsTable::open` already reads each authoritative SSTable completely, so reporting adds no I/O.
- LSM full-set compaction uses `lsm_sstable_record_version`. Units and bytes are the authoritative input
  descriptor entry counts and file sizes captured at trigger entry. The sample is appended only after the new
  version is published, mirrored, and obsolete-file reclamation completes. B+ tree reports no compaction
  samples.

The compatibility duration vectors and structured vectors are appended together and tests require their
indices/durations to agree. Successful-sample work accounting is therefore deterministic and trace-associated.
Whole-engine ordering is explicit: ordered comparisons run one candidate's complete setup/measured window before
the other, and a counterbalanced pair uses four fresh engine instances to execute one AB and one BA run.

## Repeated-attempt ledger and exclusion boundary

The counterbalanced publication path and the reusable repeated-sampling layer intentionally solve different
provenance problems. `experiment-archive-counterbalanced` retains one invocation as backward-compatible format-v2
success evidence; an execution failure or caller-requested methodological exclusion is retained as immutable
format-v3 attempt evidence instead of disappearing from the run-level denominator.

`run_counterbalanced_experiment_batch` sits above one fresh AB/BA pair. The low bit of a recorded `pair_seed`
chooses the first outer pair order and later requested pairs alternate deterministically. Every requested pair has
a zero-based index and one of three dispositions: `included`, `failed`, or `excluded`. Included entries retain the
complete counterbalanced pair report. Failed entries retain a stable `ErrorClass` and diagnostic text; fresh-engine
factory failures additionally identify left/right role and repetition 0/1, while later comparison/runtime failures
are labeled `comparison` without inventing side attribution. Exclusions happen before engine creation and require a
non-empty reason. One failed pair does not abort later requested pairs, preventing harness control flow from
silently turning a requested batch into a success-only sample set.

The repeated batch ledger itself is not yet written by an immutable archive command, and engine-local timing still
records successful REOPEN/compaction samples only: no duration/work sample is retained for an individual failed
REOPEN or compaction operation. Cache/filesystem state also remains declared metadata rather than an enforced
preparation protocol. Scheduler noise, build profile, host identity, cache state, filesystem, and storage device
must therefore still be controlled before timing distributions can support a performance claim.
