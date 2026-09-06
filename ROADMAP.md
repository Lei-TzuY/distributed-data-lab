# Distributed Data Lab Roadmap

## Phase 0 — Governance bootstrap

- [x] initialize the umbrella repository;
- [x] define database/distributed-system boundaries and forbid fake integration claims;
- [x] record exact observed source mains and implementation blockers;
- [x] add a machine-checked migration manifest and CI gate;
- [x] re-run live preflight after source implementation lanes moved;
- [x] promote stable `database-design-lab` and `distributed-systems-lab` checkpoints to READY FOR IMPORT;
- [ ] keep `tinydb-c` on HOLD until its authoritative Stage 1 stabilization lane settles.

Current source state:

- `database-design-lab@fba592f25225621f83931662565120c43ede1885` — **READY FOR IMPORT**; zero open PRs, source CI `34061539532` 5/5 green, exact source tree `4c5bb112...`, four historical GitHub Actions bot co-author trailers preserved as a legacy provenance exception;
- `distributed-systems-lab@1837b0ece467e3dff598643768b91543acc1350a` — **READY FOR IMPORT** after #82 merged; zero open PRs, source CI `34064569580` green across Ubuntu/macOS/Windows × Python 3.11/3.13, exact source tree `7f54c63f...`, configured attribution markers zero;
- `tinydb-c@c0de1768ae3080ec6d12796f82e5abf5a27be89f` — **HOLD**, Stage 1 stabilization PR #5 active.

READY is not IMPORTED. No project crosses that boundary until source ancestry and exact tree identity are genuinely preserved in the umbrella and source-equivalent CI passes from the imported path.

## Phase 1 — History-preserving imports

Next executable migration sequence:

1. [ ] recheck `database-design-lab` live source/main/open PR immediately before import;
2. [ ] non-squashed import `database-design-lab` at the exact READY SHA;
3. [ ] prove frozen source ancestry and exact tree identity;
4. [ ] mirror source quality + three-OS + Rust 1.85 gates from the umbrella path;
5. [ ] normal-merge and re-verify exact merged main;
6. [ ] repeat the same procedure independently for `distributed-systems-lab` using its six-cell Python matrix;
7. [ ] defer `tinydb-c` until PR #5 is completed or explicitly superseded and a new clean preflight passes.

Import order is decided by source stability and architecture value, not by project age or line count. If a READY source opens a new implementation PR before import, it returns to HOLD.

## Phase 2 — Explicit local data contracts

Potential work after at least two stable imports:

- [ ] identify a bounded common transaction/workload subset before any database differential claim;
- [ ] define exact snapshot/checkpoint/recovery artifact semantics where a real producer/consumer pair exists;
- [ ] preserve each engine's durable-format and failure semantics rather than forcing cosmetic unification.

## Phase 3 — Distributed state integration

Highest-value candidate edge after the two READY imports are actually verified:

- [ ] define a deterministic replicated state-machine command/reply boundary between `distributed-systems-lab` and `database-design-lab`;
- [ ] test command identity/order, replay, snapshot restore, leader/follower failure, and acknowledged durability at that boundary;
- [ ] make the consistency/durability claim no broader than the executable test.

No distributed-database claim exists until such a boundary is real.

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
- importing a stale main while a large implementation PR owns the source;
- rewriting genuine authorship during consolidation.
