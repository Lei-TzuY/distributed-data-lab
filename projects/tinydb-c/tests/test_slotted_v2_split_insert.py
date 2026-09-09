import glob
import os
import subprocess
import sys


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_slotted_v2_split_insert_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_slotted_v2_split_insert_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_slotted_v2_split_insert_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_slotted_v2_split_insert_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError("Could not find tinydb_slotted_v2_split_insert_probe executable")

    db_path = os.path.join(os.path.dirname(__file__), "test_slotted_v2_split_insert.db")
    cleanup(db_path)
    try:
        result = subprocess.run(
            [probe, db_path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=90,
        )
        output = result.stdout + result.stderr
        if result.returncode != 0:
            raise AssertionError(output)
        for marker in (
            "SLOTTED_V2_SPLIT_INSERT_OK",
            "rollback=yes",
            "commit=yes",
            "reopen=yes",
            "parent_update=yes",
            "wal=yes",
            "tail=yes",
            "rightmost=yes",
        ):
            if marker not in output:
                raise AssertionError(f"missing {marker}\n{output}")

        print(
            "PASS: production non-root slotted V2 overflow INSERT publishes both "
            "middle and rightmost tail leaf splits atomically through Pager/WAL, "
            "rolls back cleanly, and survives reopen"
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
