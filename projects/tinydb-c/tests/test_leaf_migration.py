import os
import subprocess
import sys


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_leaf_migration_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_leaf_migration_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_leaf_migration_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_leaf_migration_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError("Could not find tinydb_leaf_migration_probe executable")

    result = subprocess.run(
        [probe],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=30,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError(output)

    required = [
        "LEAF_MIGRATION_OK",
        "compact_roundtrip=yes",
        "compact_row_primitive=yes",
        "full_v1=yes",
        "oversize_downgrade_rejected=yes",
        "overcount_downgrade_rejected=yes",
        "atomic_failure=yes",
        "checksum_reserved=yes",
    ]
    for marker in required:
        if marker not in output:
            raise AssertionError(f"missing {marker}\n{output}")

    print(
        "PASS: V1/V2 leaf migration preserves schema-sized payloads and identity, "
        "exposes a failure-atomic reusable fixed-row compact encoder for whole-tree rebuilds, "
        "round-trips fixed pages, rejects unsafe downgrade geometry atomically, "
        "and leaves Pager checksum trailers untouched"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
