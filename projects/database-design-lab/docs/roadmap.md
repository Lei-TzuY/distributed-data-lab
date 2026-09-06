# Evidence-driven roadmap

Checkboxes are tied to implementation on the default branch. A phase title is not “complete” merely
because later work has begun.

## Phase 0 — Experimental constitution

- [x] Reject the “exactly 750 database types” framing and define the matrix as a constrained design
  space (`README.md`, `docs/design-space.md`).
- [x] Define current dimensions, capability status, compatibility caveats, and cross-dimension leakage
  (`docs/design-space.md`).
- [x] Define common semantics, correctness protocol, deterministic seed policy, minimized-regression
  rule, crash fault model, operational metrics, reproducibility requirements, and non-goals
  (`docs/experimental-constitution.md`).
- [x] Specify append-log v1 bytes and recovery decisions (`docs/on-disk-format.md`).

## Phase 1 — Common correctness laboratory

- [x] Versioned binary KV workload representation with `PUT`, `GET`, `DELETE`, and `REOPEN`
  (`db-core`).
- [x] Stable SplitMix64 deterministic generator with recorded seed/configuration (`db-core`).
- [x] Deterministic in-memory oracle (`db-storage-memory`).
- [x] Real persistent append-log engine with versioning, bounded decoding, checked arithmetic, header
  and record checksums, replay, synced mutations, tombstones, and explicit tail recovery
  (`db-storage-log`).
- [x] Step-by-step differential harness and deterministic recorded-seed state-machine tests.
- [x] Adversarial tests for overwrite/delete/reinsert, reopen boundaries, binary/empty/boundary values,
  prefix interruption, checksum failure, absurd lengths, offset overflow, duplicate/tombstone replay,
  and pre-existing truncated headers.
- [x] CLI generation, execution, differential comparison, non-mutating verification, and inspection.
- [x] Stable-Rust formatting, Clippy, tests, docs, and cross-platform CI.
- [x] Add automated deterministic shrinking/minimization for newly discovered failing traces.
  `db-core::minimize_failing_workload` performs chunk-removal delta debugging plus 1-minimal cleanup;
  `db-lab-shrink` replays every probe against a fresh persistent candidate, preserves the original
  differential failure signature and workload provenance, and writes a create-new minimized JSON regression.
- [x] Compaction and retained-generation ownership. The repository has non-destructive compact-copy
  construction; a strict retained generation-directory recovery contract with marker-bound committed-prefix
  proof; durable final-marker publication and generation-id reservations; a reservation-before-build
  authoritative compact switch; generation-aware routed mutations; cooperative cross-process writer
  exclusion and guarded stale-lock recovery; deterministic composed switch fault coverage; durable cleanup
  of obsolete lower history; reservation-backed guarded retirement of abandoned higher candidates/staging
  evidence; an offline legacy one-file migration that retains the source while handing imported state to
  `GenerationLogEngine`; and explicit Windows namespace-retirement/cutover protocols using audited
  write-through Win32 moves. The correctness boundary is explicitly cooperative: coordinated writers must
  respect the writer lease, while hostile same-user direct filesystem mutation through deliberate managed
  raw-path access is outside the laboratory's sandboxing claims (`docs/append-log-ownership-boundary.md`,
  `log_generation_ownership_contract_integration`).

## Phase 2 — B+ tree engine

Do not create a B+ tree crate until its first PR includes executable pager/page behavior.

- [x] Write page-format and crash-state design with version/checksum/free-space invariants
  (`docs/btree-page-format.md`).
- [x] Implement bounded pager and cache with corruption validation (`db-storage-btree`): mirrored
  superblocks, synchronized immutable page allocation, slotted-page packing, root metadata commits,
  checksum/reference validation, bounded cache eviction, interrupted-allocation recovery, and torn
  superblock/truncation/corruption tests.
- [x] Implement binary point lookup/insertion plus root and non-root split propagation using immutable
  copy-on-write path replacement and atomic root publication. Reopen validates the complete reachable
  tree and tests cover binary/empty keys, overwrite, multi-level splits, reopen, input bounds, and
  unpublished shadow-page behavior.
- [x] Implement copy-on-write deletion with missing-key no-op semantics, byte-aware adjacent-sibling
  redistribution/merge, empty-tree publication, and root contraction; deterministic multi-level tests
  delete a height-3+ tree down to one leaf and validate reopen.
- [x] Reclaim unreachable copy-on-write history as reusable allocation space by deriving orphan page
  ids from validated current-root reachability before each mutation. Recycled pages are synchronized
  before root publication, never overwrite the authoritative tree, and deterministic tests prove file
  page count stabilizes across repeated updates and empty-tree reuse. Physical file compaction remains deferred.
- [x] Support values through the common 1 MiB limit with checksummed overflow-page chains. Leaf
  references preserve logical length, overflow pages are committed tail-to-head before leaf/root
  publication, reopen validates canonical chains, and deterministic tests cover 1 MiB round-trip,
  delete, and orphan-page reuse without unbounded page-count growth.
- [x] Expose true ordered scans through the common capability contract using bounded half-open
  `[start, end)` traversal over internal child order rather than leaf sibling links. Exact child minima
  prune pre-start subtrees and stop at the upper bound; tests prove sorted/limited/read-only behavior,
  reopen/delete/overflow-value correctness, and equality with the in-memory oracle.
- [x] Admit B+ tree to the common `KvEngine` differential harness. Keys through 4 KiB use inline
  descriptors or checksummed overflow blobs in leaves and internal exact-minimum separators; trait-level
  capabilities/reopen match the common contract, and deterministic differential tests cover empty/binary
  keys, the 4 KiB/1 MiB size limits, overwrite, delete, and repeated reopen against the memory oracle.
- [x] Add deterministic mutation fault injection beyond the unpublished-shadow-page protocol. The
  pager records durable write classes and tests pre-write, synchronized half-write, and post-sync
  reported failures across appended/recycled overflow, leaf, and internal pages plus allocation/root
  superblocks. Reopen is constrained to the complete old or complete new tree; tests also cover final
  root clearing and prove torn recycled orphans can be overwritten safely by a later mutation.

## Phase 3 — LSM engine

Begin only after the B+ tree and common ordered semantics are trustworthy.

- [x] Specify and implement WAL/SSTable/manifest persistence through atomic flush installation. WAL v1
  remains in `docs/lsm-wal-format.md`; `docs/lsm-sstable-manifest-format.md` defines indexed/checksummed
  immutable SSTables, complete immutable manifest snapshots, mirrored 4 KiB `CURRENT` slots, durable
  sequence watermarks, canonical orphan handling, and the WAL-backed interrupted-install recovery rule.
- [x] Specify and implement crash-safe WAL segment rotation/reclamation. Manifest v2 binds an active WAL
  id and first sequence; rotation occurs only when the SSTable durable watermark reaches that WAL's tail.
  A new segment is synchronized before publication, the same new manifest is installed into both CURRENT
  mirrors before old WAL deletion, legacy Manifest v1 remains readable, and canonical orphan WAL ids are
  skipped rather than overwritten.
- [x] Implement an independent versioned/checksummed WAL with synchronized PUT/tombstone records,
  fail-closed bounded replay, canonical incomplete-tail recovery, and ordered mutable/immutable
  MemTables. Fixed-seed differential tests cover reopen after every operation; deterministic tests
  cover freeze/read precedence, ordered ranges, 4 KiB/1 MiB bounds, structural WAL prefixes, bit flips,
  absurd lengths, sequence gaps, tombstones, and undeclared directory entries.
- [x] Implement immutable sorted tables with complete indexes, per-record/index/header/footer checksums,
  whole-file checksum validation, full 4 KiB-key/1 MiB-value support, and manifest-bound key/extent metadata.
- [x] Add embedded checksummed SSTable v2 Bloom filters with 10 bits/key and 7 deterministic probes.
  SSTable v1 remains readable; v2 open validates every indexed key as filter-positive before point reads
  may trust a negative result. A fixed 10,000-key / 50,000-absent-key corpus produces 422 false positives
  (0.844%) and remains gated below 2%; corruption and tombstone-key coverage are tested explicitly.
- [x] Implement tombstone-aware multi-SSTable point/range reads plus crash-safe flush manifest recovery.
  Tests cover authoritative SSTable/manifest corruption, latest-CURRENT-slot fallback with WAL replay,
  unreferenced canonical orphans, mutable WAL tails, and maximum-value flush/reopen.
- [x] Add an explicit L0/L1 overlap policy and crash-published compaction. Manifest v3 records levels;
  flushes enter overlapping L0 and four L0 tables trigger a full-set rewrite into at most one L1 run.
  Any output SSTable and the manifest are synchronized and published through both CURRENT mirrors before
  obsolete SSTables/manifests are eligible for deletion. Tests cover reopen, mirror fallback after cleanup, and
  newer L0 state overriding L1; tombstone elision is established by the following milestone.
- [x] Prove and implement safe tombstone dropping for the current serialized, snapshot-free full-set
  compactor. Manifest v4 records a GC watermark, permits a GC-covered table-less durable version, carries
  the watermark through later flush/WAL rotation, and validates legacy v1–v3 manifests with an implicit
  zero watermark. Tests cover physical elision, reinsertion, fully deleted reopen/refill, corrupt
  watermarks, and old/new crash publication.
- [x] Persist the SSTable allocation frontier through table-less GC and orphan cleanup. Manifest v5
  extends the header with a monotonic table-id high watermark while preserving the v4 GC field in place;
  open raises the floor from every canonical SSTable name before cleanup and the next v5 publication
  makes that reservation durable. Tests prove v1–v4 readability, conservative table-less v4 migration,
  checksum-valid invalid-watermark rejection, and crash orphan id 99 being followed by id 100 after the
  orphan name has been removed.
- [x] Add deterministic compaction durable-write fault injection. The harness records replacement-L1,
  Manifest, first-CURRENT, and mirror-CURRENT publication classes and injects before-write, synchronized
  torn-output, and post-sync reported failures at each class. Every case poisons the live handle, reopens,
  verifies all logical keys, and requires exactly the complete four-L0 input version or complete one-L1
  compacted version; torn immutable files remain unreferenced and torn CURRENT slots fail by checksum.
  A second matrix covers table-less compaction and requires either all four tombstone L0 inputs or the
  complete zero-SSTable GC version, including the retained-WAL state when rotation is skipped after a
  reported compaction failure.
- [x] Add deterministic compaction differential tests and read/write/space-amplification instrumentation
  validation. Two full-set compaction cycles are checked against the in-memory oracle across overwrites,
  deletes, ranges, and reopen. Resettable process-local counters expose exact integer point-read, range-read,
  data-write, and sorted-table-space ratios; hand-computable tests prove 5/3 point consults, 10/9 range
  versions/results, WAL framing, first-compaction input=flush-output bytes, and authoritative SSTable sizes.

## Phase 4 — Fair B+ tree versus LSM experiments

- [x] Freeze common point/range/write/delete/durability semantics and capability preflight. The
  reusable preflight rejects mismatched logical model, caller/concurrency contract, persistence class,
  distribution mode, ordered-range support, or key/value limits while allowing storage architecture and
  recovery mechanism to remain the experimental variables.
- [x] Implement reproducible point-read, range-scan, sequential-write, random-write, and mixed traces.
  `ExperimentTrace` v1 records the full generator config and stable SplitMix64 seed, separates deterministic
  setup from the measured window, uses byte-order-preserving fixed-width keys, supports measured REOPEN,
  and is exposed through `db-lab experiment-generate`. Validation binds the encoded steps to the exact
  generator metadata and enforces explicit trace/outcome resource budgets.
- [x] Drive the common amplification contract from shared cross-engine traces. `compare_experiment_trace`
  applies capability preflight, executes and checks setup lockstep, resets both instrumentation windows, then
  executes and checks the measured vector lockstep. It rejects the first logical-outcome divergence and returns
  one self-contained trace/outcome record plus per-engine capabilities and exact amplification evidence. A real
  B+ tree/LSM mixed-trace test covers PUT/GET/DELETE/range/REOPEN under this runner. Structural read units remain
  explicitly non-device-I/O.
- [ ] Complete recovery-cost and compaction-stall distributions. Successful samples pair duration with exact
  measured trace-step indices and deterministic data-path work. Failed B+ tree REOPEN and LSM compaction
  operations retain duration, step, stable error class, and deterministic work only when completed work can be
  proven without guessing. Whole-run order is explicit; fresh AB/BA pairs and repeated batches preserve the
  failing ordered-run timing reports, the failed repetition index, and an already-completed first repetition.
  `db-lab-batch` retains every requested pair as included, failed, or explicitly excluded and publication mode
  enforces release-only `publication_warm_v1`, complete host/storage/filesystem/build/analysis/noise metadata,
  and rejects unverified cold-cache claims. `db-lab-batch-analyze` supplies verified order-stratified nearest-rank
  p50/p95 descriptive summaries, while `db-lab-batch-analysis-bundle` retains them beside byte-stable raw
  evidence. `controlled_publication_session_v2` temporally binds passing host pre/postflight snapshots to v7/v11
  publication evidence. Remaining blockers are real controlled-host data collection and review of those retained
  distributions; repository/hosted-CI timing is not a performance baseline.
- [x] Archive raw data and environment manifests before any result is publishable. The archive family remains
  versioned rather than silently mutating old evidence: single-run lockstep v1; exploratory counterbalanced
  success v2 and failed/excluded v3; single-pair publication success v4 and failed/excluded v5; normal
  exploratory/publication repeated batches v6/v7. Failure-sidecar v8/v9 remain frozen legacy formats because
  they did not carry the repeated pair index. New contextual failure archives use v10/v11 and
  `ordered_comparison_failure_sidecar_v2`; every sidecar entry binds `pair_index`/`pair_order` to the nested
  failed repetition, any completed first repetition, stable failure identity, and both engine timing reports
  without changing v6/v7 success schemas. Every path rejects existing archive targets and removes partial
  multi-file archives on write failure.
- [ ] Establish a controlled pinned performance host before adding regression gates. Linux
  `db-lab-host-preflight` checks exact affinity, online CPUs, the `performance` governor, turbo/boost disablement,
  and an explicit load budget; `controlled_publication_session_v2` encloses publication evidence between passing
  pre/postflight snapshots; and `scripts/run-controlled-publication.sh` orchestrates the full collection path.
  The item remains open until those controls are exercised on a real named and reviewed host and actual repeated
  evidence is collected there; hosted CI remains correctness/build/orchestration validation only.

## Phase 5+ — selected extensions

Choose one hypothesis at a time based on evidence from the storage laboratory. Possible work includes a
relational logical layer, explicit transaction semantics and one concurrency protocol, or standalone
state-machine integration for replication research. Raft, MVCC, SQL, graph, time-series, and columnar
engines remain out of scope until selected by a focused design proposal with a real implementation
plan and comparable correctness oracle.
