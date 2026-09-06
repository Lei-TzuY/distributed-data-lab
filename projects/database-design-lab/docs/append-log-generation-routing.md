# Append-log generation-aware mutation routing

`db_cli::generation_engine::GenerationLogEngine` owns a **generation directory** instead of requiring callers to keep and mutate a raw `generation-%020d.log` path.

It implements the common `KvEngine` contract and reports the same append-log logical/storage semantics under the stable name `append-log-generation-v2`.

## Open and recovery

`GenerationLogEngine::open(directory)` acquires the cooperative generation-writer lease before running the shared `append_log_generation_directory_v3` verifier and mutable open. Only the highest final committed marker may select authority; reservation evidence affects generation-id allocation only. Missing/corrupt highest committed generations fail closed and the wrapper never falls back to an older committed generation.

Only after the retained marker and committed-prefix proof succeed does the wrapper open the selected append-log file mutably through `LogEngine::open_managed_generation`. This preserves the existing rule for a canonical incomplete final append: managed mutable open may repair it only after the generation verifier has proven that the marker-bound compact base prefix is intact and the tail begins after that prefix. The directory is verified again after mutable open/repair and authority must still name the same generation.

Ordinary `LogEngine::open` and `LogEngine::create_new` now represent **standalone** ownership. They reject strict canonical `generation-{id:020}.log` paths before filesystem mutation. The explicit managed constructors require that canonical filename shape and preserve managed intent across `KvEngine::reopen`; read-only `LogEngine::verify` and `LogEngine::inspect` remain available for generation evidence.

## Per-operation routing and exclusion

Before every `GET`, `PUT`, `DELETE`, or `REOPEN`, the wrapper acquires the same generation-writer lease used by compact-switch publication. The lease is held through authority refresh and the entire operation, then released. Long-lived routing handles therefore do **not** monopolize the lease between calls.

After acquiring the lease, ordinary operations inspect the highest **final marker id**:

- same id as the current inner handle: continue on that handle;
- higher id: run full generation-directory verification and open the newly selected generation before executing the operation;
- lower id or no final marker: fail closed as forbidden rollback;
- malformed/corrupt higher marker or generation: full verification fails and the routing handle is poisoned.

A poisoned routing handle rejects ordinary operations with `DbError::Poisoned`. Explicit `reopen()` is the recovery boundary: it re-runs the full generation-directory verifier, refuses any authority lower than the generation already observed by the handle, and clears poison only after a valid same-or-higher authority is opened.

The lease is a create-new sibling file of the canonical generation directory, for example `data/generations` uses `data/.generations.append-log-writer.lock`. It deliberately lives **outside** the retained generation namespace, so retained evidence contains only generation logs, marker artifacts, and durable allocation reservations. New lock records contain a protocol tag, process id, and acquisition id for operator diagnosis, but the implementation never trusts any of those fields to infer liveness or steal a lock.

A crashed process may leave a stale lock. That is a liveness failure, not a reason to risk two writers: subsequent coordinated operations fail closed until an operator independently proves no writer is alive. `db-lab-log-generation-lock inspect` exposes the exact bounded lock evidence; `clear-stale` requires both the exact observed record bytes and an explicit `--confirm-no-live-writer` attestation before removal. See `docs/append-log-generation-lock-recovery.md`. Normal lease drop removes the sibling path only while its bytes still match that lease's own owner record.

## Compact-switch interaction

`compact_switch_generation_offline` builds the expensive compact candidate without holding the writer lease. This keeps normal routed operations available during the copy phase. Immediately before authority can change, the switch acquires the lease and, while holding it:

1. re-verifies the old authoritative generation and its full live state;
2. rejects the switch if any compliant writer changed the source during copy construction;
3. re-verifies the compact candidate;
4. durably publishes the new final marker;
5. verifies the new authority and exact compact image.

If a routed writer changed the source before the switch acquired the lease, the locked recheck detects the drift and the candidate remains an uncommitted orphan. If a routed writer tries to operate while the switch holds the lease, it receives a `WouldBlock`-class error before authority refresh or append. After the switch releases the lease, an existing routed handle lazily adopts the new generation on its next operation.

This closes the previous cooperative scan-to-append / final-check-to-publication race between `GenerationLogEngine`, the standalone publisher CLI, and the offline compact switch.

## Boundary

This is **cooperative** cross-process exclusion, not an OS sandbox. The ordinary standalone `LogEngine` constructors and repository CLI mutation paths fail closed on canonical generation filenames, so accidental raw-path mutation no longer bypasses the contract.

The explicit `LogEngine::open_managed_generation` / `create_new_managed_generation` APIs remain available for the generation subsystem, fault-injection fixtures, and other callers that deliberately assert managed ownership. Those constructors validate the canonical filename but cannot themselves prove that the caller holds the generation writer lease or selected the current authority. A caller that invokes them without those checks—or mutates the file through unrelated filesystem APIs—still intentionally ignores the cooperative protocol.

The standalone `db-lab-log-generation-publish` CLI acquires the same lease before invoking the lower-level marker publication primitive. The lower-level shared publisher remains a caller-serialized primitive so the compact switch can call it while already holding the lease without self-deadlock.

Directory v3 reservations are non-authoritative and do not alter routing. They only prevent generation-id reuse after candidate/staging cleanup; see `docs/append-log-generation-reservations.md`.

The project still does not claim Windows final-marker/reservation durability, automatic process-liveness inference, complete higher-orphan cleanup, or adversarial protection from a process that intentionally ignores the coordination protocol.

## Tests

Cross-platform integration proves routed CRUD/reopen behavior, no rollback after higher authority, malformed-higher-marker poisoning, and recovery after marker restoration. Storage-level ownership tests additionally prove standalone constructors reject canonical generation paths without creating/repairing them, managed intent survives reopen, read-only diagnostics remain allowed, and managed constructors do not accept arbitrary noncanonical raw paths. Writer-exclusion coverage additionally proves:

- an externally held lease blocks a routed mutation before any bytes are appended;
- stale lock evidence is not rewritten or automatically stolen;
- the lease file does not enter the retained generation namespace;
- on Unix, a compact switch cannot publish while another lease is held;
- the failed switch leaves only an uncommitted generation orphan, and retry allocates above that orphan rather than reusing its id.

Stale-lock admin coverage additionally proves read-only inspection, missing-confirmation rejection, changed-record rejection, exact-record removal, post-clear reacquisition, and owner-record-aware lease-drop cleanup.

Unix integration continues to prove that a long-lived routed handle can survive a completed offline compact switch and send its first later mutation only to the newly authoritative generation.
