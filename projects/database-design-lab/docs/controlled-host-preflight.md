# Linux controlled-host preflight

`db-lab-host-preflight` records the machine-observable portion of the Phase 4 controlled-host boundary before any publishable timing collection begins. Its protocol is `linux_controlled_host_preflight_v1`.

A passing preflight is a prerequisite, not a benchmark result. It does not make GitHub-hosted CI a performance host, it does not certify human statements as facts, and it does not create a regression threshold.

## Why this is separate from publication admission

`publication_warm_v1` already rejects debug builds, unverified cold-cache claims, missing host/storage/filesystem/build/analysis metadata, and incomplete repeated-run metadata. Those checks prove that required declarations are present; they do not independently prove that the operating system actually pinned the process or that frequency/noise controls were active.

The host preflight therefore keeps two classes of evidence visibly separate:

- **machine-observed hard controls**: process CPU affinity, online CPUs, scaling governor, turbo/boost state, and one-minute system load;
- **operator attestations**: thermal stabilization procedure, unrelated-service/background-load procedure, and filesystem/controller/device-cache procedure.

The first class can make the command fail. The second class is preserved verbatim but is explicitly labelled as attestation rather than measurement or proof.

## v1 hard controls

The first protocol is intentionally Linux-only and conservative. A snapshot passes only when all of the following are true:

1. `/proc/self/status` exposes `Cpus_allowed_list`, and the parsed process affinity is exactly the CPU set supplied by `--expected-cpus`;
2. `/sys/devices/system/cpu/online` shows every expected CPU online;
3. every expected CPU exposes `cpufreq/scaling_governor`, and every value is exactly `performance`;
4. turbo/boost is observable through a supported kernel interface and is disabled. v1 accepts either `intel_pstate/no_turbo = 1` or generic `cpufreq/boost = 0`;
5. `/proc/loadavg` exposes a finite one-minute load, and `load1 / pinned_cpu_count <= --max-load-per-cpu`.

Missing observations fail closed. The tool does not silently substitute a host label, CPU model string, or operator statement for an unavailable kernel observation.

The snapshot also records the OS/architecture, kernel release when readable, and a CPU model string when readable. Those fields are descriptive provenance and are not themselves hard-control gates.

## CPU-list syntax

`--expected-cpus` accepts Linux-style comma-separated ids and inclusive ranges, for example:

```text
2-5
2,4,6
2-3,6-7
```

Descending ranges, duplicates, overlapping ranges, empty components, unreasonably large CPU ids, and CPU sets above the protocol resource bound are rejected. The process affinity must match the normalized set exactly rather than merely contain it. This prevents a benchmark command that was intended to run on CPUs 2-3 from silently retaining access to other online CPUs.

## Noise budget

`--max-load-per-cpu` is explicit rather than hard-coded by the repository. For example, with two pinned CPUs and `--max-load-per-cpu 0.10`, a one-minute system load above `0.20` fails the preflight.

This is a coarse machine-observable noise signal, not a complete scheduler-noise model. A later reviewed host procedure may add stronger host-specific telemetry, but v1 does not invent portable guarantees that Linux does not expose consistently.

## Operator attestations

Three non-empty, bounded statements are required:

- `--thermal-control-attestation`;
- `--background-load-attestation`;
- `--storage-cache-attestation`.

These fields answer “what procedure did the operator use?” They do **not** answer “did the repository independently prove that procedure happened?” The output carries that limitation explicitly.

This separation is deliberate. CPU temperature sensor naming, controller cache state, device-internal cache behavior, and service isolation are not portable enough to turn an arbitrary string or filesystem probe into a universal proof.

## Example controlled-host invocation

Run the preflight in the same process environment/CPU affinity that will launch the benchmark collection. A Linux operator might first bind the shell or command with the host's reviewed mechanism and then run:

```text
db-lab-host-preflight \
  --output evidence/host-preflight-001.json \
  --host-label perf-host-01 \
  --expected-cpus 2-3 \
  --max-load-per-cpu 0.10 \
  --thermal-control-attestation "CPU package stabilized under the reviewed warm-up/cool-down procedure" \
  --background-load-attestation "nonessential services stopped; benchmark user is the only workload user" \
  --storage-cache-attestation "publication_warm_v1 trace-induced warm policy; no cold-cache claim"
```

The output path must not already exist. A fully collected snapshot is written with `create_new`, flushed, and synchronized. This remains true for a control violation: the file records `passed=false` and the complete `violations` array, while the command exits non-zero. Configuration/collection failures that prevent a meaningful snapshot do not fabricate a passed artifact.

## Re-verifying a retained snapshot

`db-lab-host-preflight-verify` is the fail-closed reader for retained v1 snapshots. It does not probe the current machine again. Instead, it verifies that the stored artifact is a valid representation of what the producer recorded:

```text
db-lab-host-preflight-verify \
  --snapshot evidence/host-preflight-001.json \
  --expected-host-label perf-host-01 \
  --require-passed
```

Verification rejects symlinks and oversized files, unknown JSON fields, unsupported protocols, malformed/unsorted CPU sets, mismatched governor keys, inconsistent turbo interface/raw/derived state, invalid numeric values, altered frozen limitations, and surrounding whitespace or resource-bound violations in retained text fields. It then recomputes the complete hard-control violation ledger from the stored observations and requires both `violations` and `passed` to agree exactly with that recomputation.

Without `--require-passed`, an internally consistent `passed=false` snapshot remains valid audit evidence: a failed preflight must not become unreadable merely because it correctly recorded a control failure. With `--require-passed`, such an artifact is rejected for later admission. `--expected-host-label` additionally binds the verifier call to one expected named host.

The shared library entry points are `verify_host_preflight_snapshot`, `load_verified_host_preflight_snapshot`, and `validate_host_preflight_snapshot` in `db_cli::host_preflight`. The publication-session implementation consumes this verifier rather than implementing a second JSON interpretation.

This verifier establishes internal integrity of the repository-defined snapshot contract; it is not a cryptographic signature, proof of authorship, proof that an operator attestation was truthful, or proof that the retained observation still describes the machine at a later time.

## Output boundary

A snapshot contains:

- protocol and Unix recording time;
- stable host label;
- `passed`;
- exact expected affinity, fixed `performance` governor requirement, turbo-disabled requirement, and load budget;
- observed affinity/online CPUs/governors/turbo interface and raw value/load plus descriptive kernel/CPU data;
- operator attestations;
- every hard-control violation;
- explicit limitations.

Host-control snapshots remain separate versioned artifacts rather than fields silently added to the frozen v7/v11 repeated-batch archive formats. `controlled_publication_session_v2` binds two passing snapshots—a preflight and postflight—to one publication-admitted v7/v11 archive by host label, repository revision, source format, and temporal enclosure. `scripts/run-controlled-publication.sh` orchestrates that binding on a real Linux collection host. This separation keeps batch evidence and host-control evidence independently re-verifiable.

The roadmap item “Establish a controlled pinned performance host” remains incomplete until these controls are exercised on a real named and reviewed host and actual repeated publication evidence is collected there.

## Relationship to analysis bundles

The repository can now preserve raw repeated evidence, fail-closed verify it, compute descriptive order-stratified timing summaries, and keep the summary beside re-verifiable raw evidence in an immutable analysis bundle. The host-preflight artifact addresses a different question: whether a real collection session started and ended under the repository's machine-observable host controls. `controlled_publication_session_v2` preserves those two evidence classes together without redefining either one.

Neither artifact makes hosted CI timing publishable. Real Phase 4 completion still requires actual controlled-host collection and review of the resulting denominator and distributions before any statistical regression threshold is proposed.
