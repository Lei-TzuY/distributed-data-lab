# TinyDB Stage 1 Stabilization Baseline

This document freezes the current PR #5 implementation as a stage-level integration checkpoint. The goal of this phase is **not** to add more database features. The goal is to preserve the working behavior already implemented, make the integration surface reviewable, and define the gates that must pass before the branch is considered a stable baseline.

## Checkpoint

- Integration branch: `agent/benchmark-and-capacity`
- Source checkpoint before this document: `dd8716c4ef0d41a5dd7370bf00ed6cda4896e607`
- Base branch: `main` at `c0de1768ae3080ec6d12796f82e5abf5a27be89f`
- Diff size at checkpoint: 1,358 commits ahead / 0 behind
- PR workflow on the source checkpoint: success (`Tests` run `33551599214`)
- Previous explicitly recorded full matrix baseline: `35af0f5ef6c924b2d2411a2ebc9a5918ae9d12aa`, Ubuntu 257/257 suites, Windows 257/257 suites

The commits after the earlier 257-suite baseline are focused on schema-catalog V3 publication and schema-repack durability/commit-boundary regression coverage. They are part of this stabilization checkpoint and should be treated as storage-format/recovery work, not a new feature track.

## Integrated subsystem baseline

### Pager, buffer pool, WAL, and recovery

Stage 1 includes:

- growable pager metadata and 64-bit file positioning
- a 16-frame LRU buffer pool
- no-steal dirty eviction/spill behavior
- allocator/free-page durability through WAL metadata
- rollback and savepoint restoration
- explicit pinned page handles and pin-admission barriers
- non-fatal existing-page try-pin acquisition
- `pager_try_close()` and `pager_try_checkpoint()` pressure-aware APIs
- crash/reopen and shared-Pager stress regressions

The pressure-safe APIs are additive. Historical `get_page()`-style paths are not yet globally replaced and therefore full-engine non-fatal behavior under total buffer-pool exhaustion is **not** a Stage 1 guarantee.

### Catalog and multi-table routing

Stage 1 includes:

- persistent independent table roots
- catalog validation and checksummed schema metadata
- catalog-aware prepared routing and PRAGMAs
- V1/V2 compatibility paths
- schema catalog V3 publication for schema-repack transitions
- durable commit-boundary tests for V3 catalog state
- narrow, proven cross-root primary-key join support

Catalog changes must continue to fail closed on malformed metadata, duplicate/invalid roots, corrupted WAL/catalog state, or incomplete migration publication.

### Generic records and V2 slotted storage

Stage 1 includes:

- schema-sized generic payloads
- compact `VARCHAR(n)` layouts
- V2 row envelopes and slotted leaves
- payload-native insert/find/full-scan/range-scan/update/delete paths
- root and non-root split paths with bounded recursive propagation
- tested delete borrow/merge/cascade/root-contraction cases
- fixed-format to compact-V2 migration staging/recovery
- schema-repack staging and publication infrastructure

The historical 293-byte `TinyDBRecord` / `Row` ABI remains a compatibility seam. V2 DELETE has substantial bounded coverage but is not claimed to be a mathematically complete arbitrary-depth rebalancer for every possible tree shape.

### SQL execution and backpressure-safe reads

Stage 1 includes:

- generic INSERT/SELECT/UPDATE/DELETE
- transactions and savepoints on the supported generic paths
- EXPLAIN/planning integration
- non-fatal payload point/full/range reads
- basic generic primary-key equality and FULL_SCAN SELECT pressure preflight
- fail-closed integrity/statistics/user-version diagnostic APIs

Static 16/16 pin-pressure safety for the explicitly covered APIs does not imply arbitrary concurrent SQL safety on one Pager. Range/boolean/index/order SELECT shapes are not yet globally routed through the pressure-safe payload APIs.

### Secondary indexes and planner

Stage 1 includes:

- durable typed secondary-index sidecar snapshots
- schema fingerprints and atomic snapshot publication
- `ANALYZE`
- persisted statistics, MCV, and correlation metadata
- equality/range planning
- same-index bound fusion
- distinct-index intersection
- flat OR/union planning
- deterministic cost-aware scan-vs-index routing

Secondary indexes remain durable sidecars rather than page-managed secondary B+ trees. Composite generic secondary indexes remain outside the Stage 1 guarantee.

### Maintenance and diagnostics

Stage 1 includes:

- whole-database integrity diagnostics
- non-fatal `.page`, `.btree`, `.stats`, and integrity paths under tested pressure conditions
- `db_try_get_stats()`
- `db_try_get_user_version()` / `db_try_set_user_version()`
- multi-table `VACUUM INTO` logical rebuilds with wide payloads and index rebuilds

In-place multi-root `VACUUM` remains intentionally fail-closed.

## Stage 1 freeze policy

Until this checkpoint is integrated or explicitly abandoned, changes on PR #5 should be limited to:

1. correctness fixes for behavior already claimed above;
2. crash/recovery, corruption, pressure, concurrency, portability, and differential regression tests;
3. removal of duplicate/dead integration scaffolding where behavior is already covered elsewhere;
4. documentation that narrows or clarifies guarantees;
5. CI/build fixes required to keep the checkpoint reproducible on Ubuntu and Windows.

Do **not** add new SQL surface area, new index families, new concurrency models, new replication/distribution features, or unrelated storage formats during stabilization.

## Acceptance gates before integration

A Stage 1 merge candidate should satisfy all of the following:

- current PR head is based on the latest intended `main` with no unexpected divergence;
- Ubuntu and Windows full test matrices pass on the exact candidate head;
- crash/recovery tests pass for pager/WAL/catalog/schema-repack publication boundaries;
- integrity checks reject deliberately corrupted or orphaned storage state without publishing partial diagnostics;
- buffer-pool pressure tests preserve the documented BUSY/one-free-frame behavior for the additive try APIs;
- V1/V2 reopen compatibility and schema-catalog V3 reopen/recovery regressions pass;
- wide generic rows survive transaction, reopen, and `VACUUM INTO` paths covered by Stage 1;
- no new feature claim is present without a regression proving it;
- the PR description matches the guarantees and deliberate limitations in this document.

## Explicitly deferred work

The following are useful future projects but should not block the Stage 1 checkpoint unless an existing Stage 1 claim depends on them:

- global replacement of fatal historical page fetches with a blocking/non-fatal buffer-pool policy
- arbitrary concurrent SQL safety on a shared Pager
- complete arbitrary-depth V2 delete/rebalance proof and implementation
- page-managed/composite generic secondary B+ trees
- general schema type repacking and drop-column semantics beyond the proven migration path
- general-purpose join optimization
- multi-process writers, replication, sharding, or consensus
- in-place multi-root `VACUUM`

## Next phase

The next productive phase is **stabilization and reduction**, not feature expansion: review subsystem boundaries, identify duplicated staging helpers, run the full exact-head matrix, add adversarial recovery/fuzz cases, and reduce the PR to a clearly auditable set of guarantees before considering merge.
