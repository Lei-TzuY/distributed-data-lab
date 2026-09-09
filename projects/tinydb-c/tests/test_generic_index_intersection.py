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


def scalar_results(output):
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def require_metrics(output):
    pattern = re.compile(
        r"ANALYZE: execution_time_ms=[0-9.]+ "
        r"cache_hits=\d+ cache_misses=\d+ evictions=\d+ page_accesses=\d+"
    )
    if pattern.search(output) is None:
        raise AssertionError("missing EXPLAIN ANALYZE metrics\n" + output)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_intersection.db")
    cleanup(db_file)

    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT, stock INT, category INT);",
                "INSERT INTO products VALUES (1, 'cheap', 100, 0, 1);",
                "INSERT INTO products VALUES (2, 'mid-a', 1500, 0, 1);",
                "INSERT INTO products VALUES (3, 'mid-b', 2500, 5, 1);",
                "INSERT INTO products VALUES (4, 'high-a', 5000, 0, 2);",
                "INSERT INTO products VALUES (5, 'high-b', 7000, 3, 2);",
                "INSERT INTO products VALUES (6, 'mid-c', 1800, 0, 2);",
                "INSERT INTO products VALUES (7, 'low-b', 300, 2, 2);",
                "INSERT INTO products VALUES (8, 'high-c', 9000, 0, 1);",
                "CREATE INDEX idx_products_price ON products(price);",
                "CREATE INDEX idx_products_stock ON products(stock);",
                "CREATE INDEX idx_products_category ON products(category);",
                ".exit",
            ],
        )
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        two_way = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 1000 AND stock = 0;",
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND stock = 0;",
                "SELECT name FROM products WHERE price >= 1000 AND stock = 0 LIMIT 2 OFFSET 1;",
                ".exit",
            ],
        )
        require(two_way, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(two_way, "INDEXES: 2")
        require(two_way, "FILTER: price >= 1000 AND stock = 0")
        if scalar_results(two_way) != [4]:
            raise AssertionError("two-way intersection returned wrong count\n" + two_way)
        require(two_way, "high-a")
        require(two_way, "mid-c")
        if "mid-a\nExecuted." in two_way:
            raise AssertionError("LIMIT/OFFSET did not apply after intersection matching\n" + two_way)

        three_way = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 1000 AND stock = 0 AND category = 2;",
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND stock = 0 AND category = 2;",
                ".exit",
            ],
        )
        require(three_way, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(three_way, "INDEXES: 3")
        require(three_way, "FILTER: price >= 1000 AND stock = 0 AND category = 2")
        if scalar_results(three_way) != [2]:
            raise AssertionError("three-way intersection returned wrong count\n" + three_way)

        like_residual = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 1000 AND stock = 0 AND name LIKE 'high%';",
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND stock = 0 AND name LIKE 'high%';",
                "SELECT name FROM products WHERE price >= 1000 AND stock = 0 AND name LIKE 'high%';",
                ".exit",
            ],
        )
        require(like_residual, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(like_residual, "INDEXES: 2")
        require(like_residual, "FILTER: price >= 1000 AND stock = 0 AND name LIKE 'high%'")
        if scalar_results(like_residual) != [2]:
            raise AssertionError("LIKE residual changed intersection result\n" + like_residual)
        require(like_residual, "high-a")
        require(like_residual, "high-c")
        if "mid-a\n" in like_residual or "mid-c\n" in like_residual:
            raise AssertionError("LIKE residual was not revalidated after intersection\n" + like_residual)

        fallbacks = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 1000 AND name > 'm';",
                "EXPLAIN SELECT name FROM products WHERE price >= 1000 AND price <= 3000;",
                "EXPLAIN SELECT name FROM products WHERE id = 4 AND price >= 1000 AND stock = 0;",
                ".exit",
            ],
        )
        require(fallbacks, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(fallbacks, "FILTER: price >= 1000 AND name > 'm'")
        require(fallbacks, "PLAN: GENERIC SECONDARY INDEX FUSED RANGE")
        require(fallbacks, "RANGE TERMS: 2 on price")
        require(fallbacks, "FILTER: price >= 1000 AND price <= 3000")
        require(fallbacks, "PLAN: GENERIC PRIMARY KEY LOOKUP")
        if "PLAN: GENERIC SECONDARY INDEX INTERSECTION\n  TABLE: products" in fallbacks:
            raise AssertionError("unsafe/single-index predicate was promoted to intersection\n" + fallbacks)

        mutation = run_session(
            executable,
            db_file,
            [
                "UPDATE products SET stock = 0 WHERE id = 5;",
                "SELECT COUNT(*) FROM products WHERE price >= 5000 AND stock = 0 AND category = 2;",
                "EXPLAIN ANALYZE SELECT name FROM products WHERE price >= 5000 AND stock = 0 AND category = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if scalar_results(mutation) != [2]:
            raise AssertionError("intersection did not rebuild stale candidates after mutation\n" + mutation)
        require(mutation, "QUERY PLAN")
        require(mutation, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(mutation, "INDEXES: 3")
        require(mutation, "ACTUAL RESULT")
        require(mutation, "high-a")
        require(mutation, "high-b")
        require(mutation, "ok")
        require_metrics(mutation)

        reopened = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price >= 5000 AND stock = 0 AND category = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if scalar_results(reopened) != [2]:
            raise AssertionError("intersection result did not survive reopen\n" + reopened)
        require(reopened, "ok")

        print(
            "PASS: flat generic AND queries use cost-aware intersection or scan fallback, "
            "fuse compatible terms on one index, revalidate full predicates including residual "
            "LIKE, preserve PK-equality priority, rebuild stale snapshots after mutation, and "
            "remain correct after reopen."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
