# Experimental constitution

This document is the binding methodology for Database Design Lab. Code may evolve; a result is not a
laboratory result unless it follows these rules.

## 1. Purpose and scope

The laboratory studies architectural trade-offs by holding observable semantics and experimental
conditions constant while changing a named design dimension. The first controlled logical model is a
binary key-value map. The first persistent engine is an append-only record log whose purpose is to
make persistence correctness testable before B+ tree or LSM complexity is introduced.

The laboratory is not a product database, a catalogue of every possible database, or a vehicle for
claiming that every Cartesian-product cell is useful. An implementation must be deep enough to test
its defining invariants. Empty crates, adapter-only wrappers around embedded engines, and benchmark
facades do not count as implementations.

## 2. Current normalized semantics

All comparable engines implement the baseline point/lifecycle semantics below. `RANGE` is a common
optional capability and is comparable only when every participant advertises `ordered_range_scan = true`:

| Step | State transition | Observable result |
| --- | --- | --- |
| `PUT(k, v)` | `map[k] = v` | the previous value or missing |
| `GET(k)` | none | the current value or missing |
| `DELETE(k)` | remove `k`; append-log and LSM engines persist a tombstone even for a miss | the removed value or missing |
| `RANGE(start, end, limit)` | none | up to `limit` live pairs in bytewise key order from `[start, end)`; `end` may be unbounded |
| `REOPEN` | close/reconstruct engine state | successful lifecycle boundary |

Keys and values are arbitrary bytes. Empty keys and values are valid, while missing and empty remain
distinct. Keys are at most 4,096 bytes and values at most 1,048,576 bytes. Ordered range bounds use
the same bytewise ordering as B+ tree routing: the lower bound is inclusive, the upper bound exclusive,
`end = None` is unbounded, equal bounds are empty, and `end < start` is invalid. There is no implicit
text encoding, transaction group, snapshot, TTL, compare-and-swap, or concurrency guarantee.

Workload schema version 1 records an optional seed and ordered point/lifecycle steps; it does not yet
serialize range scans. The built-in generator uses a specified SplitMix64 implementation, not an
unspecified thread RNG. It chooses 50% puts, 30% gets, and 20% deletes; these weights are generator
defaults, not a claim that they model production. A configuration and generated trace are experiment
inputs and must be archived together.

## 3. Capability before comparison

Every engine exposes capabilities for persistence, recovery, distribution, range scans, and common
size bounds. A harness must reject a requested comparison if required capabilities differ. A range-scan
comparison, for example, cannot route the append log through its internal replay map and call that an
on-disk ordered scan; the append log deliberately advertises ordered range support as false.

Only one named independent variable should change in a causal comparison. If a B+ tree uses synced
single-operation writes while an LSM batches unsynced writes, “storage engine” is not the only changed
variable. Such configurations may both be measured, but not presented as a like-for-like result.

## 4. Correctness protocol

Correctness precedes timing:

1. Validate the workload and capability requirements.
2. Execute each step against the deterministic in-memory oracle and candidate.
3. Compare the complete observable result after every step, not only final state.
4. Insert explicit reopen boundaries, including reopen after every operation in dedicated tests.
5. Verify persistent structure independently of opening it for mutation.
6. Exercise adversarial encodings and simulated interrupted appends.
7. Run deterministic randomized state machines using seeds committed in tests or fixtures.

A randomized failure report must include its seed and complete configuration. Shrinking or manual
minimization may help diagnosis, but a minimized input is not ephemeral: before the fix is accepted it
must become a reviewed, deterministic fixture or named regression test that fails without the fix.
The original seed remains recorded when it adds provenance. A passing random rerun never substitutes
for the minimized regression.

Reference databases may be external oracles only when their semantics are mapped explicitly. They may
also be benchmark context, but cannot implement a core engine in this repository or silently supply a
feature the candidate lacks.

## 5. Persistence, corruption, and crash protocol

Every persistence format must have magic, an explicit version, endian rules, bounded lengths, checked
extent arithmetic, and fail-closed validation. Checksums have a stated coverage. Unknown mandatory
flags or versions are rejected; a decoder does not guess.

The append-log v1 fault model tests process interruption at prefixes of a final append. A record is an
atomic replay unit, not an assertion that the filesystem writes all bytes atomically. Opening repairs
only a final prefix that is structurally consistent with the next record: it truncates to the previous
valid boundary and syncs that repair. Complete checksum failures and unexplained bytes are corruption,
including at EOF. Read-only verification reports but does not perform recovery.

Successful mutations call `write_all` and `sync_data` before acknowledgement. Tests may therefore
assert replay of completed calls and removal of an incomplete final record. They may not infer safety
for a filesystem or device that violates sync contracts. Initial file data is synced, but the parent
directory is not; a system crash immediately after first creation may lose the directory entry. A WAL
append I/O error is an ambiguous outcome and poisons the handle until reopen.

Crash consistency is demonstrated through fault injection or prefix/corruption fixtures, never by the
mere presence of a log or checksum. The B+ tree defines its legal COW/root-publication crash states in
its page-format specification and exercises a deterministic durable-write matrix: appended/recycled
overflow, leaf, and internal pages plus allocation/root superblocks are failed before write, after a
synchronized half write, and after a complete synchronized write whose acknowledgement is forced to
fail. Reopen must expose the complete old tree or complete new tree, and the live handle is poisoned
after every injected write error. This is a software fault model under the stated sync contract, not an
exhaustive model of device/controller/power-loss behavior. The current LSM foundation applies the same
discipline to its WAL: structural prefix cuts are recoverable only for the final expected record, while
complete checksum failures and unexplained tails fail closed. SSTable flush and manifest publication do
not exist yet, so no crash guarantee for either is claimed.

## 6. Metrics definitions

No number may be published without its raw run metadata and an operational definition. At minimum:

- **Latency:** monotonic elapsed time around the named operation boundary; report sample count and
  distribution (at least median, p95, and p99), not only an average.
- **Throughput:** completed comparable logical operations divided by measured wall time; state thread
  count, batching, durability mode, and workload mix.
- **Logical write bytes:** key plus value bytes for puts and key bytes for deletes, reported separately
  when zero-length operations would make a ratio undefined.
- **Write amplification:** physical bytes written by the engine and measured background work divided
  by logical write bytes. State whether metadata, checksums, WAL, compaction, and filesystem effects
  are included.
- **Read amplification:** physical engine bytes/pages/tables examined per completed logical read,
  with the unit and cache layer stated.
- **Space amplification:** physical bytes retained divided by the defined minimum live logical bytes;
  report empty-dataset cases separately rather than dividing by zero.
- **Recovery cost:** elapsed reopen/recovery time plus bytes and records examined; identify whether OS
  caches were warm or cold.
- **Compaction latency:** foreground stall and background work distributions, once compaction exists.

Engine instrumentation for a metric must be validated against a simple trace before use. Wall-clock
runtime alone cannot establish internal read/write amplification. Complexity claims must name the
operation, parameters, amortization, and implementation evidence.

## 7. Reproducible performance methodology

An experiment record must include repository commit, Rust version, target triple, optimization flags,
host CPU/memory/storage, operating system, filesystem and mount options, durability mode, cache/warmup
policy, workload file or seed/config, repetition count, raw samples, and analysis script version.
Order variants randomly or counterbalance them when order can bias cache or thermal state.

Correctness tests and timing experiments are separate commands/jobs. GitHub-hosted CI may compile and
smoke-run benchmark code, but its timing is not a stable baseline. A performance regression gate
requires a named pinned host, controlled noise budget, repeated samples, and a reviewed statistical
threshold. Until that exists, no hosted-CI timing change blocks a merge or supports a performance
claim.

## 8. Review and reporting rules

- Never fabricate throughput, latency, durability, amplification, crash coverage, or complexity.
- Report failures and excluded samples with reasons; do not silently discard them.
- Preserve raw data. Derived tables and plots must be reproducible from checked-in or archived inputs.
- Label simulations and fault models precisely; do not generalize them to all hardware failures.
- A roadmap item is complete only when linked implementation and tests exist on the default branch.
- One focused pull request should establish one coherent evidence increment.

## 9. Current non-goals

The current baseline does not implement SQL, relational schemas, transactions, MVCC, 2PL, OCC,
multi-process concurrency, file locking, an ordered append-log access path, generalized multi-run or
multi-level LSM compaction, snapshots, replication-aware tombstone retention, a disk block cache,
physical B+ tree file compaction, validated amplification instrumentation, replication, sharding,
consensus, graph traversal, time-series retention, columnar execution, online format migration,
encryption, or production operations. These are intentionally deferred rather than represented by
placeholders.
