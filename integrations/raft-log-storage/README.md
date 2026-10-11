# Raft committed-prefix to durable log storage

This bounded integration executes both imported projects. The real
`distributed-systems-lab` Raft implementation elects a leader, replicates a
current-term command prefix to a majority, advances the commit index, and applies
the committed prefix on two replicas. Only those applied `Put`/`Delete` commands
are projected into the versioned workload schema consumed by the real
`database-design-lab` `db-lab` process.

The database side uses its checksummed, synchronized append-log engine. A second
independent `db-lab` process reopens the durable file and reads every touched key.
The integration requires those reads to equal the state independently derived by
both Raft replicas.

Run from the umbrella root after installing the Python project and building the
Rust CLI:

```console
python -m pip install -e "projects/distributed-systems-lab[dev]"
cargo build --manifest-path projects/database-design-lab/Cargo.toml --locked -p db-cli
DB_LAB_BIN=projects/database-design-lab/target/debug/db-lab \
  python integrations/raft-log-storage/contract.py
```

## Exact contract

- participant checkpoints are pinned in `integrations/manifest.json`;
- the producer is the Raft majority-committed, state-machine-applied prefix, not
  the leader's uncommitted log;
- keys and values are projected from Python `str` through UTF-8 to the database
  workload's lowercase hexadecimal byte-string representation;
- `Put` and `Delete` order is preserved exactly;
- two Raft replicas must converge before storage comparison;
- the durable append-log state must remain equivalent after a process boundary.


## Resumable projection

`project_applied_prefix(binary, state_path, applied_records)` accepts the complete,
uncompacted `AppliedEntry` prefix, rather than a leader's raw log. Use a fresh
append-log file and then reserve it for this single writer. A binary metadata key
outside the UTF-8 application-key space stores the applied frontier, its canonical
index/term/operation fingerprint, and any pending target.

The projector synchronizes an intent before writing the new suffix and records
completion only after the real database process has synchronized those mutations.
A retry after interruption replays the idempotent Put/Delete suffix. A completed,
unchanged prefix produces no new writes. A conflicting history, source behind the
completed frontier, or source behind a pending target fails before any new writes.

The executable contract also checks unchanged-prefix replay. Regression tests
exercise incremental replay, interruptions immediately before/after the real data
subprocess, stale-source rejection during recovery, and history conflicts that
leave the final user values identical. This does not promise physical exactly-once
records, arbitrary crash timing, concurrent projectors, compacted-prefix/snapshot
support, or atomic consensus acknowledgement and database durability.

## Limitations

This does not make either imported project a production distributed database. It
does not connect Raft acknowledgement to storage `sync_data` atomically, replicate
database files, propagate client deduplication identities, implement snapshots or
SQL transactions, test arbitrary crash timing, or prove performance, availability,
linearizability beyond the exercised prefix, or Byzantine behavior.
