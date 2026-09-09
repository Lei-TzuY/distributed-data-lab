import glob
import os
import subprocess
import sys
import time


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


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_anchor_choice.db")
    category_range = db_file + ".idx_products_category.idx.range"
    price_range = db_file + ".idx_products_price.idx.range"
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, category VARCHAR, price INT);",
                "INSERT INTO products VALUES (1, 'hot', 5);",
                "INSERT INTO products VALUES (2, 'cold', 10);",
                "INSERT INTO products VALUES (3, 'hot', 20);",
                "INSERT INTO products VALUES (4, 'hot', 100);",
                "INSERT INTO products VALUES (5, 'cold', 200);",
                "CREATE INDEX idx_products_price ON products(price);",
                "CREATE INDEX idx_products_category ON products(category);",
                "EXPLAIN SELECT id FROM products WHERE price >= 10 AND category = 'hot';",
                "SELECT COUNT(*) FROM products WHERE price >= 10 AND category = 'hot';",
                "EXPLAIN SELECT id FROM products WHERE price >= 10 AND category > 'cold';",
                "SELECT COUNT(*) FROM products WHERE price >= 10 AND category > 'cold';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(first, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(first, "INDEXES: 2")
        require(first, "FILTER: price >= 10 AND category = 'hot'")
        require(first, "FILTER: price >= 10 AND category > 'cold'")
        require(first, "db > 2\nExecuted.")
        require(first, "ok")

        if not os.path.exists(category_range):
            raise AssertionError("intersection did not create category typed snapshot")
        if not os.path.exists(price_range):
            raise AssertionError("intersection did not create price typed snapshot")
        category_mtime = os.stat(category_range).st_mtime_ns
        price_mtime = os.stat(price_range).st_mtime_ns

        time.sleep(1.1)
        reopened = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price >= 10 AND category = 'hot';",
                "SELECT COUNT(*) FROM products WHERE price >= 10 AND category > 'cold';",
                ".exit",
            ],
        )
        require(reopened, "db > 2\nExecuted.")
        if os.stat(category_range).st_mtime_ns != category_mtime:
            raise AssertionError("clean reopen rewrote category intersection snapshot")
        if os.stat(price_range).st_mtime_ns != price_mtime:
            raise AssertionError("clean reopen rewrote price intersection snapshot")

        mutation = run_session(
            executable,
            db_file,
            [
                "UPDATE products SET price = 1 WHERE id = 3;",
                "SELECT COUNT(*) FROM products WHERE price >= 10 AND category = 'hot';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(mutation, "db > 1\nExecuted.")
        require(mutation, "ok")
        if os.stat(category_range).st_mtime_ns <= category_mtime:
            raise AssertionError("epoch-stale category intersection snapshot was not rebuilt")
        if os.stat(price_range).st_mtime_ns <= price_mtime:
            raise AssertionError("epoch-stale price intersection snapshot was not rebuilt")

        dropped = run_session(
            executable,
            db_file,
            [
                "DROP INDEX idx_products_category;",
                "EXPLAIN SELECT id FROM products WHERE price >= 10 AND category = 'hot';",
                "EXPLAIN SELECT id FROM products WHERE price >= 10 AND price = 100;",
                ".exit",
            ],
        )
        require(dropped, "PLAN: GENERIC SECONDARY INDEX + RESIDUAL FILTER")
        require(dropped, "INDEX: idx_products_price")
        require(dropped, "ANCHOR: price >= 10")
        require(dropped, "PLAN: GENERIC SECONDARY INDEX FUSED RANGE")
        require(dropped, "RANGE TERMS: 2 on price")
        require(dropped, "FILTER: price >= 10 AND price = 100")
        if "PLAN: GENERIC SECONDARY INDEX INTERSECTION" in dropped:
            raise AssertionError("intersection remained active with only one usable index\n" + dropped)
        if os.path.exists(category_range):
            raise AssertionError("DROP INDEX left category range snapshot behind")

        print(
            "PASS: flat AND predicates intersect distinct secondary indexes, reuse typed "
            "snapshots after reopen, rebuild both sources on epoch change, fall back after "
            "DROP INDEX, and fuse compatible terms on the one remaining index."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
