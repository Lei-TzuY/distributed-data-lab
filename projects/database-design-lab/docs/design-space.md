# Database architecture design space

The design space is a vocabulary for controlled experiments. Its cells describe hypotheses and
constraints, not “database types,” product counts, or promises of implementation.

## Dimensions

### Logical data model

| Model | Core abstraction | Semantics that must be fixed before comparison | Compatibility caveats |
| --- | --- | --- | --- |
| Relational | typed tuples, relations, constraints | schema, nulls, keys, transaction/isolation behavior, query subset | joins and constraints make access paths and concurrency visible above storage |
| Document | nested heterogeneous records | identity, path update, missing vs null, array/path indexing | large partial updates may require versioned fragments or full rewrites |
| Key-value | opaque byte key to opaque byte value | size bounds, missing/empty, ordering, conditional writes, durability | simplest current control model; range semantics require an ordered key contract |
| Wide-column | partition key with sparse clustered cells | partition/clustering order, timestamps, tombstones, consistency | data model often assumes LSM-like write/reconciliation behavior; not storage-neutral |
| Graph | vertices, edges, properties | identity, adjacency direction, traversal consistency, mutation atomicity | locality and traversal primitives strongly constrain layout and partitioning |
| Time-series | timestamped values grouped into series | timestamp precision, duplicates, lateness, retention, aggregation | time partitioning, compression, retention, and ingestion order leak into storage |

Models can overlap: a document store may expose key-value access, and a time-series system may use a
wide-column representation. Experiments must name the exact logical contract, not only a label.

### Physical storage architecture

| Architecture | Defining mechanism | Expected strength to test | Required caveats |
| --- | --- | --- | --- |
| B/B+ tree | mutable ordered pages with balanced search path | ordered point/range access and bounded lookup depth | page split/merge, free space, torn writes, cache behavior, and concurrency protocol are inseparable details |
| LSM tree | memory buffer plus immutable sorted runs and compaction | write batching and sequential immutable output | read/write/space amplification depends on policy, Bloom filters, levels, tombstones, and workload |
| Append/log-structured | mutations appended and replayed | minimal persistence and sequential append foundation | without compaction, reads require a derived index and space grows with history; it is not automatically an LSM |
| Hash-oriented | bucket/directory placement by hash | expected point access | ordering/range support is absent or separate; resizing and collision policy matter |
| Columnar | values grouped by attribute in segments | projection, encoding, and vectorized scans | point mutation and row reconstruction have different semantics/costs; usually paired with delta structures |

“Log-structured” is overloaded. This repository calls the baseline an **append log**, not an LSM,
because it has no sorted immutable tables, levels, Bloom filters, or compaction.

### Concurrency control

| Model | Commit/conflict rule | Correctness obligations and coupling |
| --- | --- | --- |
| Serialized/single writer | caller admits one mutation at a time | define reader interaction and enforce ownership; the current engine does not provide inter-process locking |
| Two-phase locking (2PL) | locks grow then shrink around a transaction | lock granularity follows records/pages/index gaps; deadlock handling and phantom prevention are part of semantics |
| MVCC | readers select versions by snapshot/visibility rules | version placement, timestamp allocation, garbage collection, and index visibility change physical design |
| OCC | validate read/write sets before commit | validation granularity, starvation, and durable commit ordering must be defined |
| Serializable design | executions equivalent to a serial history by a named mechanism | “serializable MVCC” is not one algorithm; SSI, validation, or strict 2PL need separate capability labels and anomaly tests |

Concurrency labels are incomplete without isolation level, transaction boundaries, read visibility,
failure semantics, and progress guarantees.

### Distribution and replication

| Model | Placement/coordination | Compatibility caveats |
| --- | --- | --- |
| Standalone | one local engine | still requires a process/file ownership policy and durability definition |
| Primary/replica | one ordered writer, replicated followers | synchronous vs asynchronous acknowledgement changes durability and stale-read semantics |
| Consensus/Raft | replicated log with quorum-agreed order | state-machine determinism, snapshots, membership, and storage fsync order are correctness-critical |
| Sharded | key/record space partitioned across owners | routing, repartitioning, cross-shard operations, and global constraints change the logical contract |
| Shared-nothing style | nodes own compute and storage partitions | often combines sharding and replication; failure/rebalance behavior must be named, not implied |

Raft is not a storage-engine checkbox: its own durable log, state-machine application, snapshotting, and
acknowledgement rules interact with the local engine.

## Compatibility and capability status

Status is explicit and intentionally sparse:

| Combination/capability | Status | Evidence or reason |
| --- | --- | --- |
| Binary KV + in-memory map + caller serialization + standalone | Implemented oracle | `db-storage-memory`; deterministic semantic tests |
| Binary KV + append log + caller serialization + standalone | Implemented candidate | `db-storage-log`; replay, checksum, corruption, prefix-interruption, and differential tests |
| Ordered KV range scan on append log | Not exposed | the replay `BTreeMap` is recovery state, not a measured on-disk ordered access path |
| Binary KV + B+ tree + standalone | Common persistent semantics + structural amplification evidence implemented | `db-storage-btree` implements the full 4 KiB-key/1 MiB-value point/range contract, deterministic fault recovery, and the common amplification-report trait. Hand-computable traces measure logical validated-page accesses (including cache hits), synchronized data-page writes, and retained committed data-page space including reusable COW history. Read work is explicitly tagged `btree_page_access`; it is not presented as device I/O. Physical compaction and controlled-host measurements remain deferred; shared generated traces now exercise this engine against the LSM with exact logical-outcome equality |
| Binary KV + LSM + standalone | Persistent correctness + common structural amplification evidence; not yet a performance candidate | `db-storage-lsm` implements the common point/reopen/range contract, crash-safe WAL/SSTable/Manifest-v5 full-set compaction, deterministic old-or-new fault recovery, and multi-cycle differential compaction tests. Its proven raw counters now feed the common amplification report: point reads are tagged `lsm_sstable_consult`, range work is tagged `lsm_sstable_version_decoded`, data writes cover WAL records plus flush/compaction SSTable output, and primary structure is authoritative SSTable bytes. Generalized levels, snapshot/replication-aware GC, device-level telemetry, controlled-host measurements, and comparable performance evidence remain deferred; shared generated traces and exact B+ tree outcome comparison are implemented |
| Relational/document/wide-column/graph/time-series models | Deferred | storage comparison must first be trustworthy |
| 2PL/MVCC/OCC/serializable concurrency | Deferred | transactions and anomaly suites do not yet exist |
| Replication/Raft/sharding/shared-nothing | Deferred | local persistence and crash methodology must mature first |

No deferred cell has an empty crate or a claimed benchmark result.

## What is approximately orthogonal

At the level of an abstract state machine, a logical KV `GET` can be implemented by a tree, LSM,
hash table, or log-derived index. A standalone implementation can later become a deterministic state
machine behind replication. Workload description and outcome comparison should therefore remain
separate from engine internals.

This separation is conditional. It is useful only while common semantics do not smuggle in an
engine-specific operation, durability mode, ordering rule, or consistency level.

## Where dimensions leak into one another

| Coupling | Why the boundary leaks | Experimental consequence |
| --- | --- | --- |
| Model ↔ storage | queries, adjacency, clustering, projection, and partial updates demand different locality | compare only operations with identical results and supported access paths |
| Storage ↔ concurrency | page latches, key-range locks, immutable runs, version placement, and reclamation differ | concurrency cannot be a wrapper with no physical-design changes |
| Concurrency ↔ replication | commit timestamps/lock ownership/validation must align with replicated order and failures | specify the authoritative commit point and retry semantics |
| Storage ↔ replication | WAL/log ordering, snapshots, compaction, and follower installation affect acknowledged durability | measure and fault-inject the combined write path |
| Model ↔ distribution | joins, graph cuts, series partitions, and document locality determine cross-node operations | placement and repartitioning are part of model-level behavior |
| Durability ↔ performance | sync, batching, checksums, and recovery policy alter both semantics and cost | durability mode is controlled input, never a hidden tuning difference |

## Current platform assumptions

The baseline uses Rust standard regular-file I/O, explicit little-endian fields, and no memory maps or
POSIX-only syscall. CI therefore tests Linux, macOS, and Windows. Filesystem and device guarantees for
`sync_data`/`sync_all` differ; experiments must record them. The append engine assumes one caller and
one process owns a file. The B+ tree likewise provides no cross-process exclusion. Its current mutation protocol avoids
in-place replacement entirely: copy-on-write pages are synchronized before a mirrored root-pointer
transition publishes the new tree. Pages outside current-root reachability are recycled only on later
mutations, after they are proven unreachable; physical file compaction is not implemented. Opening the same persistent file concurrently is outside the capability
contract, not a supported concurrency mode.

The LSM has the same single-owner restriction. It uses canonical numbered WAL segments selected by the
manifest, immutable SSTables (v2 embedding a Bloom filter), and mirrored CURRENT publication inside one
engine directory; undeclared directory entries fail closed. Its tombstone-GC proof depends on consuming
every authoritative SSTable and on the absence of snapshots, concurrent compaction, and replication
history. Parent-directory durability at initial file creation is not provided by a portable
standard-library primitive.
