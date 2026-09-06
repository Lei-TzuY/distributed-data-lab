# Append-log generation writer lock recovery

`append_log_generation_writer_lock_v1` intentionally fails closed when its sibling lock file already exists. A crashed coordinated writer can therefore leave a stale lock that blocks `GenerationLogEngine`, the standalone generation publisher, and the compact-switch publication critical section.

The recovery tool is explicit and operator-driven:

```text
db-lab-log-generation-lock inspect \
  --directory data/generations
```

`inspect` is read-only. It reports the canonical sibling lock path, exact bounded `record_hex`, recorded protocol/PID/acquisition id when parseable, and whether a lock is present.

After independently confirming that **no coordinated writer is alive**, remove exactly the inspected evidence:

```text
db-lab-log-generation-lock clear-stale \
  --directory data/generations \
  --expected-record-hex <record_hex from inspect> \
  --confirm-no-live-writer
```

## Safety contract

The tool does not infer liveness from PID, process age, file mtime, or acquisition id. PIDs can be reused and timestamps are not ownership proofs. `--confirm-no-live-writer` is therefore a load-bearing operator attestation, not a cosmetic switch.

`clear-stale` fails unless all of the following hold:

1. the explicit confirmation flag is present;
2. the sibling lock is a real regular file rather than a symlink or non-file;
3. the record is at most 4096 bytes;
4. the exact current bytes equal the supplied `--expected-record-hex`;
5. a second read immediately before removal still matches those bytes.

This protects against accidentally clearing different lock evidence than the operator inspected. It does **not** claim an adversarial atomic compare-and-delete primitive; the generation writer protocol remains cooperative. A process that deliberately ignores the lease or an operator who falsely confirms liveness can still violate exclusion.

## Acquisition identity and normal cleanup

New leases record:

```text
protocol=append_log_generation_writer_lock_v1
pid=<process id>
acquisition=<pid>-<time component>-<process-local counter>
```

The acquisition id distinguishes successive lock ownership records for diagnosis and guarded cleanup; it is not a cryptographic token and is never used to decide whether a process is alive.

`GenerationWriterLease::drop` now removes the sibling path only when the current bounded regular-file bytes still equal the lease's own owner record. If the path was externally removed and replaced with different evidence, the old lease leaves the replacement untouched instead of blindly deleting it.

## Failure behavior

A stale or live lock remains authoritative for cooperative exclusion until it is removed. Inspection never rewrites it. A missing confirmation flag, changed bytes, oversized record, symlink/non-file, missing lock, or I/O error fails closed and leaves the observed path untouched whenever possible.

After a successful stale clear, the next coordinated operation still performs the normal create-new lease acquisition. The admin tool does not itself grant a writer lease or mutate retained generation/marker evidence.

## Remaining boundary

This closes the repository's stale-lock **operator tooling** gap. It does not provide automatic crash-owner detection, lease timeouts, forced process termination, Windows final-marker durability, obsolete/orphan generation cleanup, or legacy single-file migration/coexistence. Composed compact-switch fault injection is documented separately and deliberately uses graceful injected errors; actual process death may retain the lock evidence governed here.
