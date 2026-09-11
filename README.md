# Distributed Data Lab

A portfolio-oriented umbrella for database systems, durable storage, query execution, replication, consensus, recovery, and distributed-state correctness.

This repository is **not** a monolithic database product and does not imply that its source projects already share a storage format, SQL dialect, replication protocol, or consistency model. Sources enter only through history-preserving migration after a clean freeze. Cross-project edges are claimed only when an executable contract proves them.

## Live source/import map

| Project | Primary role | Status |
| --- | --- | --- |
| [database-design-lab](projects/database-design-lab) | relational engine, indexes, durability/recovery | **IMPORTED / VERIFIED** — source `fba592f2...`; exact merged-main gates green |
| [distributed-systems-lab](projects/distributed-systems-lab) | consensus, replication, failure/reordering correctness | **IMPORTED / VERIFIED** — source `f1638f91...`; exact migration and merged-main gates green |
| [tinydb-c](projects/tinydb-c) | C SQL database, pager/WAL/B+ tree/query engine | **IMPORTED / VERIFIED** — source `81d98f75...`; exact migration and merged-main gates green |

`database-design-lab` was imported by a genuine non-squashed subtree operation. Bootstrap run `34064926421` rechecked the exact source head and zero open PRs, scanned all **131 reachable source commits**, preserved exactly four historical GitHub Actions bot co-author trailers, rejected all other configured attribution markers, and proved exact source tree `4c5bb112...` equals `projects/database-design-lab` after import. Subtree commit `70ead65382d116f09ab95c7f195cdbdde06b0e69` has exact source `fba592f25225621f83931662565120c43ede1885` as its second parent.

The temporary write-capable bootstrap workflows have been removed. Permanent **read-only** history/tree/provenance and source-equivalent gates protect every imported subtree, and all three are green on their exact migration PR heads and exact merged `main` commits. The umbrella is now at the complete verified **3/3** import checkpoint.

`distributed-systems-lab` was independently imported at source `f1638f9121ee550b688a3f6bf9ab03df369139dc`. Bootstrap run `34332419886` rechecked unchanged live main and zero open PRs, audited all **110 reachable source commits**, found zero configured attribution markers, and proved tree `b816d1f0...` equals `projects/distributed-systems-lab`. Subtree commit `447a8c8493c1c612e9e9b8a66fd5c194a4200dd7` retains the exact source as its second parent.

`tinydb-c` Stage 1 PR #5 was repaired at exact head `85555e7...`, passed Ubuntu and Windows, and was normal-merged as `81d98f75b92a80b6e103af86913009b2ec7a6ec2`. Exact merged-main run `34334920828` is green and the source has zero open PRs. Bootstrap run `34335988505` audited all **1,370 reachable commits**, found zero configured attribution markers, and created subtree commit `d5ebf9c21279a7da452e20be5f20b82085893174` whose second parent is the exact source. Tree `e0c9006f...` equals `projects/tinydb-c` exactly. Migration PR #5 head `1547e9f...` passed all four umbrella workflows and normal merge `50008bb...` passed the same exact-main gates.

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

The broad product lines above remain future hypotheses. One bounded edge between
`distributed-systems-lab` and `database-design-lab` is now verified below, but it
does not turn the imported projects into a complete distributed database. The
`tinydb-c` line remains entirely future work.

## Verified cross-project integration

`integrations/raft-log-storage/` is the first bounded executable edge between two
imported projects. The real `distributed-systems-lab`
implementation elects a leader, commits a `Put`/`Delete` prefix through majority
replication, and applies it on two replicas. That exact applied prefix is projected
to the versioned workload accepted by the real `database-design-lab` append-log
engine. A second database process must reopen the durable log and return the same
state.

Evidence: PR #6 exact head `48616d7058047a12f4b569596dd613425a27e96f`
passed run `34570497138`; normal merge
`17239e702ca38945201382c7a1769e90f6f4f392` passed exact-main run
`34570574828`. The bounded exclusions are recorded in
`integrations/manifest.json`.

## Flagship checkpoint

The umbrella now has three history-preserved, independently verified imports and
one permanent, non-trivial cross-project executable edge. This satisfies the first
Distributed Data flagship checkpoint. It remains a laboratory checkpoint, not a
production distributed-database claim: acknowledgement-to-storage atomicity,
snapshots, failure recovery and stronger replicated-durability contracts remain
future milestones.

## Migration invariants

1. Recheck exact source `main`, open PRs, recent commits and CI immediately before every freeze/import.
2. Any active implementation/stabilization PR puts that source on HOLD.
3. Preserve source history with non-squashed subtree migration; never substitute a ZIP/current-tree copy.
4. Audit reachable history and repository hygiene; preserve genuine historical provenance rather than rewriting it.
5. Prove source tree equals imported subtree at the frozen SHA.
6. Run source-equivalent native CI from the umbrella path.
7. Remove temporary write-capable migration machinery before the actual import PR.
8. Normal-merge migration PRs so source ancestry stays reachable.
9. Keep source repositories available.
10. Never claim distributed/database interoperability without executable proof.

`projects/manifest.json` is the machine-checked source ledger. See [ROADMAP.md](ROADMAP.md) and [docs/MIGRATION.md](docs/MIGRATION.md).
