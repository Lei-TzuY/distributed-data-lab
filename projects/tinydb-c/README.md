# TinyDB C

An educational relational database engine written in C. TinyDB implements a disk-backed B+ tree, write-ahead logging, transactions, secondary indexes, a buffer pool, a reusable engine API, multi-root table execution, and a growing SQL layer in one compact codebase.

> **Goal:** explore how storage, recovery, indexing, catalogs and query execution fit together — not replace SQLite or claim production-level performance.

## Verification at a glance

The current PR head is exercised by **78 automated suites** on both Ubuntu and Windows through GitHub Actions.

Coverage includes:

- persistent B+ tree storage with split / merge behavior
- growable heap-backed Pager metadata instead of a fixed page-table ceiling
- no-steal buffer eviction: uncommitted dirty pages are never evicted into the main database file
- WAL recovery, explicit checkpointing, rollback and savepoints
- secondary and composite indexes
- page checksums and integrity verification
- reusable `tinydb_core` engine facade tested without the REPL
- persistent multi-table schema catalog and independent B+ tree roots
- cross-root transactions, savepoints and hard-crash atomicity
- primary-key cross-root INNER JOIN execution and profiling
- catalog-aware `PRAGMA table_info(...)` / `PRAGMA index_list(...)`
- per-root `.schema`, `.stats`, `.btree` and `.check` diagnostics
- native benchmark runner with text and JSON output
- deterministic repeated crash/recovery stress runner
- regression coverage that writes and reopens more than 4096 database pages
- large uncommitted-transaction crash coverage that forces eviction beyond the 16-frame buffer pool
- offline binary page/checksum inspector that does not depend on opening the DB through TinyDB
- `EXPLAIN ANALYZE` query profiling with execution time and buffer-pool deltas

Run the complete suite with:

```sh
python tests/run_all.py
```

## Architecture

```text
                REPL / embedded caller
                        |
                        v
                 tinydb_core API
             tinydb_execute_sql(...)
                        |
              +---------+----------+
              |                    |
              v                    v
        parser / planner      schema catalog
              |               + root routing
              v                    |
       query execution <------------+
              |
      +-------+---------+
      |                 |
      v                 v
  secondary         transactions
   indexes          / savepoints
      |                 |
      +--------+--------+
               v
          buffer pool
          (16 frames)
               |
       +-------+--------+
       |                |
       v                v
 no-steal spill      clean pages
       |                |
       +-------+--------+
               v
       per-table B+ trees
               |
       +-------+--------+
       |                |
       v                v
      WAL            DB pages
```

The interesting part of the project is the interaction between these layers. A feature is only useful if the parser, catalog, storage engine, transaction path and recovery behavior agree on the same state.

## Reusable engine API

The native implementation is exposed through the `tinydb_core` static library. The REPL is now a thin user-interface layer over the same API that embedded callers can use.

```c
#include "engine.h"

TinyDB* db = tinydb_open("app.db");
if (db == NULL) {
    return 1;
}

TinyDBSqlResult result;
TinyDBSqlStatus status = tinydb_execute_sql(
    db,
    "SELECT * FROM users WHERE id = 42;",
    &result
);

if (status != TINYDB_SQL_SUCCESS) {
    fprintf(stderr, "%s\n", result.message);
}

tinydb_close(db);
```

The facade centralizes:

- parsing
- multi-table root resolution
- policy guards for unsupported destructive paths
- cross-root JOIN dispatch
- `EXPLAIN ANALYZE`
- catalog-aware PRAGMAs
- schema-catalog persistence
- VM execution and result status mapping

`tests/engine_api_probe.c` links directly against `tinydb_core`; it does not use stdin or the REPL. The probe creates an additional table, forces its non-zero B+ tree root to split, checks per-root statistics and integrity, executes a cross-root JOIN, closes the database, reopens it, and verifies that the catalog root and rows persisted.

## Multi-table storage and routing

TinyDB now persists a schema catalog in sidecar files:

```text
<database>.schema
<database>.schema.wal
```

Each catalog table has its own `root_page_num`. Direct DML is routed to that root instead of implicitly using page 0:

```sql
CREATE TABLE archive (
    id INT,
    username VARCHAR,
    email VARCHAR
);

INSERT INTO users VALUES (1, 'main', 'main@example.com');
INSERT INTO archive VALUES (1, 'archived', 'archive@example.com');

SELECT * FROM users WHERE id = 1;
SELECT * FROM archive WHERE id = 1;
```

The same primary key can therefore exist independently in two table roots.

Current executable secondary tables use the physical `Row` layout already implemented by TinyDB: `id`, `username`, `email`. The catalog can describe other schemas for metadata/DDL experiments, but arbitrary physical row layouts are **not yet implemented**. DML against an incompatible schema fails closed instead of reinterpreting bytes incorrectly.

### Multi-root transactions

Transactions operate over the shared Pager/WAL rather than one table root. Regression coverage verifies:

- writes to `users` and another root in the same `BEGIN ... ROLLBACK`
- writes to both roots in one `BEGIN ... COMMIT`
- savepoint rollback across roots
- reopen durability after commit
- a hard kill during one large uncommitted transaction that dirties hundreds of rows across both roots

After that crash, both tables must retain only their previously committed rows and `PRAGMA integrity_check` must pass.

### Cross-root JOIN

The currently implemented cross-root JOIN path is intentionally narrow and measurable:

```sql
SELECT *
FROM users
JOIN archive
ON users.id = archive.id;
```

Execution uses a sequential scan of the left B+ tree and a primary-key B+ tree lookup on the right for every left key. It is not the old same-root self-scan behavior.

```text
left table scan
      |
      v
 left_row.id
      |
      v
right B+ tree PK lookup
      |
      v
 matching joined row
```

`LIMIT` and `OFFSET` are supported on this path. More general cross-root JOIN predicates remain unsupported and fail closed rather than silently scanning the wrong root.

## Cross-root EXPLAIN / EXPLAIN ANALYZE

Plain `EXPLAIN` reports the join strategy without running the query:

```sql
EXPLAIN
SELECT * FROM users
JOIN archive ON users.id = archive.id;
```

Representative plan:

```text
PLAN: CROSS-ROOT PRIMARY KEY NESTED LOOP JOIN
      LEFT: FULL TABLE SCAN users (root page 0)
      RIGHT: PRIMARY KEY LOOKUP archive.id (root page ...)
```

`EXPLAIN ANALYZE` executes that same path and reports measured buffer-pool deltas:

```sql
EXPLAIN ANALYZE
SELECT * FROM users
JOIN archive ON users.id = archive.id;
```

```text
QUERY PLAN
PLAN: CROSS-ROOT PRIMARY KEY NESTED LOOP JOIN
      LEFT: FULL TABLE SCAN users (root page 0)
      RIGHT: PRIMARY KEY LOOKUP archive.id (root page ...)
ACTUAL RESULT
...
ANALYZE: execution_time_ms=... cache_hits=... cache_misses=... evictions=... page_accesses=...
```

## Catalog-aware inspection

Table-scoped PRAGMAs now resolve the requested catalog table instead of always reporting `users`:

```sql
PRAGMA table_info(users);
PRAGMA table_info(archive);
PRAGMA index_list(users);
PRAGMA index_list(archive);
```

The parameterless forms remain compatible with the historical built-in table:

```sql
PRAGMA table_info;
PRAGMA index_list;
```

For multi-table databases, `PRAGMA integrity_check` walks every catalog B+ tree root and rejects duplicate root assignments in addition to checking each tree's structure.

The REPL diagnostics are also root-aware:

```text
.tables                 list catalog tables
.schema [table]         inspect one or all schemas
.btree [table]          visualize a selected B+ tree root
.stats [table]          global totals or one root's rows/pages/height
.check [table|all]      validate one or all catalog roots
.page <n>               inspect a physical page
.buffer_pool / .cache   inspect cache state
.checkpoint              checkpoint WAL state
```

## Core subsystems

### Storage engine

- disk-backed B+ tree
- internal / leaf node splitting, borrowing and merging
- page-level checksums
- physical integrity checking
- 16-frame buffer-pool LRU caching
- heap-backed page table, dirty map and free-page list that grow on demand
- metadata capacity starts at `PAGER_INITIAL_CAPACITY` (64 pages by default) and doubles as required
- 32-bit page numbers remain the logical page identifier; the old `TABLE_MAX_PAGES=4096` value is no longer the Pager's hard page ceiling
- 64-bit file offsets for database positioning and truncation
- per-root page inspection and B+ tree visualization

`TABLE_MAX_PAGES` is retained as a legacy compatibility/sanity constant for older auxiliary-file validation. Code that needs the current Pager allocation should use `pager_metadata_capacity()` rather than treating `TABLE_MAX_PAGES` as database capacity.

### Transactions and recovery

- write-ahead logging (WAL)
- commit / rollback
- savepoints
- log replay for recovery
- manual checkpointing
- no-steal dirty-page spill when an uncommitted page is evicted from the 16-frame buffer pool
- committed shadow pages keep post-WAL state visible before checkpoint without writing uncommitted state to the main file
- WAL transaction records include the final logical page count so shrink/truncate state can be recovered after a crash
- backward recovery support for the repository's previous WAL record layout
- exact transaction free-page-list snapshot/restore on rollback
- deterministic crash/recovery stress testing for committed and uncommitted transactions

Single-table backup / compaction workflows such as `VACUUM` and `VACUUM INTO` remain available. They are currently blocked once a database has multiple table roots, because the old compaction implementation assumes one root and `VACUUM INTO` does not yet package all catalog sidecars.

### Indexing and catalog

- primary-key B+ tree access
- secondary indexes
- generic secondary indexes
- composite indexes
- persisted index metadata
- persisted table/view schema catalog
- `sqlite_master`-style catalog inspection
- automatic index selection where supported by the current query path
- catalog-aware `PRAGMA index_list(table)`

Secondary indexes on non-primary table roots are currently blocked until their persistence and routing path is table-scoped end to end.

### Query layer

The SQL layer includes common DDL / DML operations plus selected higher-level features such as:

- `CREATE TABLE`, `INSERT`, `UPDATE`, `DELETE`, `SELECT`
- filtering predicates
- `ORDER BY`, `GROUP BY`, `HAVING`, `LIMIT` / `OFFSET`
- scalar subqueries and `IN` / `EXISTS`
- views and common table expressions
- `UNION` / `UNION ALL`
- window functions such as `ROW_NUMBER()`
- `EXPLAIN` and `EXPLAIN ANALYZE`
- a small full-text-search path
- built-in string, math and system functions

This is intentionally a teaching-oriented SQL surface rather than a claim of SQL-standard completeness.

## Build

CMake builds the reusable `tinydb_core` static library and separate executables for the REPL, benchmark, pager-growth probe and direct engine-API probe.

```sh
cmake -S . -B build
cmake --build build
```

Example REPL launch on Windows:

```powershell
.\build\Debug\tinydb.exe my_database.db
```

The exact executable path depends on the generator / platform used by CMake.

## Benchmark

`tinydb_bench` provides a reproducible storage-engine workload instead of relying on unverified performance claims. It performs one transactional insert batch followed by randomized primary-key lookups, verifies every lookup, and reports throughput plus page / buffer-pool statistics.

Unix-like build layout:

```sh
./build/tinydb_bench benchmark.db 5000 20000
```

Windows multi-config build layout:

```powershell
.\build\Debug\tinydb_bench.exe benchmark.db 5000 20000
```

Arguments are `DATABASE ROWS LOOKUPS`. The benchmark refuses to overwrite an existing database path so a previous result cannot silently contaminate a new run.

Example text output fields include:

```text
insert: inserted=5000 seconds=... rows_per_sec=...
lookup: requested=20000 lookup_hits=20000 seconds=... lookups_per_sec=...
storage: pages=... leaf_pages=... internal_pages=... rows=5000 metadata_capacity=...
cache_lookup_phase: hits=... misses=... evictions=...
BENCHMARK_OK
```

Machine-readable output is available with `--json`:

```sh
./build/tinydb_bench benchmark.db 5000 20000 --json
```

The JSON payload includes workload counts, throughput, storage-page counts, lookup cache metrics, `dynamic_page_table`, `initial_metadata_capacity`, current `metadata_capacity`, the retained `legacy_page_ceiling`, and an `ok` correctness flag. This is intended for automated before/after regression analysis.

Use the benchmark for before/after comparisons on the same machine, compiler, build type, dataset size and storage device. Cross-machine numbers are not directly comparable without controlling those variables.

## Query profiling

For a normal single-table query:

```sql
EXPLAIN SELECT * FROM users WHERE id = 42;
EXPLAIN ANALYZE SELECT * FROM users WHERE id = 42;
```

Representative analyzed output:

```text
QUERY PLAN
PLAN: PRIMARY KEY LOOKUP (id = 42)
ACTUAL RESULT
(42, user42, u42@example.com)
ANALYZE: execution_time_ms=... cache_hits=... cache_misses=... evictions=... page_accesses=...
```

The counters are execution deltas, not process-lifetime totals. `page_accesses` is the sum of hits and misses during the measured query. Timing uses the C runtime clock and should be treated as local diagnostic evidence rather than a cross-machine benchmark.

## Pager growth and eviction regression tests

`tinydb_pager_growth_probe` writes pages `0..4128`, deliberately crossing the historical 4096-page boundary while a 16-frame buffer pool repeatedly evicts pages. It commits, checkpoints, reopens the database, and verifies markers on both sides of the old boundary. The Python wrapper also checks that the physical file contains all 4129 pages.

`test_large_transaction_crash.py` seeds committed data, starts one explicit transaction, inserts 700 additional rows, then hard-kills the process before `COMMIT`. Reopening must show only the previously committed rows and still pass `PRAGMA integrity_check`.

`test_multi_table_crash.py` repeats the same no-steal invariant while alternating hundreds of writes between two independent table roots in one transaction. Neither root may expose a ghost row after the kill.

## Crash / recovery stress runner

`tools/recovery_stress.py` repeatedly injects hard process kills against the same database. Every round has two phases:

1. `BEGIN` + inserts + `COMMIT`, followed by an immediate kill without graceful `.exit`; all committed rows must survive recovery.
2. `BEGIN` + inserts, followed by a kill before `COMMIT`; none of those rows may become visible.

After each recovery it reopens the database, checks the complete expected/forbidden row set, and runs `PRAGMA integrity_check`. The workload uses a fixed seed by default so failures are reproducible.

```sh
python tools/recovery_stress.py --iterations 20 --rows-per-round 2 --seed 1337
```

## Offline page inspector

`tools/page_inspect.py` reads a TinyDB file directly without going through the pager. This is useful for damaged-database inspection because it can still report page metadata even when TinyDB itself refuses to open the file.

It currently checks:

- file/page alignment
- per-page FNV-1a checksum
- root/internal/leaf page type and header fields
- leaf key ordering and duplicate keys
- internal separator ordering
- basic leaf sibling and internal child pointer bounds
- total rows, page counts and checksum failures

Text summary:

```sh
python tools/page_inspect.py my_database.db --pages --strict
```

Machine-readable report:

```sh
python tools/page_inspect.py my_database.db --json --strict
```

`--strict` returns a non-zero exit code when the inspector finds a checksum, header, key-order or pointer-range problem.

## Scope and limitations

TinyDB is an educational database engine. The project is useful for studying implementation tradeoffs, but it should not be described as a drop-in SQLite replacement or a production database.

In particular:

- SQL coverage is intentionally incomplete.
- the catalog supports multiple schemas, but executable additional tables currently require the fixed `id / username / email` physical Row shape.
- arbitrary variable physical row formats and row migration are not implemented yet.
- cross-root JOIN currently supports the primary-key `id = id` path; arbitrary join predicates, cross-root UNION and cross-root nested subqueries fail closed.
- secondary indexes on non-primary roots are not enabled yet.
- multi-table `VACUUM`, `VACUUM INTO` and prepared execution are blocked until those paths preserve/root-route every relevant sidecar and bound statement.
- the Pager grows its in-memory metadata on demand, but page identifiers are still 32-bit and mature databases use more sophisticated allocation/free-space structures.
- the no-steal implementation uses heap-backed page shadows for educational clarity; it is not intended to model the memory efficiency of a production buffer manager.
- durability / crash semantics should be evaluated against the tests and documented recovery paths, not inferred from feature names alone.
- performance claims require benchmarks against defined workloads; results depend on hardware, compiler settings, build type and workload.
- the offline inspector performs structural sanity checks but is not a formal proof that every possible B+ tree invariant holds.
- concurrency, locking and production-hardening expectations are different from mature database systems.

The strongest signal in this repository is the implementation + testable invariants, not the number of supported SQL keywords.

## License

MIT — see [LICENSE](LICENSE).
