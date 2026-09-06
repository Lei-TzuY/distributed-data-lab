# Distributed Data Lab Migration Protocol & Ledger

This document records migration governance, history-preserving imports, and cross-project integration evidence for `distributed-data-lab`.

## Status vocabulary

- **PRE-FLIGHT** — candidate selected but not frozen.
- **READY FOR IMPORT** — exact source main, open-PR, CI, history/provenance, hygiene, and source-equivalent umbrella-gate definitions are clean.
- **HOLD** — active implementation/stabilization or another blocker prevents a safe freeze.
- **IMPORTED / VERIFIED** — non-squashed source ancestry, exact tree identity and source-equivalent umbrella CI are all proven.
- **INTEGRATION VERIFIED** — an executable cross-project data/distributed contract is permanently tested.

## Live source ledger — 2026-09-07

| Project | Exact observed source | Status |
| --- | --- | --- |
| `database-design-lab` | `fba592f25225621f83931662565120c43ede1885` | **IMPORTED / VERIFIED candidate** — migration history/tree complete; PR/main permanent gates still required |
| `distributed-systems-lab` | `1837b0ece467e3dff598643768b91543acc1350a` | **READY FOR IMPORT** |
| `tinydb-c` | `c0de1768ae3080ec6d12796f82e5abf5a27be89f` | **HOLD** — Stage 1 PR #5 active |

Source state is re-read immediately before every migration. Historical green evidence never overrides a moved source head or a new implementation lane.

## Migration 1 — `database-design-lab`

Frozen source: `fba592f25225621f83931662565120c43ede1885`.
Source tree: `4c5bb1129c4b145923dafa2003412bcda8c0c806`.

### Source/preflight evidence

- zero open implementation PRs at final preflight;
- exact source-main CI `34061539532`: all five jobs success;
- source contract: rustfmt, Clippy `-D warnings`, tests/docs, Ubuntu/macOS/Windows tests, and Rust 1.85 workspace check;
- recursive source-tree review found LICENSE, Cargo workspace/lockfile, source/tests/docs/workflow content, no `.gitmodules`, and no committed target/cache tree observed.

### Migration-time provenance and history proof

Temporary bootstrap run `34064926421` rechecked the source immediately before import and then used the real Git history rather than a copied snapshot:

- exact live source main remained `fba592f25225621f83931662565120c43ede1885`;
- open source PR count remained zero;
- exact fetched source tree remained `4c5bb1129c4b145923dafa2003412bcda8c0c806`;
- complete `git log` scan audited **131 reachable source commits**;
- exactly four `Co-authored-by` trailers were found, all `github-actions[bot] <41898282+github-actions[bot]@users.noreply.github.com>` on the expected four historical commits;
- all other Co-authored-by values and Generated-By, Assisted-By, Signed-off-by, Claude, Anthropic and OpenAI markers were zero;
- policy: preserve the four historical CI-bot trailers; do not rewrite canonical source history.

The first bootstrap attempt `34064887549` failed closed before import because a NUL-delimited audit parser retained a leading newline on SHA fields. Its scan already observed the expected four trailers; no subtree step ran. The parser was corrected by normalizing commit IDs, without weakening any source/provenance rule, and run `34064926421` then passed every guard.

### History-preserving subtree

Subtree commit: `70ead65382d116f09ab95c7f195cdbdde06b0e69`.

It is a two-parent commit:

1. migration branch parent `b7ab702733a80009100deafcd3703a6bf04d7528`;
2. exact source parent `fba592f25225621f83931662565120c43ede1885`.

The commit records `git-subtree-split: fba592f...`. Bootstrap proof required both source ancestry and exact source-tree == `HEAD:projects/database-design-lab` tree equality before the branch could be pushed.

The temporary `contents: write` bootstrap workflow was deleted immediately after the import commit. Permanent `.github/workflows/database-design-lab.yml` uses `contents: read` and repeats history/tree/provenance plus source-equivalent Rust verification on migration PRs and exact merged main.

This import does **not** imply distributed-database, replication, or cross-engine interoperability.

## READY candidate — `distributed-systems-lab`

Selected candidate: `1837b0ece467e3dff598643768b91543acc1350a`, tree `7f54c63fb8d67acde388747dc39544233f8c1dc0`.

PR #82 completed the membership-aware leadership-transfer catch-up path during live preflight, so the source was re-read after that merge:

- zero open implementation PRs;
- exact-main CI `34064569580`: success;
- Ubuntu/macOS/Windows × Python 3.11/3.13; every cell installs `.[dev]`, runs Ruff and pytest;
- complete canonical commit list fits one 100-entry API page and page 2 is empty;
- configured Co-authored-by, Generated-By, Assisted-By, Signed-off-by, Claude, Anthropic and OpenAI searches are all zero;
- exact source tree contains source/tests/docs/pyproject/CI, no `.gitmodules` or obvious committed build/cache payload;
- no top-level LICENSE is present and migration must preserve that state rather than invent metadata.

It remains READY, not imported, until its own independent non-squashed migration passes.

## HOLD candidate — `tinydb-c`

Observed main remains `c0de1768ae3080ec6d12796f82e5abf5a27be89f`.

Stage 1 stabilization PR #5 remains the authoritative storage/recovery/planner lane at head `c22699d8877bb9bb109c670a67d225e1285623d4`. Do not import around that in-flight history. Re-run live main/open-PR/CI/provenance/hygiene preflight after Stage 1 completes or is explicitly superseded.

## Preflight gate

Immediately before every import:

1. re-read exact source `main` and umbrella `main`;
2. require zero active implementation/stabilization PRs;
3. confirm exact source CI;
4. inspect recent commits/checkpoint documents;
5. audit reachable history and attribution policy;
6. inspect repository hygiene and platform requirements;
7. define source-equivalent umbrella CI;
8. freeze only the exact source commit that passed those gates.

## Integration evidence rule

Importing database and consensus repositories into one umbrella does **not** create a distributed database. A verified edge must name and execute a real boundary such as state-machine commands/replies, supported transaction/workload semantics, snapshot/restore formats, or acknowledgement/durability behavior under failures. Claims stay bounded to that executable proof.

## Original repository policy

Source repositories remain available. Routine consolidation does not archive, delete, force-rewrite, or cosmetically re-author them.
