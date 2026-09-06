# Phase 5 atomic snapshot transaction slice

This slice tests one narrow transaction hypothesis without introducing locking, MVCC, SQL, or a new WAL: can the existing durable append-log provide an explicit multi-key atomic commit boundary when the entire post-transaction logical state is encoded as one append-log value?

## Semantics

`db-log-tx <path> batch ...` accepts an ordered list of `PUT` and `DELETE` mutations. All inputs are decoded and validated before the engine is opened for the commit. Mutations execute in list order against a candidate snapshot. A successful batch appends exactly one snapshot record through `LogEngine::put`, whose existing contract synchronizes the complete checksummed record before acknowledgement. Only after that call succeeds does the live logical snapshot advance.

An empty transaction is not exposed by the CLI. Duplicate keys inside one batch are legal and follow list order. Binary and empty keys/values use the common KV limits. The encoded complete live state must fit the existing 1 MiB value limit; exceeding it fails before the backing append.

## Failure and crash model

The transaction layer deliberately reuses the append-log's established final-record recovery boundary. If a crash leaves the final snapshot record incomplete, reopen validates the structural prefix and truncates that incomplete record as one unit, so none of the transaction's logical mutations become visible. If the record is complete and checksummed, the entire encoded snapshot is visible.

The snapshot payload has its own `DBTXSNAP` magic, version 1, bounded entry lengths, canonical strictly increasing key order, and CRC32 trailer. This payload version is independent of append-log v1 and does not change the existing append-log on-disk format. Old append-log data remains readable because the transaction representation is ordinary key/value content inside an existing record.

I/O or append-log durability errors propagate unchanged. The transaction layer does not skip `sync_data`, weaken append-log ownership checks, or acknowledge an in-memory candidate before the backing record is durable.

## Oracle and portability

Unit coverage applies the same deterministic batches to `db-storage-memory::MemoryEngine` after complete validation and compares logical reads before and after reopen. Integration coverage invokes the production CLI, checks ordered overwrite/delete/reinsert behavior, truncates the final backing record to model a torn commit, and proves reopen restores the entire prior snapshot rather than a partial transaction.

The implementation uses only portable Rust filesystem operations already exercised by the append-log engine; the workspace CI therefore runs the transaction tests on Ubuntu, macOS, and Windows and checks Rust 1.85.

## Deliberate limits / next hypothesis

This design rewrites the complete logical snapshot on each commit and is capped by the 1 MiB backing-value limit. It is evidence for transaction semantics and crash atomicity, not a scalable transaction manager. The next Phase 5 slice should move from snapshot replacement to an incremental transaction record/commit protocol (or an equivalent engine-native batch record) while preserving the same oracle and all-or-none reopen contract. Concurrency control remains a later, separate hypothesis.
