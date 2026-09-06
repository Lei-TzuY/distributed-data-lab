# Controlled publication collection runner

`scripts/run-controlled-publication.sh` composes the repository's existing Phase 4 evidence tools into one fail-fast Linux operator workflow. It does not create a new evidence protocol and it does not turn hosted CI into a benchmark host. Its purpose is to make a real controlled-host collection harder to perform incorrectly.

Run it only after the release binaries have already been built and the dedicated host has been configured. The script deliberately does **not** invoke Cargo inside the collection workflow because compiling immediately before or during a timing run would contaminate the host state that preflight is meant to check.

Invoke the file through Bash; executable mode is not required in the source checkout:

```text
bash scripts/run-controlled-publication.sh \
  --bin-dir target/release \
  --trace evidence/traces/mixed-42.json \
  --run-dir evidence/controlled/mixed-42-001 \
  --revision 0123456789abcdef0123456789abcdef01234567 \
  --pair-seed 42 \
  --pairs 20 \
  --expected-cpus 2-3 \
  --max-load-per-cpu 0.10 \
  --host-label perf-host-01 \
  --host-cpu "reviewed CPU/topology description" \
  --host-memory "reviewed memory configuration" \
  --filesystem ext4 \
  --mount-options "rw,noatime" \
  --storage-device "reviewed device/model label" \
  --thermal-attestation "reviewed thermal stabilization procedure" \
  --background-attestation "reviewed background-load isolation procedure" \
  --storage-cache-attestation "publication_warm_v1 trace-induced warm policy" \
  --optimization-flags "release profile and reviewed rustflags" \
  --analysis-script-version "verified_operational_timing_descriptive_v1" \
  --noise-budget "reviewed-noise-budget-v1"
```

Use `--notes` only for bounded non-secret experiment notes. `--btree-cache-pages` defaults to 64.

## Preconditions

The runner is Linux-only and requires `taskset`. It requires these already-built executables under `--bin-dir`:

- `db-lab-host-preflight`;
- `db-lab-batch`;
- `db-lab-batch-verify`;
- `db-lab-publication-session`;
- `db-lab-batch-analysis-bundle`.

The trace must be a real regular file rather than a symlink. The requested run directory must not exist. This create-new root prevents a retry from silently mixing artifacts from different attempts.

The operator remains responsible for configuring the host before invoking the script: CPU governor/turbo state, thermal procedure, unrelated services, storage/cache procedure, filesystem/mount configuration, and any host-specific isolation outside the portable preflight contract.

## Measurement ordering

The script performs this exact high-level sequence:

1. Run a passing host preflight under `taskset -c <expected CPUs>`.
2. Run `db-lab-batch` under the **same** `taskset` CPU set with `--admission publication-warm-v1` and `--cache-state warm`.
3. Immediately run the host preflight producer again under the same CPU set, producing the postflight snapshot.
4. Verify the raw publication archive with `db-lab-batch-verify --require-publication`.
5. Create a v2 publication session from preflight + postflight + raw archive.
6. Re-verify that session.
7. Create an immutable analysis bundle from the session's retained `evidence/` copy rather than from the original raw-archive pathname.
8. Re-verify the analysis bundle.

The resulting run root contains:

```text
<run-dir>/
  host-preflight.json
  host-postflight.json
  engines/
  raw-archive/
  session/
    index.json
    host-preflight.json
    host-postflight.json
    evidence/...
  analysis-bundle/
    index.json
    analysis.json
    evidence/...
```

`engines/` is working state, not publication evidence. The publication session and analysis bundle retain their own validated evidence copies under their existing protocols.

## Failed-pair handling is intentional

`db-lab-batch` can return nonzero after successfully retaining a batch that contains a failed pair. The runner must not interpret every nonzero batch exit as permission to skip postflight.

It therefore records the batch exit status, runs postflight regardless, and then attempts the normal publication archive verification/session/bundle path. If the retained failure archive is structurally valid publication evidence, it is enclosed and analyzed before the runner finally returns the original nonzero batch status.

This is load-bearing. Stopping immediately on the batch exit code would create a success-only evidence bias: successful batches would receive postflight/session enclosure while retained failed batches would not.

If the batch failed before producing a valid archive, the later raw verifier fails and the runner returns nonzero. If postflight itself fails its host controls, the script stops before creating a publication session; the raw archive and both control snapshots remain available for diagnosis but are not promoted into a passing controlled session.

## What CI proves

The integration test uses fake repository binaries plus fake `taskset`/Linux host identity. It validates orchestration only:

- success follows the intended command sequence;
- a retained batch failure still receives postflight, archive verification, session creation/verification, and analysis-bundle creation/verification before the batch status is returned.

Those fake fixtures are never timing evidence. GitHub-hosted CI still proves only code/build/orchestration correctness.

## Remaining Phase 4 boundary

This runner removes an operator sequencing hazard; it does not remove the need for a real named controlled host. The Phase 4 roadmap remains incomplete until the workflow is actually run on reviewed hardware, the retained denominator/order strata/distributions are reviewed, and any later regression threshold is frozen from that real evidence rather than from repository CI.
