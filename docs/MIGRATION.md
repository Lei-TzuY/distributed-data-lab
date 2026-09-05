# Distributed Data Lab Migration Protocol & Ledger

This document records migration governance and live preflight state for `distributed-data-lab`.

## Status vocabulary

- **PRE-FLIGHT** — candidate selected but not frozen.
- **READY FOR IMPORT** — exact source main, open-PR, CI, history and hygiene gates are clean.
- **HOLD** — active implementation/stabilization or another blocker prevents a safe freeze.
- **IMPORTED / VERIFIED** — non-squashed source ancestry, exact tree identity and source-equivalent umbrella CI are all proven.
- **INTEGRATION VERIFIED** — an executable cross-project data/distributed contract is permanently tested.

## Phase 0 source ledger — 2026-09-05

| Project | Observed main | Active PR | Status |
| --- | --- | --- | --- |
| `database-design-lab` | `35c226fbdfd8334f62f6a0a9fdeef39bc1fbc0c3` | #128 head `681bfad427928b35a2bbd491406f1292f6226bf2` | **HOLD** |
| `distributed-systems-lab` | `5cd1fa28fee3bcf8f1e6742b7f1033f2e7365828` | #65 head `910561b8ec298f23567133802c82db4bf94c5126` | **HOLD** |
| `tinydb-c` | `c0de1768ae3080ec6d12796f82e5abf5a27be89f` | #5 head `c22699d8877bb9bb109c670a67d225e1285623d4` | **HOLD** |

### Why every source is on HOLD

`database-design-lab` PR #128 is actively maintaining secondary-index state across relational commits. Freezing current main would omit the implementation currently defining the next source checkpoint.

`distributed-systems-lab` PR #65 is hardening AppendEntries attempt correlation so stale same-term success responses cannot advance newer probes. This is active consensus-correctness work and must settle before a freeze.

`tinydb-c` PR #5 is the authoritative Stage 1 stabilization lane. It contains 1,359 commits and 565 changed files relative to current main and explicitly defines storage/recovery/planner acceptance gates. Importing old main while that checkpoint is unresolved would knowingly produce a stale umbrella snapshot.

## Preflight gate

Immediately before any future import:

1. re-read exact source `main` and umbrella `main`;
2. require zero active implementation/stabilization PRs for that source;
3. confirm exact source CI required by the repository;
4. inspect recent commits and declared stability/checkpoint documents;
5. audit reachable history for configured attribution policy;
6. inspect repository hygiene, generated artifacts, caches, binaries, secrets, nested repositories and platform/hardware requirements;
7. define source-equivalent umbrella CI before migration;
8. freeze only the exact source commit that passed those gates.

## History-preserving procedure

A selected source commit must remain reachable from umbrella history through a non-squashed subtree operation.

```bash
git remote add source-project https://github.com/Lei-TzuY/<project>.git
git fetch source-project --tags
git subtree add --prefix=projects/<project> source-project <frozen-sha>
```

A ZIP, archive, copied worktree or squashed snapshot does not satisfy migration history requirements. Temporary write-capable bootstrap machinery, if required to bridge repository ancestry, must fail closed on source drift/open PRs and be removed before the actual migration PR is merged.

## Integration evidence rule

Importing database and consensus repositories into one umbrella does **not** create a distributed database.

A verified edge must name and exercise a real boundary, such as:

- deterministic state-machine command/reply encoding;
- transaction/workload semantics explicitly supported by both compared engines;
- versioned snapshot/checkpoint format and restore semantics;
- replicated acknowledgement/durability conditions under crash, delay and response reordering.

The executable regression must constrain the claim to exactly what it proves.

## Original repository policy

Source repositories remain available. No source is archived, deleted, force-rewritten or cosmetically re-authored as part of routine consolidation.
