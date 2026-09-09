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
    process = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if process.returncode != 0:
        raise AssertionError(process.stdout + "\n" + process.stderr)
    return process.stdout + process.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def require_count(output, marker, minimum):
    actual = output.count(marker)
    if actual < minimum:
        raise AssertionError(
            f"marker {marker!r} occurred {actual} times, expected >= {minimum}\n{output}"
        )


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_range.db")
    price_range = db_file + ".idx_products_price.idx.range"
    code_range = db_file + ".idx_labels_code.idx.range"
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                "INSERT INTO products VALUES (1, 'p2', 2);",
                "INSERT INTO products VALUES (2, 'p10', 10);",
                "INSERT INTO products VALUES (3, 'p20', 20);",
                "INSERT INTO products VALUES (4, 'p100', 100);",
                "INSERT INTO products VALUES (5, 'p200', 200);",
                "CREATE INDEX idx_products_price ON products(price);",
                "EXPLAIN SELECT name FROM products WHERE price >= 10;",
                "SELECT COUNT(*) FROM products WHERE price >= 10;",
                "SELECT COUNT(*) FROM products WHERE price < 100;",
                "SELECT COUNT(*) FROM products WHERE price <= 20;",
                "SELECT COUNT(*) FROM products WHERE price > 100;",
                "SELECT name FROM products WHERE price > 10 LIMIT 2 OFFSET 1;",
                "CREATE TABLE labels (id INT, code VARCHAR);",
                "INSERT INTO labels VALUES (1, 'aa');",
                "INSERT INTO labels VALUES (2, 'b');",
                "INSERT INTO labels VALUES (3, 'ba');",
                "INSERT INTO labels VALUES (4, 'z');",
                "CREATE INDEX idx_labels_code ON labels(code);",
                "EXPLAIN SELECT code FROM labels WHERE code > 'b';",
                "SELECT COUNT(*) FROM labels WHERE code > 'b';",
                "SELECT COUNT(*) FROM labels WHERE code < 'ba';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require_count(first, "PLAN: GENERIC SECONDARY INDEX RANGE SCAN", 2)
        require(first, "INDEX: idx_products_price")
        require(first, "FILTER: price >= 10")
        require(first, "INDEX: idx_labels_code")
        require(first, "FILTER: code > 'b'")
        require(first, "db > 4\nExecuted.")
        require_count(first, "db > 3\nExecuted.", 2)
        require_count(first, "db > 1\nExecuted.", 1)
        require_count(first, "db > 2\nExecuted.", 2)
        require(first, "p100")
        require(first, "p200")
        require(first, "ok")

        if not os.path.exists(price_range):
            raise AssertionError("INT range snapshot was not created")
        if not os.path.exists(code_range):
            raise AssertionError("VARCHAR range snapshot was not created")
        price_mtime = os.stat(price_range).st_mtime_ns
        code_mtime = os.stat(code_range).st_mtime_ns

        # Clean reopen should consume typed range snapshots without rewriting.
        time.sleep(1.1)
        reopened = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price >= 10;",
                "SELECT COUNT(*) FROM labels WHERE code > 'b';",
                ".exit",
            ],
        )
        require(reopened, "db > 4\nExecuted.")
        require(reopened, "db > 2\nExecuted.")
        if os.stat(price_range).st_mtime_ns != price_mtime:
            raise AssertionError("clean reopen rewrote INT range snapshot")
        if os.stat(code_range).st_mtime_ns != code_mtime:
            raise AssertionError("clean reopen rewrote VARCHAR range snapshot")

        # Updating one indexed table bumps the shared durable epoch. The old
        # range image stays lazy/stale until the next indexed range query.
        mutation = run_session(
            executable,
            db_file,
            [
                "UPDATE products SET price = 300 WHERE id = 2;",
                ".exit",
            ],
        )
        require(mutation, "Executed.")
        if os.stat(price_range).st_mtime_ns != price_mtime:
            raise AssertionError("mutation eagerly rewrote INT range snapshot")

        time.sleep(1.1)
        rebuilt = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price >= 100;",
                "SELECT COUNT(*) FROM labels WHERE code > 'b';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(rebuilt, "db > 3\nExecuted.")
        require(rebuilt, "db > 2\nExecuted.")
        require(rebuilt, "ok")
        if os.stat(price_range).st_mtime_ns <= price_mtime:
            raise AssertionError("epoch-stale INT range snapshot was not rebuilt")
        if os.stat(code_range).st_mtime_ns <= code_mtime:
            raise AssertionError("database-global epoch did not invalidate VARCHAR range snapshot")

        dropped = run_session(
            executable,
            db_file,
            [
                "DROP INDEX idx_products_price;",
                "EXPLAIN SELECT name FROM products WHERE price >= 100;",
                "SELECT COUNT(*) FROM products WHERE price >= 100;",
                ".exit",
            ],
        )
        require(dropped, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(dropped, "db > 3\nExecuted.")
        if os.path.exists(price_range):
            raise AssertionError("DROP INDEX left the typed range snapshot behind")

        print(
            "PASS: typed INT/VARCHAR persistent secondary-index range scans, planner routing, "
            "reopen reuse, epoch rebuild, and DROP cleanup verified."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
