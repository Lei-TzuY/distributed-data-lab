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


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def require_metrics(output):
    pattern = re.compile(
        r"ANALYZE: execution_time_ms=[0-9.]+ "
        r"cache_hits=\d+ cache_misses=\d+ evictions=\d+ page_accesses=\d+"
    )
    if pattern.search(output) is None:
        raise AssertionError(f"missing ANALYZE metrics\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_explain.db")
    cleanup(db_file)

    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                "INSERT INTO users VALUES (42, 'legacy-user', 'legacy@example.com');",
                "INSERT INTO products VALUES (1, 'keyboard', 2599);",
                "INSERT INTO products VALUES (2, 'mouse', 1299);",
                "INSERT INTO products VALUES (3, 'cable', 399);",
                ".exit",
            ],
        )
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        plain_pk = run_session(
            executable,
            db_file,
            ["EXPLAIN SELECT name FROM products WHERE id = 2;", ".exit"],
        )
        require(plain_pk, "PLAN: GENERIC PRIMARY KEY LOOKUP")
        require(plain_pk, "TABLE: products (root page ")
        require(plain_pk, "PROJECTION: name")
        require(plain_pk, "FILTER: id = 2")
        if "ACTUAL RESULT" in plain_pk or "db > mouse\n" in plain_pk:
            raise AssertionError("plain generic EXPLAIN executed the query\n" + plain_pk)

        plain_scan = run_session(
            executable,
            db_file,
            ["EXPLAIN SELECT name FROM products WHERE price = 1299;", ".exit"],
        )
        require(plain_scan, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(plain_scan, "PROJECTION: name")
        require(plain_scan, "FILTER: price = 1299")
        if "ACTUAL RESULT" in plain_scan or "db > mouse\n" in plain_scan:
            raise AssertionError("plain generic scan EXPLAIN executed the query\n" + plain_scan)

        plain_range = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 1000 LIMIT 2;",
                "EXPLAIN SELECT COUNT(*) FROM products WHERE name < 'mouse';",
                "EXPLAIN SELECT * FROM products WHERE id > 1;",
                ".exit",
            ],
        )
        require(plain_range, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(plain_range, "FILTER: price >= 1000")
        require(plain_range, "FILTER: name < 'mouse'")
        require(plain_range, "FILTER: id > 1")
        if "ACTUAL RESULT" in plain_range:
            raise AssertionError("plain generic range EXPLAIN executed a query\n" + plain_range)

        analyze_pk = run_session(
            executable,
            db_file,
            ["EXPLAIN ANALYZE SELECT name FROM products WHERE id = 2;", ".exit"],
        )
        require(analyze_pk, "QUERY PLAN")
        require(analyze_pk, "PLAN: GENERIC PRIMARY KEY LOOKUP")
        require(analyze_pk, "ACTUAL RESULT")
        require(analyze_pk, "mouse")
        require_metrics(analyze_pk)

        analyze_scan = run_session(
            executable,
            db_file,
            ["EXPLAIN ANALYZE SELECT * FROM products WHERE price = 399;", ".exit"],
        )
        require(analyze_scan, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(analyze_scan, "ACTUAL RESULT")
        require(analyze_scan, "(3, cable, 399)")
        require_metrics(analyze_scan)

        analyze_range = run_session(
            executable,
            db_file,
            [
                "EXPLAIN ANALYZE SELECT name FROM products WHERE price >= 1000 LIMIT 1;",
                ".exit",
            ],
        )
        require(analyze_range, "QUERY PLAN")
        require(analyze_range, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(analyze_range, "FILTER: price >= 1000")
        require(analyze_range, "ACTUAL RESULT")
        require(analyze_range, "keyboard")
        require_metrics(analyze_range)

        invalid = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT * FROM products WHERE price = '1299';",
                "EXPLAIN SELECT * FROM products WHERE price >= '1299';",
                ".exit",
            ],
        )
        if invalid.count("Syntax error. Could not parse statement.") < 2:
            raise AssertionError("typed invalid filters were not rejected\n" + invalid)
        if "PLAN: GENERIC" in invalid:
            raise AssertionError("invalid typed generic filter produced a plan\n" + invalid)

        legacy = run_session(
            executable,
            db_file,
            ["EXPLAIN ANALYZE SELECT * FROM users WHERE id = 42;", ".exit"],
        )
        require(legacy, "QUERY PLAN")
        require(legacy, "ACTUAL RESULT")
        require(legacy, "(42, legacy-user, legacy@example.com)")
        require_metrics(legacy)
        if "PLAN: GENERIC" in legacy:
            raise AssertionError("legacy users query was incorrectly captured by generic planner\n" + legacy)

        print(
            "PASS: generic EXPLAIN/ANALYZE reports equality PK lookup versus "
            "typed equality/range scans while preserving legacy profiling."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
