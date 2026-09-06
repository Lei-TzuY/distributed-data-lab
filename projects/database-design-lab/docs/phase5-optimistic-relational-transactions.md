# Phase 5 optimistic relational transaction validation

This slice adds a second single-process concurrency-control experiment above the durable relational engine. The previous read-write transaction experiment holds one process-local mutex across reads, decisions, append/sync, and publication. `OptimisticRelationalEngine` narrows that critical section: it captures an explicit row read set under the mutex, runs the transaction body after releasing the mutex, then reacquires the mutex only to validate and durably commit.

## Semantics

Callers declare every row the transaction body may read with `RowRead`. Snapshot capture clones those rows from one mutex-protected relational state. Access to an undeclared row through `OptimisticSnapshot::row` is rejected, so the validator has a complete row read set for this experiment.

Each observed row carries both its value and a process-local row version. Rows present when the wrapper opens start at version zero. After a successful durable relational commit, every upserted/deleted row receives that durable transaction id as its new version. Commit validates both value and version, so an A -> B -> A change during the process lifetime is still detected as a conflict.

The transaction body executes without the durable commit mutex. When it returns, commit reacquires the mutex and validates every observed row before calling `RelationalEngine::commit`. If any observed row changed, the operation returns an `InvalidInput` whose message begins with `optimistic transaction conflict`; no relational transaction record is appended, no transaction id is consumed, and nothing is published in memory. Read-only transactions are also revalidated before they return.

Successful writes still use exactly one existing relational transaction record and the existing append-log `put` + `sync_data` durability boundary. Blind writes that do not depend on a declared read are serialized in durable commit order. The durable relational format remains `DBRELTX1`; this slice adds no new on-disk bytes and requires no migration.

## Oracle and recovery

Deterministic contention tests place two transaction bodies behind a barrier after both snapshots exist. When both read the same account and attempt a read-modify-write increment, exactly one commit succeeds and the other conflicts before append; `next_transaction_id` proves the rejected transaction consumed no durable slot. Reopen proves the single committed increment is the only durable state.

A second barrier test uses disjoint rows. Both bodies overlap outside the mutex, both commits succeed with contiguous durable transaction ids, and reopen reproduces both results. An undeclared-read regression proves the explicit read-set contract fails before commit.

## Deliberate limits

This is row-read optimistic validation, not MVCC or general SQL serializability. Predicate/range scans do not yet carry phantom protection; schema/catalog reads are not part of the optimistic read set; multi-process isolation remains outside the single-process relational ownership boundary; durable appends still serialize; and there is no lock wait, deadlock detection, or parallel fsync. Those are separate hypotheses rather than hidden claims of this slice.
