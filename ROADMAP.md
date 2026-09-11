# Distributed Data Lab Roadmap

## Phase 0 — Governance bootstrap

- [x] initialize the umbrella repository;
- [x] define database/distributed-system boundaries and forbid fake integration claims;
- [x] add a machine-checked migration manifest and CI gate;
- [x] re-run live source preflight after implementation lanes moved;
- [x] promote stable `database-design-lab` and `distributed-systems-lab` checkpoints to READY FOR IMPORT;
- [x] repair and merge `tinydb-c` Stage 1 PR #5 only after exact Ubuntu/Windows CI, then re-freeze its clean source main.

## Phase 1 — History-preserving imports

### `database-design-lab`

- [x] freeze exact source `fba592f25225621f83931662565120c43ede1885` after zero-open-PR/live-main recheck;
- [x] verify exact source CI `34061539532` across quality, Ubuntu/macOS/Windows tests, and Rust 1.85;
- [x] run migration-time complete provenance/hygiene audit over all 131 reachable source commits;
- [x] preserve exactly four historical `github-actions[bot]` co-author trailers and reject other configured attribution markers;
- [x] perform non-squashed subtree import as `70ead65382d116f09ab95c7f195cdbdde06b0e69` with the exact source SHA as second parent;
- [x] prove source ancestry and tree `4c5bb1129c4b145923dafa2003412bcda8c0c806` equals `projects/database-design-lab`;
- [x] remove temporary write-capable bootstrap workflow;
- [x] add permanent read-only source-equivalent migration CI;
- [x] pass exact migration-PR head gates;
- [x] normal-merge and pass exact merged-main gates (`9c1de1a...`, runs `34065376109` and `34065376168`).

### `distributed-systems-lab`

- [x] re-freeze exact source `f1638f9121ee550b688a3f6bf9ab03df369139dc` after #107 merged, zero open PRs and exact-main CI `34318592362` success;
- [x] recheck exact source main and zero-open-PR state immediately before import;
- [x] perform the independent non-squashed import as `447a8c8493c1c612e9e9b8a66fd5c194a4200dd7` and prove exact tree `b816d1f0a658a611887b5d4fe17f05a5499e9bd2`;
- [x] add permanent Ubuntu/macOS/Windows × Python 3.11/3.13 Ruff/pytest gates;
- [x] normal-merge as `b5f4f364cb60d8e74cfb9ac7115233e03ef3062e` and re-verify exact merged main (runs `34333643232`, `34333643233`, and `34333643318`).

### `tinydb-c`

- [x] resolve all four review threads on PR #5 and pass exact-head Ubuntu/Windows CI at `85555e75710936ce5e6a8cb8ac873ddb29135170`;
- [x] normal-merge Stage 1 as `81d98f75b92a80b6e103af86913009b2ec7a6ec2` and pass exact merged-main CI `34334920828`;
- [x] require zero open PRs, freeze tree `e0c9006f316e6107f13845a3e5a1a0a94264b0e5`, and audit all 1,370 reachable commits;
- [x] perform non-squashed subtree import as `d5ebf9c21279a7da452e20be5f20b82085893174` with the exact source as second parent;
- [x] remove the write-capable bootstrap and add a permanent Ubuntu/Windows source-equivalent gate;
- [x] pass the exact migration PR head gates (`1547e9f...`, runs `34336374920`, `34336375001`, `34336374844`, `34336374982`);
- [x] normal-merge as `50008bb9b18af694ba050a692a808c544f1525f2` and pass exact merged-main gates (`34337504100`, `34337504096`, `34337504113`, `34337504168`).

Import count is not a goal. If a READY source opens a new implementation lane before import, it returns to HOLD.

## Phase 2 — Explicit local data contracts

Potential work after at least two verified imports:

- [ ] identify a bounded common transaction/workload subset before any database differential claim;
- [ ] define exact snapshot/checkpoint/recovery artifact semantics where a real producer/consumer pair exists;
- [ ] preserve each engine's durable-format and failure semantics rather than forcing cosmetic unification.

## Phase 3 — Distributed state integration

Highest-value candidate after `database-design-lab` and `distributed-systems-lab` are both exact-main verified imports:

- [x] verify and merge `integrations/raft-log-storage/`, which executes a deterministic Raft majority-committed `Put`/`Delete` prefix, applies it on two replicas, projects only that applied prefix into the database workload contract, and requires a second `db-lab` process to reopen the durable append log with equivalent state (PR #6 head `48616d7...`, run `34570497138`; merge `17239e7...`, exact-main run `34570574828`);
- [ ] extend the first bounded prefix contract to command identity/deduplication, snapshots, leader/follower failure, and an explicit acknowledgement-to-durability rule;
- [ ] permanently gate only the exact bounded consistency/durability claims that those later executable contracts prove.

The first bounded boundary is executable. This does not yet prove atomic
acknowledgement-to-storage durability, snapshot transport, failure recovery, or a
production distributed database.

## Phase 4 — Flagship checkpoint

**Complete at the first bounded flagship checkpoint:**

- [x] multiple source histories are imported and exact merged-main CI is green;
- [x] one non-trivial replicated-state/storage edge is permanently executable;
- [x] manifest/docs distinguish imported projects, verified integration and future hypotheses;
- [x] active deeper-integration debt remains explicit rather than hidden.

## Non-goals

- merging independent databases into one codebase for aesthetics;
- calling consensus + storage a distributed database without a tested state-machine contract;
- pretending differing SQL/durability semantics are equivalent;
- importing a stale main while an implementation PR owns the source;
- rewriting genuine authorship during consolidation.
