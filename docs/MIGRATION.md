# Distributed Data Lab Migration Protocol & Ledger

This document records migration governance, history-preserving imports, and cross-project integration evidence for `distributed-data-lab`.

## Status vocabulary

- **PRE-FLIGHT** — candidate selected but not frozen.
- **READY FOR IMPORT** — exact source main, open-PR, CI, history/provenance, hygiene, and source-equivalent umbrella-gate definitions are clean.
- **HOLD** — active implementation/stabilization or another blocker prevents a safe freeze.
- **IMPORTED / VERIFIED** — non-squashed source ancestry, exact tree identity and source-equivalent umbrella CI are all proven.
- **INTEGRATION VERIFIED** — an executable cross-project data/distributed contract is permanently tested.

## Live source ledger — 2026-09-09

| Project | Exact observed source | Status |
| --- | --- | --- |
| `database-design-lab` | `fba592f25225621f83931662565120c43ede1885` | **IMPORTED / VERIFIED** — exact migration and merged-main gates green |
| `distributed-systems-lab` | `f1638f9121ee550b688a3f6bf9ab03df369139dc` | **IMPORTED / VERIFIED** — exact migration and merged-main gates green |
| `tinydb-c` | `81d98f75b92a80b6e103af86913009b2ec7a6ec2` | **IMPORTED / VERIFIED** — exact migration and merged-main gates green |

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

## Migration 2 — `distributed-systems-lab`

Frozen source: `f1638f9121ee550b688a3f6bf9ab03df369139dc`.
Source tree: `b816d1f0a658a611887b5d4fe17f05a5499e9bd2`.

PR #107 completed the crashed-leader `InstallSnapshot` authority fence, so the source was re-read after that merge:

- zero open implementation PRs;
- exact-main CI `34318592362`: success;
- Ubuntu/macOS/Windows × Python 3.11/3.13; every cell installs `.[dev]`, runs Ruff and pytest;
- bootstrap run `34332419886` audited all 110 commits reachable from the exact source head;
- configured Co-authored-by, Generated-By, Assisted-By, Signed-off-by, Claude, Anthropic and OpenAI markers are all zero;
- exact source tree contains source/tests/docs/pyproject/CI, no `.gitmodules` or obvious committed build/cache payload;
- no top-level LICENSE is present and migration must preserve that state rather than invent metadata.

Temporary bootstrap run `34332419886` then rechecked the same live main and zero open PRs, fetched the exact source history, repeated provenance/hygiene guards, and created non-squashed subtree commit `447a8c8493c1c612e9e9b8a66fd5c194a4200dd7`. Its first parent is bootstrap commit `ea7a616830660f07e7f34b179054369952584b28`; its second parent is exact source `f1638f9121ee550b688a3f6bf9ab03df369139dc`. The imported subtree tree equals `b816d1f0a658a611887b5d4fe17f05a5499e9bd2` exactly.

The temporary `contents: write` workflow was removed before publication. Permanent `.github/workflows/distributed-systems-lab.yml` uses `contents: read`, re-proves source ancestry/tree/provenance, and mirrors the source's Ubuntu/macOS/Windows × Python 3.11/3.13 Ruff/pytest matrix. PR #4 exact head `a1fff30503aac7ebc8a5446783e5ded2bac93073` passed runs `34332958390`, `34332958427`, and `34332958577`; normal merge `b5f4f364cb60d8e74cfb9ac7115233e03ef3062e` passed exact-main runs `34333643232`, `34333643233`, and `34333643318`.

## Migration 3 candidate — `tinydb-c`

Frozen source: `81d98f75b92a80b6e103af86913009b2ec7a6ec2`.
Source tree: `e0c9006f316e6107f13845a3e5a1a0a94264b0e5`.

The four unresolved review findings on Stage 1 PR #5 were repaired at exact head `85555e75710936ce5e6a8cb8ac873ddb29135170`: user-version metadata reads now use the frame read lock, write-unlock failure attempts pin release, nullable scan visitors remain nullable/defensive, and `profile.c` includes its required standard headers. The full local suite passed 293/293; exact PR-head run `34333863736` passed Ubuntu and Windows. The 1,360-commit PR was normal-merged to preserve history, producing source main `81d98f75b92a80b6e103af86913009b2ec7a6ec2`; exact merged-main run `34334920828` also passed Ubuntu and Windows.

Final source preflight then established:

- zero open PRs;
- 1,370 commits reachable from the exact merged main;
- configured Co-authored-by, Generated-By, Assisted-By, Signed-off-by, Claude, Anthropic and OpenAI markers all zero;
- exact source tree contains MIT `LICENSE`, `CMakeLists.txt`, `STABILIZATION.md`, source/tests and read-only CI;
- no `.gitmodules` or committed build/cache tree.

Temporary bootstrap run `34335988505` rechecked the unchanged source main and zero-open-PR state, fetched exact history, repeated the provenance/hygiene guards, and created non-squashed subtree commit `d5ebf9c21279a7da452e20be5f20b82085893174`. Its first parent is bootstrap commit `5a04ea92a4723d40777997f99335668979d385a4`; its second parent is exact source `81d98f75b92a80b6e103af86913009b2ec7a6ec2`. The imported subtree tree equals `e0c9006f316e6107f13845a3e5a1a0a94264b0e5` exactly.

The temporary `contents: write` workflow was removed before publication. Permanent `.github/workflows/tinydb-c.yml` uses `contents: read`, re-proves source ancestry/tree/provenance, and mirrors the source's CMake Debug build plus full test runner on Ubuntu and Windows. PR #5 exact head `1547e9f56d9d27f98083f56fce972b4f3794008e` passed umbrella runs `34336374920`, `34336375001`, `34336374844`, and `34336374982`; normal merge `50008bb9b18af694ba050a692a808c544f1525f2` passed exact-main runs `34337504100`, `34337504096`, `34337504113`, and `34337504168`.

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
