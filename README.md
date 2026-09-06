# Distributed Data Lab

A portfolio-oriented umbrella for database systems, durable storage, query execution, replication, consensus, recovery, and distributed-state correctness.

This repository is **not** a monolithic database product and does not imply that its source projects already share a storage format, SQL dialect, replication protocol, or consistency model. Sources enter only through history-preserving migration after a clean freeze. Cross-project edges are claimed only when an executable contract proves them.

## Phase 0 live source map

| Project | Primary role | Live status |
| --- | --- | --- |
| [database-design-lab](https://github.com/Lei-TzuY/database-design-lab) | relational engine, indexes, durability/recovery | **READY FOR IMPORT** — `fba592f2...`; exact-main CI `34061539532` 5/5 green |
| [distributed-systems-lab](https://github.com/Lei-TzuY/distributed-systems-lab) | consensus, replication, failure/reordering correctness | **READY FOR IMPORT** — `1837b0ec...`; exact-main six-cell CI `34064569580` green |
| [tinydb-c](https://github.com/Lei-TzuY/tinydb-c) | C SQL database, pager/WAL/B+ tree/query engine | **HOLD** — Stage 1 stabilization PR #5 active; observed main `c0de1768...` |

The first two sources have now crossed preflight: zero open implementation PRs, exact-main CI, repository-hygiene review, provenance review, and an explicit source-equivalent umbrella verification contract are recorded in `projects/manifest.json`. `tinydb-c` remains intentionally deferred; import count is not a reason to bypass its active Stage 1 lane.

No source is shown as imported yet. A READY checkpoint still requires a genuine non-squashed history-preserving migration, exact source-tree ↔ imported-subtree proof, umbrella CI, normal merge, and exact merged-main re-verification.

## Architectural story

```text
local data semantics / storage

 database-design-lab        tinydb-c
 relational/storage         SQL/pager/WAL/B+ tree
          \                   /
           \ future explicit /
            \ workload/state/
             \   contract   /
              ▼             ▼
          distributed-systems-lab
       consensus / replication / faults
```

The lines above are **future hypotheses, not verified integration edges**. A local database does not become a distributed database merely because a Raft-like library exists beside it, and two database engines are not interchangeable just because both support transactions or SQL-like operations.

## What would count as real integration?

Examples of acceptable future work include:

- a deterministic logical state-machine command/reply format used by a distributed consensus layer and one database engine, with crash/replay assertions;
- a bounded differential workload executed against `database-design-lab` and `tinydb-c` only for semantics both explicitly support;
- a snapshot/checkpoint artifact whose encoding, versioning, atomicity, and restore semantics are named and tested across a real producer/consumer boundary;
- fault/reordering tests proving that acknowledged replicated operations satisfy an explicitly stated durability/consistency contract.

A shared README, matching nouns such as WAL/transaction/replication, or copying data between directories does not count.

## Migration invariants

1. Recheck exact source `main`, open PRs, recent commits and CI immediately before every freeze/import.
2. Any active implementation/stabilization PR puts that source on HOLD.
3. Preserve source history with non-squashed subtree migration; never substitute a ZIP/current-tree copy.
4. Audit reachable history and repository hygiene before import; preserve genuine historical provenance rather than rewriting it.
5. Prove source tree equals imported subtree at the frozen SHA.
6. Run source-equivalent native CI from the umbrella path.
7. Remove temporary write-capable migration machinery before the actual import PR.
8. Normal-merge migration PRs so source ancestry stays reachable.
9. Keep source repositories available.
10. Never claim distributed/database interoperability without executable proof.

`projects/manifest.json` is the machine-checked source ledger. See [ROADMAP.md](ROADMAP.md) and [docs/MIGRATION.md](docs/MIGRATION.md).
