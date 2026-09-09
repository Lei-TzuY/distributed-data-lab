# Distributed Data Lab Roadmap

## Phase 0 — Governance bootstrap

- [x] initialize the umbrella repository;
- [x] define database/distributed-system boundaries and forbid fake integration claims;
- [x] add a machine-checked migration manifest and CI gate;
- [x] re-run live source preflight after implementation lanes moved;
- [x] promote stable `database-design-lab` and `distributed-systems-lab` checkpoints to READY FOR IMPORT;
- [x] keep `tinydb-c` on HOLD while its authoritative Stage 1 stabilization lane remains active.

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
- [ ] normal-merge and re-verify exact merged main.

### `tinydb-c`

- [ ] defer import until Stage 1 PR #5 is completed or explicitly superseded and a new clean preflight passes.

Import count is not a goal. If a READY source opens a new implementation lane before import, it returns to HOLD.

## Phase 2 — Explicit local data contracts

Potential work after at least two verified imports:

- [ ] identify a bounded common transaction/workload subset before any database differential claim;
- [ ] define exact snapshot/checkpoint/recovery artifact semantics where a real producer/consumer pair exists;
- [ ] preserve each engine's durable-format and failure semantics rather than forcing cosmetic unification.

## Phase 3 — Distributed state integration

Highest-value candidate after `database-design-lab` and `distributed-systems-lab` are both exact-main verified imports:

- [ ] define a deterministic replicated state-machine command/reply boundary between the consensus layer and the relational/storage engine;
- [ ] test command identity/order, replay, snapshot restore, leader/follower failure, and acknowledged durability;
- [ ] permanently gate the exact bounded consistency/durability claim.

No distributed-database claim exists until such a boundary is executable.

## Phase 4 — Flagship checkpoint

Not complete until:

- multiple source histories are imported and exact merged-main CI is green;
- at least one non-trivial local-data or replicated-state cross-project edge is permanently executable;
- manifest/docs distinguish READY, imported projects, and verified integrations;
- no active migration debt is hidden by documentation.

## Non-goals

- merging independent databases into one codebase for aesthetics;
- calling consensus + storage a distributed database without a tested state-machine contract;
- pretending differing SQL/durability semantics are equivalent;
- importing a stale main while an implementation PR owns the source;
- rewriting genuine authorship during consolidation.
