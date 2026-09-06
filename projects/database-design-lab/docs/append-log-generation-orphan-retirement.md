# Reserved append-log generation orphan retirement

`append_log_generation_orphan_retire_unix_v2` is a guarded maintenance protocol for reclaiming a higher **uncommitted** append-log generation candidate after its builder has been explicitly declared stopped.

It exists because durable generation reservations now separate identity lifetime from candidate-file lifetime. Once `reserve-%020d.frontier` is durably retained, the matching generation id must never be allocated again even if an abandoned `generation-%020d.log` or `staging-commit-%020d.marker` is later removed.

## Commands

First inspect retained evidence without mutation:

```text
db-lab-log-generation-orphan inspect \
  --directory data/log-generations \
  --generation 9
```

Inspection succeeds only when the requested id is above current authority, has no final commit marker, has a canonical generation log, and has the same-id durable reservation. It reports the exact authoritative generation, candidate fingerprint, optional staging-marker fingerprint, reservation name, and current allocation frontier.

Retirement requires those exact observations plus an explicit operator attestation:

```text
db-lab-log-generation-orphan retire \
  --directory data/log-generations \
  --generation 9 \
  --expected-authority 3 \
  --expected-orphan-bytes 12345 \
  --expected-orphan-crc32 123456789 \
  --expected-staging-bytes 64 \
  --expected-staging-crc32 987654321 \
  --confirm-generation-builder-stopped
```

The staging arguments are omitted when inspection reported no staging marker. Both staging fields must be supplied together.

## Safety contract

Retirement is intentionally not automatic garbage collection. A candidate can be built outside the short cooperative writer lease, so the repository cannot infer from PID, age, or lack of a final marker that its builder is dead. The operator must explicitly confirm that the generation builder has stopped.

On Unix, retirement then:

1. acquires the shared generation writer lease;
2. verifies the expected committed authority;
3. requires a same-id durable reservation and no same-id final commit marker;
4. re-fingerprints the candidate and optional staging marker and requires exact equality with inspection;
5. repeats authority/namespace/fingerprint checks immediately before deletion;
6. removes the optional staging marker and synchronizes the parent directory;
7. re-verifies authority and candidate evidence;
8. removes the abandoned generation candidate and synchronizes the parent directory again;
9. re-verifies unchanged authority and requires the durable reservation to remain;
10. requires the allocation frontier to remain at or above the retired id.

The final reservation is deliberately **not** removed. It is the durable proof that the retired generation id stays consumed forever.

## Fail-closed cases

The operation refuses to retire when:

- the id is at or below current authority;
- a final commit marker exists for the id;
- the same-id durable reservation is absent;
- the candidate is absent or not a real regular file;
- the candidate fingerprint changed after inspection;
- the optional staging-marker presence or fingerprint changed after inspection;
- current authority differs from the inspected authority;
- the cooperative writer lease is held or crash-stale;
- a synchronized directory-removal barrier fails.

A directory-sync failure after a visible removal returns a durability-uncertain error rather than claiming successful reclamation.

## Platform and trust boundary

Retirement is Unix-only because successful deletion requires a parent-directory durability barrier. Non-Unix targets fail before filesystem access. This does not claim adversarial protection against a non-cooperating process that mutates raw paths outside the shared lease contract; the explicit builder-stopped confirmation remains part of the safety boundary.

## Relationship to normal cleanup

`db-lab-log-generation-cleanup` handles history below current authority. Reserved orphan retirement handles a different class: explicitly abandoned candidates **above** authority. Durable reservation evidence is what makes higher-candidate/staging removal safe without lowering the generation allocation frontier.

The broader Phase 1 compaction milestone still remains open for Windows-equivalent durability and legacy single-file migration/coexistence. Physical power-loss testing remains distinct from deterministic crash-state fault injection.
