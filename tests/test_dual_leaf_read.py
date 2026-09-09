import glob
import os
import subprocess
import sys


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_dual_leaf_read_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_dual_leaf_read_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_dual_leaf_read_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_dual_leaf_read_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find tinydb_dual_leaf_read_probe executable")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_dual_leaf_read.db")
    cleanup(db_path)
    try:
        result = subprocess.run(
            [probe, db_path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        output = result.stdout + result.stderr
        if result.returncode != 0:
            raise AssertionError(output)
        marker = (
            "PASS: dual-format generic reads traverse mixed fixed-V1/slotted-V2 "
            "leaves across lookup, scan, reopen, accept a versioned schema-bound V2 "
            "row envelope, reject corrupt envelope version/schema/length metadata and "
            "raw schema-length mismatches, and fail closed on out-of-range sibling "
            "corruption without extending the pager; legacy mutation remains rejected."
        )
        if marker not in output:
            raise AssertionError("missing success marker\n" + output)
        print(output.strip())
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
