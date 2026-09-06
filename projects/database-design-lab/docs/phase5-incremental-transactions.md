# Phase 5 incremental transaction records

This slice removes the full-live-snapshot rewrite from the first Phase 5 transaction experiment without introducing locking, MVCC, or a second storage engine.

`db-log-tx-incremental` stores each committed ordered PUT/DELETE batch as exactly one append-log value under the reserved key prefix `00 64 62 2d 6c 61 62 2d 74 78 2d 76 32 2f || tx_id_be64`. Transaction ids start at 1 and must remain contiguous. The value format is version 2: `DBTXMUT2`, u16 version, zeroed reserved u16, little-endian u64 transaction id, little-endian u32 mutation count, bounded mutation entries, then CRC32 over all preceding value bytes. PUT and DELETE entries carry explicit kind/key/value lengths; DELETE must have a zero value length.

## Commit and recovery semantics

The engine validates and encodes the whole mutation set before calling `LogEngine::put` once. That existing append-log mutation performs the write and `sync_data` before acknowledging the commit, so there is one durable record and one commit boundary per transaction. In-memory state advances only after that call succeeds.

Reopen uses read-only append-log inspection after normal append-log recovery, requires only v2 transaction keys, verifies contiguous ids and key/value id agreement, decodes each transaction fail-closed, and replays mutations in transaction order. A structurally valid incomplete final append is therefore discarded as one transaction before replay. A complete malformed transaction, checksum mismatch, id gap, unknown live key, oversized payload, or invalid KV mutation fails closed.

Record growth is proportional to the current mutation set rather than the complete live logical state. Historical transactions remain in the append log, so this is an incremental commit protocol experiment, not yet transaction-log garbage collection or checkpointing.

## Compatibility boundary

The existing `db-log-tx` full-snapshot v1 executable remains unchanged and supported. The v2 executable deliberately rejects a database containing the v1 reserved snapshot key or any other non-v2 live key. It never silently combines the two protocols. Migration/checkpointing between v1 and v2 is deferred to a separate bounded slice if evidence justifies it.

## Deferred

Concurrent writers, isolation levels, locking, deadlock handling, MVCC, checkpoints, transaction-log compaction, and relational semantics remain out of scope for this slice.
