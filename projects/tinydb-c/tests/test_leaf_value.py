import os
import subprocess
import sys
import tempfile


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_leaf_value_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_leaf_value_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_leaf_value_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_leaf_value_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError("Could not find tinydb_leaf_value_probe executable")

    with tempfile.TemporaryDirectory() as tempdir:
        database = os.path.join(tempdir, "leaf-value.db")
        result = subprocess.run(
            [probe, database],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=30,
        )

    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError(output)
    if "LEAF_VALUE_OK" not in output:
        raise AssertionError("missing LEAF_VALUE_OK marker\n" + output)
    if "split=yes" not in output or "reopen=yes" not in output:
        raise AssertionError("leaf value split/reopen coverage missing\n" + output)
    if "legacy_layout=yes" not in output or "padding=canonical" not in output:
        raise AssertionError("leaf value compatibility invariant missing\n" + output)
    if "format=v1" not in output:
        raise AssertionError("versioned leaf format descriptor coverage missing\n" + output)

    print("PASS: V1 leaf format and compatibility seam survive split, update, and reopen")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
