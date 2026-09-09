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


def scalar_results(output):
    return [
        int(value)
        for value in re.findall(r"db > (\d+)\nExecuted\.", output)
    ]


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def require_scalars(output, expected):
    actual = scalar_results(output)
    if actual != expected:
        raise AssertionError(
            f"scalar results {actual!r}, expected {expected!r}\n{output}"
        )


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(
        os.path.dirname(__file__), "test_generic_parenthesized_predicates.db"
    )
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE items (id INT, bucket VARCHAR, price INT);",
                "INSERT INTO items VALUES (1, 'active', 100);",
                "INSERT INTO items VALUES (2, 'active', 1000);",
                "INSERT INTO items VALUES (3, 'active', 3000);",
                "INSERT INTO items VALUES (4, 'obsolete', 4000);",
                "INSERT INTO items VALUES (5, 'obsolete', 1500);",
                "INSERT INTO items VALUES (6, 'obsolete', 200);",
                "INSERT INTO items VALUES (7, 'active', 5000);",
                "INSERT INTO items VALUES (8, 'active', 2000);",
                "CREATE INDEX idx_items_price ON items(price);",
                "EXPLAIN SELECT id FROM items WHERE price >= 3000;",
                "EXPLAIN SELECT id FROM items WHERE (price < 500 OR price >= 3000) AND bucket = 'active';",
                "SELECT COUNT(*) FROM items WHERE price < 500 OR price >= 3000 AND bucket = 'active';",
                "SELECT COUNT(*) FROM items WHERE (price < 500 OR price >= 3000) AND bucket = 'active';",
                "SELECT COUNT(*) FROM items WHERE price < 500 OR (price >= 3000 AND (bucket = 'active' OR id = 4));",
                "SELECT id FROM items WHERE price < 500 OR (price >= 3000 AND (bucket = 'active' OR id = 4)) LIMIT 3;",
                "EXPLAIN ANALYZE SELECT id FROM items WHERE (price < 500 OR price >= 3000) AND bucket = 'active' LIMIT 2;",
                "UPDATE items SET price = 777 WHERE (bucket = 'obsolete' OR id = 1) AND price < 2000;",
                "SELECT COUNT(*) FROM items WHERE price = 777;",
                "SELECT COUNT(*) FROM items WHERE price = 100;",
                "BEGIN;",
                "UPDATE items SET price = 888 WHERE (id = 2 OR id = 3) AND bucket = 'active';",
                "SELECT COUNT(*) FROM items WHERE price = 888;",
                "SELECT COUNT(*) FROM items WHERE price = 777;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM items WHERE price = 888;",
                "SELECT COUNT(*) FROM items WHERE price = 777;",
                "ALTER TABLE items ADD COLUMN stock INT;",
                "SELECT COUNT(*) FROM items WHERE (stock = 0 OR id = 999) AND (bucket = 'active' OR bucket = 'obsolete');",
                "UPDATE items SET stock = 3 WHERE (price = 777 OR id = 2) AND bucket = 'obsolete';",
                "SELECT COUNT(*) FROM items WHERE stock = 3;",
                "DELETE FROM items WHERE (stock = 3 OR price >= 5000) AND (bucket = 'obsolete' OR id = 7);",
                "SELECT COUNT(*) FROM items;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(first, "PLAN: GENERIC SECONDARY INDEX RANGE SCAN")
        require(first, "INDEX: idx_items_price")
        require(first, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(
            first,
            "FILTER: (price < 500 OR price >= 3000) AND bucket = 'active'",
        )
        require(first, "db > 1\n3\n4\nExecuted.")
        require(first, "QUERY PLAN")
        require(first, "ACTUAL RESULT")
        require_scalars(first, [4, 3, 5, 3, 0, 2, 3, 0, 3, 8, 2, 5])
        require(first, "Column 'stock' added to table 'items'.")
        require(first, "ok")

        reopened = run_session(
            executable,
            db_file,
            [
                "PRAGMA table_info(items);",
                "SELECT COUNT(*) FROM items;",
                "SELECT COUNT(*) FROM items WHERE (id = 1 OR id = 4) AND price >= 777;",
                "SELECT COUNT(*) FROM items WHERE (stock = 3 OR id = 999) AND id >= 1;",
                "SELECT COUNT(*) FROM items WHERE id = 3 OR id = 8;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(reopened, "stock")
        require_scalars(reopened, [5, 2, 0, 2])
        require(reopened, "ok")

        invalid = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM items WHERE (price >= 777 OR price = 'bad') AND id >= 1;",
                "UPDATE items SET price = 9 WHERE (bucket = 'active' OR price = 'bad') AND id >= 1;",
                "DELETE FROM items WHERE (missing = 1 OR id = 3) AND id >= 1;",
                "SELECT COUNT(*) FROM items WHERE (id = 1 OR id = 4;",
                ".exit",
            ],
        )
        if invalid.count("Syntax error. Could not parse statement.") != 4:
            raise AssertionError(
                "invalid parenthesized predicates were accepted\n" + invalid
            )

        print(
            "PASS: parenthesized generic boolean predicates override default precedence "
            "across SELECT/UPDATE/DELETE, nested grouping and EXPLAIN ANALYZE work, "
            "ungrouped indexed range routing remains intact, and rollback/reopen/type "
            "safety remain correct."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
