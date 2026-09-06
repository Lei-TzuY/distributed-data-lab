# Distributed Data Lab Migration Protocol & Ledger

This document records migration governance and live preflight state for `distributed-data-lab`.

## Status vocabulary

- **PRE-FLIGHT** — candidate selected but not frozen.
- **READY FOR IMPORT** — exact source main, open-PR, CI, history/provenance, hygiene, and source-equivalent umbrella-gate definitions are clean.
- **HOLD** — active implementation/stabilization or another blocker prevents a safe freeze.
- **IMPORTED / VERIFIED** — non-squashed source ancestry, exact tree identity and source-equivalent umbrella CI are all proven.
- **INTEGRATION VERIFIED** — an executable cross-project data/distributed contract is permanently tested.

## Live source ledger — 2026-09-07

| Project | Exact observed main | Open implementation lane | Status |
| --- | --- | --- | --- |
| `database-design-lab` | `fba592f25225621f83931662565120c43ede1885` | none | **READY FOR IMPORT** |
| `distributed-systems-lab` | `1837b0ece467e3dff598643768b91543acc1350a` | none | **READY FOR IMPORT** |
| `tinydb-c` | `c0de1768ae3080ec6d12796f82e5abf5a27be89f` | #5 head `c22699d8877bb9bb109c670a67d225e1285623d4` | **HOLD** |

The previous Phase 0 HOLD entries for database PR #128 and distributed-systems PR #65 are historical preflight observations and are superseded by the live checks below. Source state is always re-read rather than assumed from this document.

## READY candidate 1 — `database-design-lab`

Selected preflight candidate: `fba592f25225621f83931662565120c43ede1885`, tree `4c5bb1129c4b145923dafa2003412bcda8c0c806`.

### Live-state and CI evidence

- zero open implementation PRs at the 2026-09-07 preflight;
- exact-main CI `34061539532`: **success**;
- all five jobs passed: format/Clippy/tests/docs, Ubuntu tests, macOS tests, Windows tests, and Rust 1.85 MSRV check;
- permanent source workflow is read-only and defines the source-equivalent umbrella contract recorded in `projects/manifest.json`.

### Provenance and hygiene evidence

- canonical history from the exact source head was traversed through the GitHub commits API until the first empty page; the history occupies the first two 100-entry pages and page 3 is empty;
- configured attribution searches found exactly four `Co-authored-by` commits, all carrying `github-actions[bot] <41898282+github-actions[bot]@users.noreply.github.com>`;
- Generated-By, Assisted-By, Signed-off-by, Claude, Anthropic, and OpenAI searches returned zero matches;
- preservation policy: retain the four historical GitHub Actions CI-bot trailers as genuine legacy provenance, mirroring the umbrella's preserve-not-rewrite rule; new umbrella commits still add no attribution trailers;
- recursive exact-tree review shows LICENSE, Cargo workspace/lockfile, source/tests/docs/workflow content and no `.gitmodules` or committed `target`/cache tree observed.

This evidence makes the exact source candidate READY; it is not yet an imported project.

## READY candidate 2 — `distributed-systems-lab`

Selected preflight candidate: `1837b0ece467e3dff598643768b91543acc1350a`, tree `7f54c63fb8d67acde388747dc39544233f8c1dc0`.

PR #82 completed the membership-aware leadership-transfer catch-up path immediately during this live preflight, so the source was re-read after that merge rather than frozen at the earlier #82-active state.

### Live-state and CI evidence

- zero open implementation PRs after #82 merged;
- exact-main CI `34064569580`: **success**;
- source CI is Ubuntu/macOS/Windows × Python 3.11/3.13; every cell installs `.[dev]`, runs Ruff, and runs pytest;
- source-equivalent umbrella verification is recorded explicitly in `projects/manifest.json`.

### Provenance and hygiene evidence

- the complete canonical commit list from the exact head fits in one 100-entry GitHub commits API page; page 2 is empty;
- configured Co-authored-by, Generated-By, Assisted-By, Signed-off-by, Claude, Anthropic, and OpenAI searches are all zero;
- recursive exact-tree review shows Python source, tests, docs, pyproject and CI with no `.gitmodules` or obvious committed build/cache payload;
- no top-level LICENSE file is present at the selected source tree. Migration must preserve that state rather than invent a license file.

This source is READY but not yet imported.

## HOLD candidate — `tinydb-c`

Observed main remains `c0de1768ae3080ec6d12796f82e5abf5a27be89f`.

Stage 1 stabilization PR #5 remains the authoritative storage/recovery/planner lane, with head `c22699d8877bb9bb109c670a67d225e1285623d4`. Do not import current main around that in-flight history. Re-run exact main/open-PR/CI/provenance/hygiene preflight only after the Stage 1 lane is completed or explicitly superseded.

## Preflight gate

Immediately before every actual import:

1. re-read exact source `main` and umbrella `main`;
2. require zero active implementation/stabilization PRs for that source;
3. confirm exact source CI required by the repository;
4. inspect recent commits and declared stability/checkpoint documents;
5. audit reachable history for configured attribution policy;
6. inspect repository hygiene, generated artifacts, caches, binaries, secrets, nested repositories and platform/hardware requirements;
7. define source-equivalent umbrella CI before migration;
8. freeze only the exact source commit that passed those gates.

READY status is revoked if the source moves into a new implementation lane before migration.

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

The executable regression must constrain the claim to exactly what it proves. Once both READY sources are genuinely imported, the highest-value candidate is a bounded replicated state-machine contract between `distributed-systems-lab` and `database-design-lab`; it is still only a proposal at this checkpoint.

## Original repository policy

Source repositories remain available. No source is archived, deleted, force-rewritten or cosmetically re-authored as part of routine consolidation.
