import glob
import os
import re
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


def scalar_results(output):
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def reject(output, marker):
    if marker in output:
        raise AssertionError(f"unexpected marker {marker!r}\n{output}")


def require_scalars(output, expected):
    actual = scalar_results(output)
    if actual != expected:
        raise AssertionError(f"scalar results {actual!r}, expected {expected!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    source_path = os.path.join(repo_root, "src", "generic_wide_boolean_index_route.c")
    with open(source_path, "r", encoding="utf-8") as handle:
        source = handle.read()
    for marker in [
        "wide_candidate_union",
        "wide_candidate_intersection",
        "tinydb_generic_index_collect_candidates",
        "tinydb_record_payload_find",
        "tinydb_generic_boolean_matches",
    ]:
        if marker not in source:
            raise AssertionError(f"wide boolean candidate seam missing {marker}")

    db_file = os.path.join(os.path.dirname(__file__), "test_wide_boolean_index_algebra.db")
    cleanup(db_file)
    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE wide_docs (id INT, bucket VARCHAR(145), note VARCHAR(145), price INT);",
                "INSERT INTO wide_docs VALUES (1, 'cold', 'alpha', 100);",
                "INSERT INTO wide_docs VALUES (2, 'hot', 'beta', 200);",
                "INSERT INTO wide_docs VALUES (3, 'hot', 'special', 900);",
                "INSERT INTO wide_docs VALUES (4, 'cold', 'special', 1000);",
                "INSERT INTO wide_docs VALUES (5, 'hot', 'special', 50);",
                "INSERT INTO wide_docs VALUES (6, 'warm', 'misc', 500);",
                "INSERT INTO wide_docs VALUES (7, 'hot', 'misc', 850);",
                "INSERT INTO wide_docs VALUES (8, 'cold', 'misc', 150);",
                "CREATE INDEX idx_wide_price ON wide_docs (price);",
                "CREATE INDEX idx_wide_bucket ON wide_docs (bucket);",
                ".exit",
            ],
        )
        reject(setup, "Error:")
        reject(setup, "Syntax error")

        first = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT COUNT(*) FROM wide_docs WHERE price <= 150 OR price >= 850;",
                "SELECT COUNT(*) FROM wide_docs WHERE price <= 150 OR price >= 850;",
                "EXPLAIN SELECT COUNT(*) FROM wide_docs WHERE id = 2 OR price >= 900;",
                "SELECT COUNT(*) FROM wide_docs WHERE id = 2 OR price >= 900;",
                "EXPLAIN SELECT COUNT(*) FROM wide_docs WHERE price <= 150 OR note = 'special';",
                "SELECT COUNT(*) FROM wide_docs WHERE price <= 150 OR note = 'special';",
                "EXPLAIN SELECT COUNT(*) FROM wide_docs WHERE (price <= 150 OR price >= 850) AND note = 'special';",
                "SELECT COUNT(*) FROM wide_docs WHERE (price <= 150 OR price >= 850) AND note = 'special';",
                "EXPLAIN SELECT COUNT(*) FROM wide_docs WHERE (price <= 150 OR price >= 850) AND bucket = 'hot';",
                "SELECT COUNT(*) FROM wide_docs WHERE (price <= 150 OR price >= 850) AND bucket = 'hot';",
                "EXPLAIN SELECT COUNT(*) FROM wide_docs WHERE ((price <= 150 OR price >= 850) AND bucket = 'hot') OR id = 6;",
                "SELECT COUNT(*) FROM wide_docs WHERE ((price <= 150 OR price >= 850) AND bucket = 'hot') OR id = 6;",
                "SELECT id FROM wide_docs WHERE price <= 150 OR price >= 850 LIMIT 2 OFFSET 1;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject(first, "fixed B+ tree value slot")
        reject(first, "unable to decode")
        require(first, "PLAN: GENERIC INDEX UNION")
        require(first, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(first, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require_scalars(first, [6, 3, 5, 3, 3, 4])
        require(first, "3\n4\nExecuted.")
        require(first, "ok")

        reopened = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT COUNT(*) FROM wide_docs WHERE price <= 150 OR price >= 850;",
                "SELECT COUNT(*) FROM wide_docs WHERE price <= 150 OR price >= 850;",
                "EXPLAIN SELECT COUNT(*) FROM wide_docs WHERE (price <= 150 OR price >= 850) AND bucket = 'hot';",
                "SELECT COUNT(*) FROM wide_docs WHERE (price <= 150 OR price >= 850) AND bucket = 'hot';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject(reopened, "fixed B+ tree value slot")
        reject(reopened, "unable to decode")
        require(reopened, "PLAN: GENERIC INDEX UNION")
        require(reopened, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require_scalars(reopened, [6, 3])
        require(reopened, "ok")

        print(
            "PASS: wide OR/grouped predicates use safe payload-native index candidate "
            "union/intersection algebra, preserve residual rechecks and PK branches, "
            "fall back when an OR branch is unbounded, and survive reopen."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
