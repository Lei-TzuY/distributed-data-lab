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

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_and_explain.db")
    cleanup(db_file)

    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                "INSERT INTO products VALUES (1, 'keyboard', 2599);",
                "INSERT INTO products VALUES (2, 'mouse', 1299);",
                "INSERT INTO products VALUES (3, 'cable', 399);",
                "INSERT INTO products VALUES (4, 'monitor', 4999);",
                "CREATE INDEX idx_products_price ON products(price);",
                ".exit",
            ],
        )
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        anchored_range = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 1000 AND price <= 3000;",
                "SELECT name FROM products WHERE price >= 1000 AND price <= 3000;",
                ".exit",
            ],
        )
        require(anchored_range, "PLAN: GENERIC SECONDARY INDEX FUSED RANGE")
        require(anchored_range, "INDEX: idx_products_price")
        require(anchored_range, "RANGE TERMS: 2 on price")
        require(anchored_range, "FILTER: price >= 1000 AND price <= 3000")
        require(anchored_range, "keyboard")
        require(anchored_range, "mouse")
        if "ACTUAL RESULT" in anchored_range:
            raise AssertionError("plain compound EXPLAIN executed the query\n" + anchored_range)

        pk = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE id = 2 AND price >= 1000;",
                ".exit",
            ],
        )
        require(pk, "PLAN: GENERIC PRIMARY KEY LOOKUP")
        require(pk, "FILTER: id = 2 AND price >= 1000")
        if "SECONDARY INDEX + RESIDUAL" in pk or "SECONDARY INDEX FUSED RANGE" in pk:
            raise AssertionError("secondary index incorrectly displaced PK equality\n" + pk)

        indexed_residual = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price = 1299 AND id > 1;",
                "SELECT name FROM products WHERE price = 1299 AND id > 1;",
                ".exit",
            ],
        )
        require(indexed_residual, "PLAN: GENERIC SECONDARY INDEX + RESIDUAL FILTER")
        require(indexed_residual, "INDEX: idx_products_price")
        require(indexed_residual, "ANCHOR: price = 1299")
        require(indexed_residual, "FILTER: price = 1299 AND id > 1")
        require(indexed_residual, "mouse")

        disjunction = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 4000 OR price <= 500;",
                "SELECT name FROM products WHERE price >= 4000 OR price <= 500;",
                ".exit",
            ],
        )
        require(disjunction, "PLAN: GENERIC INDEX UNION")
        require(disjunction, "BRANCHES: 2")
        require(disjunction, "FILTER: price >= 4000 OR price <= 500")
        require(disjunction, "monitor")
        require(disjunction, "cable")

        analyzed = run_session(
            executable,
            db_file,
            [
                "EXPLAIN ANALYZE SELECT name FROM products WHERE price >= 1000 AND price <= 3000;",
                "EXPLAIN ANALYZE SELECT name FROM products WHERE id = 1 OR price >= 4000;",
                ".exit",
            ],
        )
        require(analyzed, "QUERY PLAN")
        require(analyzed, "PLAN: GENERIC SECONDARY INDEX FUSED RANGE")
        require(analyzed, "PLAN: GENERIC INDEX UNION")
        require(analyzed, "FILTER: price >= 1000 AND price <= 3000")
        require(analyzed, "FILTER: id = 1 OR price >= 4000")
        require(analyzed, "ACTUAL RESULT")
        require(analyzed, "keyboard")
        require(analyzed, "mouse")
        require(analyzed, "monitor")
        require_metrics(analyzed)

        invalid = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT * FROM products WHERE price >= 1000 AND price <= '3000';",
                ".exit",
            ],
        )
        if invalid.count("Syntax error. Could not parse statement.") < 1:
            raise AssertionError("invalid typed EXPLAIN predicate was accepted\n" + invalid)

        print(
            "PASS: generic EXPLAIN/ANALYZE fuses compatible same-index range terms, "
            "keeps residual filtering for mixed predicates, preserves PK equality priority, "
            "promotes fully-indexable OR predicates to index union, and rejects invalid typed filters."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
