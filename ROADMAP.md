# Distributed Data Lab Roadmap

## Phase 0 — Governance bootstrap

- [x] initialize the umbrella repository;
- [x] define database/distributed-system boundaries and forbid fake integration claims;
- [x] record exact observed source mains and active implementation blockers;
- [x] add a machine-checked migration manifest and CI gate;
- [ ] import a source only after its implementation lane reaches a clean frozen checkpoint.

Current source state:

- `database-design-lab@35c226fb...` — **HOLD**, PR #128 active;
- `distributed-systems-lab@5cd1fa28...` — **HOLD**, PR #65 active;
- `tinydb-c@c0de1768...` — **HOLD**, Stage 1 stabilization PR #5 active.

No import count is a progress target. A safe no-op is preferable to importing moving source history.

## Phase 1 — History-preserving imports

For each stable source:

1. freeze exact source/umbrella SHAs;
2. require zero active implementation PRs;
3. confirm exact source CI and native platform requirements;
4. audit reachable history and repository hygiene;
5. use a non-squashed subtree import;
6. prove source ancestry and exact tree identity;
7. mirror source-equivalent CI from the umbrella path;
8. normal-merge only after exact candidate gates are green;
9. rerun exact merged-main verification.

Import order is decided by source stability and architecture value, not by project age or line count.

## Phase 2 — Explicit local data contracts

Potential work after at least two stable imports:

- [ ] identify a bounded common transaction/workload subset before any database differential claim;
- [ ] define exact snapshot/checkpoint/recovery artifact semantics where a real producer/consumer pair exists;
- [ ] preserve each engine's durable-format and failure semantics rather than forcing cosmetic unification.

## Phase 3 — Distributed state integration

Potential high-value edge:

- [ ] define a deterministic replicated state-machine boundary between `distributed-systems-lab` and exactly one local data engine;
- [ ] test command identity/order, replay, snapshot restore, leader/follower failure, and acknowledged durability at that boundary;
- [ ] make the consistency/durability claim no broader than the executable test.

No distributed-database claim exists until such a boundary is real.

## Phase 4 — Flagship checkpoint

Not complete until:

- multiple source histories are imported and exact merged-main CI is green;
- at least one non-trivial local-data or replicated-state cross-project edge is permanently executable;
- manifest/docs distinguish imported projects from verified integrations;
- no active migration debt is hidden by documentation.

## Non-goals

- merging independent databases into one codebase for aesthetics;
- calling consensus + storage a distributed database without a tested state-machine contract;
- pretending differing SQL/durability semantics are equivalent;
- importing a stale main while a large implementation PR owns the source;
- rewriting genuine authorship during consolidation.
