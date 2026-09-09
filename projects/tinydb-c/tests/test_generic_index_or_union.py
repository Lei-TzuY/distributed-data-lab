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

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_or_union.db")
    price_range = db_file + ".idx_products_price.idx.range"
    category_range = db_file + ".idx_products_category.idx.range"
    cleanup(db_file)

    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT, category INT);",
                "INSERT INTO products VALUES (1, 'cheap-active', 100, 1);",
                "INSERT INTO products VALUES (2, 'clearance-mid', 2000, 2);",
                "INSERT INTO products VALUES (3, 'clearance-cheap', 100, 2);",
                "INSERT INTO products VALUES (4, 'premium', 5000, 3);",
                "INSERT INTO products VALUES (5, 'clearance-high', 5000, 2);",
                "INSERT INTO products VALUES (6, 'regular', 700, 4);",
                "CREATE INDEX idx_products_price ON products(price);",
                "CREATE INDEX idx_products_category ON products(category);",
                ".exit",
            ],
        )
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        union = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price < 500 OR category = 2;",
                "SELECT COUNT(*) FROM products WHERE price < 500 OR category = 2;",
                "SELECT name FROM products WHERE price < 500 OR category = 2 LIMIT 2 OFFSET 1;",
                ".exit",
            ],
        )
        require(union, "PLAN: GENERIC INDEX UNION")
        require(union, "BRANCHES: 2")
        require(union, "FILTER: price < 500 OR category = 2")
        require(union, "db > 4\nExecuted.")
        require(union, "clearance-mid")
        require(union, "clearance-cheap")
        if union.count("clearance-cheap") != 1:
            raise AssertionError("OR index union emitted a duplicate PK\n" + union)

        if not os.path.exists(price_range) or not os.path.exists(category_range):
            raise AssertionError("OR index union did not materialize typed range snapshots")

        residual = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price < 500 AND category = 1 OR category = 2 AND price >= 1000;",
                "SELECT COUNT(*) FROM products WHERE price < 500 AND category = 1 OR category = 2 AND price >= 1000;",
                "SELECT name FROM products WHERE price < 500 AND category = 1 OR category = 2 AND price >= 1000;",
                ".exit",
            ],
        )
        require(residual, "PLAN: GENERIC INDEX UNION")
        require(residual, "db > 3\nExecuted.")
        require(residual, "cheap-active")
        require(residual, "clearance-mid")
        require(residual, "clearance-high")
        if "clearance-cheap" in residual:
            raise AssertionError("OR residual revalidation admitted a false candidate\n" + residual)

        mixed = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE id = 4 OR category = 2;",
                "SELECT COUNT(*) FROM products WHERE id = 4 OR category = 2;",
                ".exit",
            ],
        )
        require(mixed, "PLAN: GENERIC INDEX UNION")
        require(mixed, "BRANCHES: 2")
        require(mixed, "db > 4\nExecuted.")

        fallback = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE name = 'premium' OR price < 500;",
                "SELECT COUNT(*) FROM products WHERE name = 'premium' OR price < 500;",
                "EXPLAIN SELECT name FROM products WHERE (price < 500 OR category = 2) AND name = 'cheap-active';",
                ".exit",
            ],
        )
        require(fallback, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(fallback, "db > 3\nExecuted.")
        if fallback.count("PLAN: GENERIC INDEX UNION") != 0:
            raise AssertionError("unsafe/unparenthesized fallback was promoted to index union\n" + fallback)

        mutation = run_session(
            executable,
            db_file,
            [
                "UPDATE products SET category = 2 WHERE id = 6;",
                "SELECT COUNT(*) FROM products WHERE price < 500 OR category = 2;",
                "EXPLAIN ANALYZE SELECT name FROM products WHERE price < 500 OR category = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(mutation, "db > 5\nExecuted.")
        require(mutation, "QUERY PLAN")
        require(mutation, "PLAN: GENERIC INDEX UNION")
        require(mutation, "ACTUAL RESULT")
        require(mutation, "regular")
        require(mutation, "ok")
        require_metrics(mutation)

        print(
            "PASS: generic OR index union combines PK/equality/range anchors across a "
            "mixed VARCHAR/INT schema, deduplicates candidates, revalidates residual "
            "predicates, preserves safe scan fallback, and rebuilds stale snapshots after mutation."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
