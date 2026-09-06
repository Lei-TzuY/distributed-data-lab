# Append-log legacy cutover verification

`db-lab-log-generation-cutover-verify` is a read-only verifier for the narrow handoff point immediately after legacy migration and pathname cutover.

It is deliberately stricter than ordinary generation-directory verification. Run it after `db-lab-log-generation-cutover` and before routing any new mutation, reserving a later generation, or compacting the target.

```console
cargo run -p db-cli --bin db-lab-log-generation-cutover-verify -- \
  --legacy-source legacy.db \
  --target-directory generations
```

A successful report uses protocol `append_log_legacy_cutover_verification_v1` and proves all of the following without modifying the cutover evidence:

- the former legacy pathname is a real regular file containing only the expected `append_log_legacy_cutover_sentinel_v1` JSON schema;
- the sentinel binds the supplied canonical generation directory and the retained sibling path `<legacy>.retired-append-log-v1`;
- the retained sibling is a complete, clean append-log image with no recoverable tail;
- the target is still the untouched imported generation 1, with marker 1, no staging marker, no uncommitted generation, and no later reservation frontier;
- the authoritative generation has not grown past its marker-bound committed prefix; and
- the compacted generation-1 authority reproduces the retained legacy live key/value state exactly.

Migration intentionally compacts the legacy append history while importing it. Therefore the retained legacy file and generation-1 file are generally not byte-for-byte identical even when migration is correct: old overwritten/deleted records remain in retained rollback evidence but are absent from the compacted authority. Fresh cutover verification compares the complete live key/value state instead of making a false physical-byte-identity claim.

Unix migration may predate durable generation-1 reservation evidence, so fresh verification accepts either no reservation or exactly reservation 1. Windows migration requires and retains reservation 1, but the verifier intentionally checks the common cross-platform handoff contract rather than duplicating platform-specific migration policy.

The verifier is expected to fail after normal generation-aware writes begin. That failure does not mean the database is corrupt; it means the one-time migration/cutover equivalence proof is no longer fresh. Use `db-lab-log-generation-verify` for the ongoing generation-directory recovery contract after handoff.

The command also detects retained rollback drift that changes logical state. On Unix, an already-open pre-cutover raw handle can continue writing the retained inode after pathname replacement; on Windows, later direct mutation of the retained copy is likewise outside the generation ownership contract. If such mutation changes the retained live state, fresh-cutover verification fails closed. The v1 sentinel does not contain a cryptographic digest of the original retained bytes, so this verifier does not claim to detect byte-level history changes that leave the final live state unchanged.
