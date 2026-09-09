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
    process = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if process.returncode != 0:
        print("FAIL: tinydb returned", process.returncode)
        print(process.stdout)
        print(process.stderr)
        sys.exit(1)
    return process.stdout + process.stderr


def require(output, marker):
    if marker not in output:
        print(f"FAIL: missing marker: {marker}")
        print(output)
        sys.exit(1)


def require_count(output, marker, expected):
    actual = output.count(marker)
    if actual != expected:
        print(f"FAIL: marker {marker!r} occurred {actual} times, expected {expected}")
        print(output)
        sys.exit(1)


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), "..")
    executable = find_tinydb(base_dir)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_and_predicates.db")
    cleanup(db_file)

    inserts = []
    for row_id in range(1, 61):
        if row_id % 3 == 0:
            bucket = "z"
        elif row_id % 3 == 1:
            bucket = "a"
        else:
            bucket = "m"
        inserts.append(
            f"INSERT INTO items VALUES ({row_id}, '{bucket}', {row_id + 100});"
        )

    first = run_session(
        executable,
        db_file,
        [
            "CREATE TABLE items (id INT, bucket VARCHAR, price INT);",
            *inserts,
            "SELECT COUNT(*) FROM items WHERE price >= 120 AND price <= 130;",
            "SELECT id FROM items WHERE price >= 120 AND price <= 130 LIMIT 3 OFFSET 2;",
            "UPDATE items SET bucket = 'window' WHERE price >= 120 AND price < 130;",
            "SELECT COUNT(*) FROM items WHERE bucket = 'window';",
            "DELETE FROM items WHERE bucket = 'z' AND price >= 140;",
            "SELECT COUNT(*) FROM items;",
            "UPDATE items SET price = 777 WHERE id = 5 AND bucket = 'm';",
            "SELECT price FROM items WHERE id = 5;",
            "SELECT COUNT(*) FROM items WHERE id = 5 AND price >= 700;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(first, "db > 11\nExecuted.")
    require(first, "db > 22\n23\n24\nExecuted.")
    require(first, "db > 10\nExecuted.")
    require(first, "db > 53\nExecuted.")
    require(first, "db > 777\nExecuted.")
    require(first, "db > 1\nExecuted.")
    require(first, "ok")

    second = run_session(
        executable,
        db_file,
        [
            "BEGIN;",
            "DELETE FROM items WHERE price >= 150 AND price <= 155;",
            "SELECT COUNT(*) FROM items;",
            "ROLLBACK;",
            "SELECT COUNT(*) FROM items;",
            "ALTER TABLE items ADD COLUMN stock INT;",
            "SELECT COUNT(*) FROM items WHERE stock = 0 AND price < 120;",
            "UPDATE items SET stock = 3 WHERE price >= 110 AND price < 115;",
            "SELECT COUNT(*) FROM items WHERE stock = 3 AND id >= 12;",
            "DELETE FROM items WHERE stock = 3 AND id >= 12;",
            "SELECT COUNT(*) FROM items;",
            "SELECT COUNT(*) FROM items WHERE stock = 3;",
            "SELECT COUNT(*) FROM items WHERE price >= 120 OR price <= 130;",
            "SELECT id FROM items WHERE bucket = 'window' AND price < 125 OR id = 5 LIMIT 4;",
            "UPDATE items SET bucket = 'bad' WHERE bucket = 7 AND price > 0;",
            "DELETE FROM items WHERE missing = 1 AND price > 0;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(second, "db > 49\nExecuted.")
    require(second, "db > 53\nExecuted.")
    require(second, "Column 'stock' added to table 'items'.")
    require(second, "db > 18\nExecuted.")
    require(second, "db > 3\nExecuted.")
    require_count(second, "db > 50\nExecuted.", 2)
    require(second, "db > 2\nExecuted.")
    require(second, "db > 5\n20\n21\n22\nExecuted.")
    require_count(second, "Syntax error. Could not parse statement.", 2)
    require(second, "ok")

    third = run_session(
        executable,
        db_file,
        [
            "PRAGMA table_info(items);",
            "SELECT COUNT(*) FROM items;",
            "SELECT COUNT(*) FROM items WHERE stock = 3;",
            "SELECT COUNT(*) FROM items WHERE stock = 0 AND price < 120;",
            "SELECT COUNT(*) FROM items WHERE id = 5 OR bucket = 'window';",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(third, "stock")
    require(third, "db > 50\nExecuted.")
    require(third, "db > 2\nExecuted.")
    require(third, "db > 13\nExecuted.")
    require(third, "db > 11\nExecuted.")
    require(third, "ok")

    cleanup(db_file)
    print(
        "PASS: shared typed generic predicates execute equality/range AND filters and "
        "AND-before-OR SELECT semantics across appended columns and reopen durability, "
        "while UPDATE/DELETE retain two-phase mutation and rollback behavior."
    )


if __name__ == "__main__":
    run_test()
