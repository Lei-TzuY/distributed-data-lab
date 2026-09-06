#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: bash scripts/run-controlled-publication.sh [options]

Required paths / experiment:
  --bin-dir PATH
  --trace PATH
  --run-dir PATH                 Must not already exist.
  --revision SHA
  --pair-seed N
  --pairs N

Controlled-host admission:
  --expected-cpus LIST           taskset syntax, e.g. 2-5 or 2,4,6.
  --max-load-per-cpu FLOAT
  --host-label TEXT
  --host-cpu TEXT
  --host-memory TEXT
  --filesystem TEXT
  --mount-options TEXT
  --storage-device TEXT
  --thermal-attestation TEXT
  --background-attestation TEXT
  --storage-cache-attestation TEXT

Publication metadata:
  --optimization-flags TEXT
  --analysis-script-version TEXT
  --noise-budget TEXT

Optional:
  --btree-cache-pages N          Default: 64.
  --notes TEXT
  -h, --help

The runner is Linux-only and requires taskset. It does not build binaries inside the
measurement workflow. Build the release binaries first, configure the host, then run
this script from a quiesced controlled environment.
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 2
}

need_value() {
  [[ $# -ge 2 ]] || die "missing value for $1"
}

BIN_DIR=
TRACE=
RUN_DIR=
REVISION=
PAIR_SEED=
PAIRS=
EXPECTED_CPUS=
MAX_LOAD_PER_CPU=
HOST_LABEL=
HOST_CPU=
HOST_MEMORY=
FILESYSTEM=
MOUNT_OPTIONS=
STORAGE_DEVICE=
THERMAL_ATTESTATION=
BACKGROUND_ATTESTATION=
STORAGE_CACHE_ATTESTATION=
OPTIMIZATION_FLAGS=
ANALYSIS_SCRIPT_VERSION=
NOISE_BUDGET=
BTREE_CACHE_PAGES=64
NOTES=

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin-dir) need_value "$@"; BIN_DIR=$2; shift 2 ;;
    --trace) need_value "$@"; TRACE=$2; shift 2 ;;
    --run-dir) need_value "$@"; RUN_DIR=$2; shift 2 ;;
    --revision) need_value "$@"; REVISION=$2; shift 2 ;;
    --pair-seed) need_value "$@"; PAIR_SEED=$2; shift 2 ;;
    --pairs) need_value "$@"; PAIRS=$2; shift 2 ;;
    --expected-cpus) need_value "$@"; EXPECTED_CPUS=$2; shift 2 ;;
    --max-load-per-cpu) need_value "$@"; MAX_LOAD_PER_CPU=$2; shift 2 ;;
    --host-label) need_value "$@"; HOST_LABEL=$2; shift 2 ;;
    --host-cpu) need_value "$@"; HOST_CPU=$2; shift 2 ;;
    --host-memory) need_value "$@"; HOST_MEMORY=$2; shift 2 ;;
    --filesystem) need_value "$@"; FILESYSTEM=$2; shift 2 ;;
    --mount-options) need_value "$@"; MOUNT_OPTIONS=$2; shift 2 ;;
    --storage-device) need_value "$@"; STORAGE_DEVICE=$2; shift 2 ;;
    --thermal-attestation) need_value "$@"; THERMAL_ATTESTATION=$2; shift 2 ;;
    --background-attestation) need_value "$@"; BACKGROUND_ATTESTATION=$2; shift 2 ;;
    --storage-cache-attestation) need_value "$@"; STORAGE_CACHE_ATTESTATION=$2; shift 2 ;;
    --optimization-flags) need_value "$@"; OPTIMIZATION_FLAGS=$2; shift 2 ;;
    --analysis-script-version) need_value "$@"; ANALYSIS_SCRIPT_VERSION=$2; shift 2 ;;
    --noise-budget) need_value "$@"; NOISE_BUDGET=$2; shift 2 ;;
    --btree-cache-pages) need_value "$@"; BTREE_CACHE_PAGES=$2; shift 2 ;;
    --notes) need_value "$@"; NOTES=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

for name in \
  BIN_DIR TRACE RUN_DIR REVISION PAIR_SEED PAIRS EXPECTED_CPUS MAX_LOAD_PER_CPU \
  HOST_LABEL HOST_CPU HOST_MEMORY FILESYSTEM MOUNT_OPTIONS STORAGE_DEVICE \
  THERMAL_ATTESTATION BACKGROUND_ATTESTATION STORAGE_CACHE_ATTESTATION \
  OPTIMIZATION_FLAGS ANALYSIS_SCRIPT_VERSION NOISE_BUDGET; do
  [[ -n ${!name} ]] || die "required option for $name is missing or empty"
done

[[ $(uname -s) == Linux ]] || die "controlled publication runner is supported only on Linux"
command -v taskset >/dev/null 2>&1 || die "taskset is required"
[[ -f $TRACE && ! -L $TRACE ]] || die "trace must be a real regular file: $TRACE"
[[ ! -e $RUN_DIR ]] || die "run directory already exists: $RUN_DIR"
[[ $PAIRS =~ ^[0-9]+$ && $PAIRS -gt 0 ]] || die "pairs must be a positive integer"
[[ $PAIR_SEED =~ ^[0-9]+$ ]] || die "pair-seed must be an unsigned integer"
[[ $BTREE_CACHE_PAGES =~ ^[0-9]+$ && $BTREE_CACHE_PAGES -gt 0 ]] || die "btree-cache-pages must be a positive integer"

required_bins=(
  db-lab-host-preflight
  db-lab-batch
  db-lab-batch-verify
  db-lab-publication-session
  db-lab-batch-analysis-bundle
)
for bin in "${required_bins[@]}"; do
  [[ -x "$BIN_DIR/$bin" ]] || die "required executable is missing: $BIN_DIR/$bin"
done

mkdir "$RUN_DIR"
PREFLIGHT="$RUN_DIR/host-preflight.json"
POSTFLIGHT="$RUN_DIR/host-postflight.json"
ENGINE_ROOT="$RUN_DIR/engines"
ARCHIVE_DIR="$RUN_DIR/raw-archive"
SESSION_DIR="$RUN_DIR/session"
ANALYSIS_BUNDLE="$RUN_DIR/analysis-bundle"

run_preflight() {
  local output=$1
  taskset -c "$EXPECTED_CPUS" "$BIN_DIR/db-lab-host-preflight" \
    --output "$output" \
    --host-label "$HOST_LABEL" \
    --expected-cpus "$EXPECTED_CPUS" \
    --max-load-per-cpu "$MAX_LOAD_PER_CPU" \
    --thermal-control-attestation "$THERMAL_ATTESTATION" \
    --background-load-attestation "$BACKGROUND_ATTESTATION" \
    --storage-cache-attestation "$STORAGE_CACHE_ATTESTATION"
}

printf '%s\n' '== preflight =='
run_preflight "$PREFLIGHT"

batch_args=(
  --trace "$TRACE"
  --engine-root "$ENGINE_ROOT"
  --archive-dir "$ARCHIVE_DIR"
  --pair-seed "$PAIR_SEED"
  --pairs "$PAIRS"
  --btree-cache-pages "$BTREE_CACHE_PAGES"
  --revision "$REVISION"
  --host-label "$HOST_LABEL"
  --host-cpu "$HOST_CPU"
  --host-memory "$HOST_MEMORY"
  --filesystem "$FILESYSTEM"
  --mount-options "$MOUNT_OPTIONS"
  --storage-device "$STORAGE_DEVICE"
  --cache-state warm
  --admission publication-warm-v1
  --optimization-flags "$OPTIMIZATION_FLAGS"
  --analysis-script-version "$ANALYSIS_SCRIPT_VERSION"
  --noise-budget "$NOISE_BUDGET"
)
if [[ -n $NOTES ]]; then
  batch_args+=(--notes "$NOTES")
fi

printf '%s\n' '== publication batch =='
set +e
taskset -c "$EXPECTED_CPUS" "$BIN_DIR/db-lab-batch" "${batch_args[@]}"
batch_status=$?
set -e

# The postflight is load-bearing even when the batch returns nonzero. A retained failed-pair archive
# is scientifically meaningful evidence and must be enclosed by the same host-control snapshots.
printf '%s\n' '== postflight =='
run_preflight "$POSTFLIGHT"

printf '%s\n' '== raw archive verification =='
"$BIN_DIR/db-lab-batch-verify" \
  --archive-dir "$ARCHIVE_DIR" \
  --expected-revision "$REVISION" \
  --require-publication

printf '%s\n' '== publication session creation =='
"$BIN_DIR/db-lab-publication-session" create \
  --host-preflight "$PREFLIGHT" \
  --host-postflight "$POSTFLIGHT" \
  --archive-dir "$ARCHIVE_DIR" \
  --session-dir "$SESSION_DIR" \
  --expected-revision "$REVISION"

printf '%s\n' '== publication session verification =='
"$BIN_DIR/db-lab-publication-session" verify \
  --session-dir "$SESSION_DIR" \
  --expected-revision "$REVISION"

# Analyze the session's retained copy, not the mutable source archive pathname. Bundle creation itself
# re-verifies and runs the descriptive analyzer before sealing its own evidence snapshot.
printf '%s\n' '== analysis bundle creation =='
"$BIN_DIR/db-lab-batch-analysis-bundle" create \
  --archive-dir "$SESSION_DIR/evidence" \
  --bundle-dir "$ANALYSIS_BUNDLE" \
  --expected-revision "$REVISION"

printf '%s\n' '== analysis bundle verification =='
"$BIN_DIR/db-lab-batch-analysis-bundle" verify \
  --bundle-dir "$ANALYSIS_BUNDLE" \
  --expected-revision "$REVISION"

printf 'controlled publication artifacts sealed under %s\n' "$RUN_DIR"
if [[ $batch_status -ne 0 ]]; then
  printf 'batch exited with status %d; retained failure evidence was enclosed and verified\n' "$batch_status" >&2
  exit "$batch_status"
fi
