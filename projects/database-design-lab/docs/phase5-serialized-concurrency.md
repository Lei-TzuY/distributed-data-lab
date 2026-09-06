# Phase 5 serialized single-process concurrency

`db-log-tx-serialized` is the first explicit concurrency-control slice built on the version-2 incremental durable transaction protocol. It keeps the existing on-disk transaction namespace and record encoding unchanged.

## Isolation and commit semantics

One process owns one `SerializedTransactionEngine`. Concurrent callers share that engine and enter a process-local mutex before transaction validation, encoding, transaction-id assignment, the single underlying `LogEngine::put` append plus `sync_data`, and in-memory publication. A successful transaction therefore receives one unique contiguous transaction id, and those ids define the complete serialization order.

The experiment provides strict single-process serialization of complete transaction batches: no successful transaction can observe or publish a partial batch, and two successful concurrent commits are equivalent to running their whole batches sequentially in transaction-id order. The underlying append log still performs exactly one durable append/sync boundary per committed transaction.

## Recovery and oracle

The durable format is exactly the incremental v2 format documented in `phase5-incremental-transactions.md`. Reopen validates contiguous transaction ids, decodes each checksummed mutation set fail-closed, and replays the committed sequence. A structurally valid incomplete final append remains recoverable as one whole uncommitted transaction.

Tests launch contending transaction threads, record their assigned transaction ids, replay those batches into `MemoryEngine` in committed-id order, compare live state, drop the shared engine, reopen the durable log, and compare again. The CLI integration test performs the same contention path through `concurrent` and verifies persisted outcomes across fresh process opens.

## Executable interface

Each `concurrent` argument is one comma-separated transaction batch; all batch arguments are submitted concurrently through the shared engine:

```text
db-log-tx-serialized <path> concurrent \
  put:78:6f6e65,put:61:31 \
  put:78:74776f,delete:61 \
  put:62:33,put:78:7468726565
```

Output reports each worker's assigned transaction id. Worker order is not the serialization order; transaction-id order is authoritative.

## Explicitly deferred

This slice does not claim multi-process writer safety, reader snapshots, MVCC, deadlock detection, lock granularity below the full transaction, parallel durable commits, or relational isolation levels beyond the demonstrated single-process serial execution model. Those require separate evidence and design work.
