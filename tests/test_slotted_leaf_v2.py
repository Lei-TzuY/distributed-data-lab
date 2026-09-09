import os
import subprocess
import sys


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_slotted_leaf_v2_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_slotted_leaf_v2_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_slotted_leaf_v2_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_slotted_leaf_v2_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError("Could not find tinydb_slotted_leaf_v2_probe executable")

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
    if "SLOTTED_LEAF_V2_OK" not in output:
        raise AssertionError("missing SLOTTED_LEAF_V2_OK marker\n" + output)

    required = [
        "variable_gt_293=yes",
        "sorted=yes",
        "update=yes",
        "delete=yes",
        "compact=yes",
        "checksum_reserved=yes",
        "exact_fit=yes",
    ]
    for marker in required:
        if marker not in output:
            raise AssertionError(f"missing {marker}\n{output}")

    print("PASS: in-memory slotted leaf V2 codec preserves variable-length geometry")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
