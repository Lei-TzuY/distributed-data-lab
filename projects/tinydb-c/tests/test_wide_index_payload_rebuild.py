import glob
import os
import subprocess
import sys


def find_tinydb(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_file, commands):
    result = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def reject(output, marker):
    if marker in output:
        raise AssertionError(f"unexpected marker {marker!r}\n{output}")


def remove_range_snapshots(db_file):
    for path in glob.glob(db_file + "*.range"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    source_path = os.path.join(repo_root, "src", "generic_index_candidates.c")
    with open(source_path, "r", encoding="utf-8") as handle:
        source = handle.read()
    for marker in [
        "tinydb_record_payload_scan",
        "build_payload_entry",
        "tinydb_record_payload_decode_values",
        "scan_complete",
        "discard_snapshot",
    ]:
        if marker not in source:
            raise AssertionError(f"payload-native candidate rebuild seam missing {marker}")

    db_file = os.path.join(os.path.dirname(__file__), "test_wide_index_payload_rebuild.db")
    cleanup(db_file)
    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE wide_docs (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_docs VALUES (10, 'left-a', 'right-a');",
                "INSERT INTO wide_docs VALUES (20, 'left-b', 'right-b');",
                "ALTER TABLE wide_docs ADD COLUMN score INT;",
                "INSERT INTO wide_docs VALUES (30, 'left-c', 'right-c', 7);",
                "CREATE INDEX idx_wide_score ON wide_docs (score);",
                ".exit",
            ],
        )
        reject(setup, "Error:")
        reject(setup, "Syntax error")

        # Force the ordered candidate sidecar to be rebuilt from the table rows.
        # Rows 10/20 are older compact schema generations and materialize score=0.
        remove_range_snapshots(db_file)
        indexed = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT * FROM wide_docs WHERE score = 0;",
                "SELECT * FROM wide_docs WHERE score = 0;",
                "SELECT * FROM wide_docs WHERE score = 7;",
                ".exit",
            ],
        )
        reject(indexed, "fixed B+ tree value slot")
        reject(indexed, "unable to load or rebuild typed generic index snapshot")
        reject(indexed, "unable to decode")
        require(indexed, "INDEX: idx_wide_score")
        require(indexed, "(10, left-a, right-a, 0)")
        require(indexed, "(20, left-b, right-b, 0)")
        require(indexed, "(30, left-c, right-c, 7)")

        snapshots = glob.glob(db_file + "*.range")
        if not snapshots:
            raise AssertionError("wide indexed query did not publish a rebuilt range snapshot")

        # Mutation advances the generic-index epoch. The stale sidecar must not be
        # reused; the next indexed lookup has to rebuild from payload rows again.
        mutated = run_session(
            executable,
            db_file,
            [
                "UPDATE wide_docs SET score = 5 WHERE id = 10;",
                "SELECT * FROM wide_docs WHERE score = 5;",
                "SELECT * FROM wide_docs WHERE score = 0;",
                ".exit",
            ],
        )
        reject(mutated, "fixed B+ tree value slot")
        reject(mutated, "unable to load or rebuild typed generic index snapshot")
        require(mutated, "(10, left-a, right-a, 5)")
        require(mutated, "(20, left-b, right-b, 0)")

        reopened = run_session(
            executable,
            db_file,
            [
                "SELECT * FROM wide_docs WHERE score = 7;",
                "SELECT * FROM wide_docs WHERE score = 5;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject(reopened, "fixed B+ tree value slot")
        reject(reopened, "unable to load or rebuild typed generic index snapshot")
        require(reopened, "(30, left-c, right-c, 7)")
        require(reopened, "(10, left-a, right-a, 5)")
        require(reopened, "ok")

        print(
            "PASS: wide secondary-index snapshots rebuild through schema-sized payload "
            "rows, index append-generation defaults, reject the fixed carrier seam, "
            "refresh after epoch invalidation, survive reopen, and preserve integrity."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
