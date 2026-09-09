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

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_or_predicates.db")
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
                "SELECT COUNT(*) FROM items WHERE price < 500 OR price >= 3000 AND bucket = 'active';",
                "SELECT id FROM items WHERE price < 500 OR price >= 3000 AND bucket = 'active' LIMIT 4;",
                "SELECT COUNT(*) FROM items WHERE bucket = 'obsolete' OR id = 2;",
                "SELECT COUNT(*) FROM items WHERE price = 1000;",
                "UPDATE items SET price = 777 WHERE bucket = 'obsolete' OR id = 1;",
                "SELECT COUNT(*) FROM items WHERE price = 777;",
                "SELECT COUNT(*) FROM items WHERE price = 4000;",
                "BEGIN;",
                "UPDATE items SET price = 888 WHERE id = 2 OR price = 777 AND bucket = 'obsolete';",
                "SELECT COUNT(*) FROM items WHERE price = 888;",
                "SELECT COUNT(*) FROM items WHERE price = 777;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM items WHERE price = 888;",
                "SELECT COUNT(*) FROM items WHERE price = 777;",
                "ALTER TABLE items ADD COLUMN stock INT;",
                "SELECT COUNT(*) FROM items WHERE stock = 0 OR id = 999;",
                "UPDATE items SET stock = 3 WHERE price = 777 OR id = 2 AND bucket = 'active';",
                "SELECT COUNT(*) FROM items WHERE stock = 3;",
                "DELETE FROM items WHERE stock = 3 OR price >= 5000 AND bucket = 'active';",
                "SELECT COUNT(*) FROM items;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(first, "db > 1\n3\n6\n7\nExecuted.")
        require_scalars(first, [4, 4, 1, 4, 0, 4, 1, 0, 4, 8, 5, 2])
        require(first, "Column 'stock' added to table 'items'.")
        require(first, "ok")

        reopened = run_session(
            executable,
            db_file,
            [
                "PRAGMA table_info(items);",
                "SELECT COUNT(*) FROM items;",
                "SELECT COUNT(*) FROM items WHERE price = 3000;",
                "SELECT COUNT(*) FROM items WHERE stock = 3 OR id = 999;",
                "SELECT COUNT(*) FROM items WHERE id = 3 OR id = 8;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(reopened, "stock")
        require_scalars(reopened, [2, 1, 0, 2])
        require(reopened, "ok")

        invalid = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM items WHERE price = 3000 OR price = 'bad';",
                "UPDATE items SET price = 9 WHERE bucket = 'active' OR price = 'bad';",
                "DELETE FROM items WHERE missing = 1 OR id = 3;",
                ".exit",
            ],
        )
        if invalid.count("Syntax error. Could not parse statement.") != 3:
            raise AssertionError("typed-invalid OR predicates were accepted\n" + invalid)

        print(
            "PASS: generic OR predicates honor AND-before-OR precedence across SELECT, "
            "UPDATE, and DELETE; indexed-column mutations invalidate equality caches; "
            "transaction rollback, appended columns, reopen durability, and typed errors "
            "remain correct."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
