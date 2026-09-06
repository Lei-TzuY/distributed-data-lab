# Offline append-log generation compact switch

`db-lab-log-generation-compact-switch` composes the shared generation-directory verifier, durable generation reservation, append-log compact-copy implementation, and durable generation-marker publisher into one **offline** authoritative switch on Unix.

```text
db-lab-log-generation-compact-switch \
  --directory data/log-generations
```

The current protocol is `append_log_offline_generation_compact_switch_unix_v2`. Version 2 differs from v1 by durably reserving the candidate generation id before candidate construction and retaining that reservation in the result/evidence namespace.

## Required operating condition

Every raw-path writer that can mutate an append-log generation directly must remain quiesced for the entire command. This is a real correctness precondition, not a performance recommendation.

`GenerationLogEngine` writers use the same cooperative sibling lease as the switch. They may run while the expensive compact copy is built; the switch then acquires the lease, detects any source drift, and fails before marker publication. They cannot run during the final recheck-to-publication critical section. The repository still exposes raw-path `LogEngine`, however, and such callers bypass this lease. The operation therefore remains an offline maintenance primitive rather than claiming protection from non-cooperating writers.

On non-Unix targets the operation returns unsupported before touching the supplied directory. In particular, Windows remains disabled because the repository does not yet claim an equivalent durable parent-directory publication primitive there.

## Successful order

The switch preserves the frozen generation recovery law while separating identity allocation from candidate lifetime:

1. reserve `highest_observed_generation + 1` durably through `append_log_generation_reservation_unix_v1` while holding the shared writer lease;
2. release the reservation lease and verify `append_log_generation_directory_v3`, selecting only the highest final committed marker;
3. require the reserved id still to be strictly newer than current authority;
4. snapshot the authoritative generation's complete inspected live state and marker/recovery witness;
5. build a fresh compact log at the reserved canonical generation path through `append_log_compact_copy_v1`;
6. acquire the cooperative writer lease again, re-verify the generation directory, and re-inspect the old authoritative source;
7. require the authoritative generation, committed-prefix witness, complete log verification, and complete live state to be unchanged;
8. independently inspect the compact target and require exact live-state equality;
9. publish its marker through `append_log_generation_marker_publication_unix_v1`;
10. re-run generation-directory verification and require the reserved generation to be authoritative;
11. re-inspect the new authoritative generation and require exact equality with the pre-switch live state.

The old generation, old marker, and durable reservation are retained. This command performs no historical cleanup.

The reservation is intentionally durable before step 5. A later failed candidate build, source-drift rejection, or marker-publication failure therefore cannot make the generation id reusable merely because candidate/staging artifacts are later removed.

## Reservation races

Reservation and candidate construction use separate lease windows so the expensive compact copy does not block routed writers. This permits useful concurrency without allowing identity reuse.

After reservation succeeds, another cooperative process may reserve an even higher id while the first switch is building. That does not invalidate the first reservation because reservations are non-authoritative. Both ids remain permanently consumed.

If another switch actually advances committed authority before the first switch snapshots its source and the new authority reaches or exceeds the first reserved id, the first switch fails with `ReservedGenerationObsolete` and must retry with a new reservation. If authority changes later during candidate construction, the final lease-held source/authority recheck fails with `SourceChanged`; the candidate remains uncommitted and its reservation remains retained.

## Race and crash behavior

A new compact generation has no authority until its final marker is durably published. If the old authoritative source changes before the pre-publication recheck, the command fails with `SourceChanged`; the compact target remains only an uncommitted orphan and recovery continues to select the old committed generation.

The test suite injects a deterministic late source write after compact construction and before marker publication. It requires the switch to fail, requires the new generation file to remain uncommitted, requires the corresponding durable reservation to remain retained, and requires the v3 reader to keep the old generation authoritative.

The allocation frontier treats uncommitted generation logs, staging markers, and durable reservations as consumed ids. Crash residue is never silently reused. Actual CLI integration additionally retains orphan generation 5 and staging marker 7, durably reserves id 8 through the standalone reservation command, then requires the switch itself to reserve and publish generation 9.

Marker publication retains its own durability-uncertain state. If the final marker becomes visible but the final parent-directory durability barrier fails, the operation returns the publication error and does not pretend the switch failed before authority could have changed. Operators must preserve retained generations and run the generation verifier before deciding how to retry.

## Deterministic composed interruption matrix

The unit-test fault harness injects one reported failure at each retained-state boundary in the composed switch. Each case starts from the same deterministic binary-KV history, first creates a durable reservation for generation 2, drops the live operation at the selected later boundary, runs the normal generation-directory verifier, and requires the selected generation to contain exactly the source live entries.

| Injected boundary | Retained evidence | Permitted recovery |
| --- | --- | --- |
| compact candidate published | durable reservation + visible complete uncommitted generation | old only |
| lease-held source/candidate recheck complete | durable reservation + complete uncommitted generation | old only |
| generation file and directory synchronized | durable reservation + durable uncommitted generation | old only |
| half of staging marker written | durable reservation + truncated non-authoritative staging marker | old only |
| staging marker synchronized | durable reservation + complete non-authoritative staging marker | old only |
| final marker hard-linked, before directory sync | reservation + final marker may survive or be lost; staging remains | old or new |
| final marker directory sync complete | reservation + durable final marker; staging may remain | new only |
| publisher complete | reservation + durable final marker; staging cleanup attempted | new only |

Every injected case requires `reservation_generation_ids == [2]`. Thus a failed switch no longer relies on candidate/staging retention merely to stop id 2 from being reused.

For the pre-directory-sync hard-link boundary, the ordinary injected-error fixture observes the visible final marker and selects new. A second deterministic fixture models the other filesystem-permitted outcome by removing that unsynchronized directory entry and requires recovery to select old. Both generations contain the same logical state; the test does not claim that deleting an entry is a physical power-loss emulator.

Injected errors unwind normally, so a lease acquired by the operation is released. A real process death may retain the sibling writer-lock evidence and must use the guarded stale-lock recovery procedure. Hosted CI establishes protocol and recovery behavior for these concrete namespace states; it is not evidence that every filesystem/hardware stack implements power-loss durability identically.

## What this completes

The Unix offline switch now performs the complete reservation -> old-authority snapshot -> fresh compact generation -> durable marker -> new-authority sequence without subprocess composition or duplicated recovery rules.

It closes these repository-side pieces for the Unix offline case:

- durable generation identity allocation before candidate construction;
- authoritative source selection;
- exact live-state compaction;
- stale-source detection before publication;
- durable marker publication;
- deterministic fault coverage across every composed retained-state boundary after reservation;
- permanent id retirement after failed candidate/publication work;
- exact old-or-new logical recovery through the normal generation verifier;
- post-publication authoritative/state verification.

## What remains

The broader Phase 1 compaction milestone remains open because the append-log engine still lacks several lifecycle pieces:

- Windows-equivalent durable marker/reservation publication;
- guarded deletion of confirmed-abandoned higher candidates/staging artifacts now that reservation evidence can preserve their ids;
- migration/coexistence rules for legacy one-file append-log users;
- stronger ownership if non-cooperating raw-path writers must be prevented rather than administratively quiesced.

Until those exist, this command is an explicit offline maintenance primitive, not a transparent production compactor.
