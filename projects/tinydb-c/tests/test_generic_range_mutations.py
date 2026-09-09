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


def require_count(output, marker, minimum):
    actual = output.count(marker)
    if actual < minimum:
        print(f"FAIL: marker {marker!r} occurred {actual} times, expected >= {minimum}")
        print(output)
        sys.exit(1)


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), "..")
    executable = find_tinydb(base_dir)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(
        os.path.dirname(__file__), "test_generic_range_mutations.db"
    )
    cleanup(db_file)

    inserts = []
    for row_id in range(1, 61):
        if row_id <= 20:
            bucket = "alpha"
        elif row_id <= 40:
            bucket = "middle"
        else:
            bucket = "zulu"
        inserts.append(
            f"INSERT INTO items VALUES ({row_id}, '{bucket}', {100 + row_id});"
        )

    first = run_session(
        executable,
        db_file,
        [
            "CREATE TABLE items (id INT, bucket VARCHAR, price INT);",
            *inserts,
            "ALTER TABLE items ADD COLUMN stock INT;",
            "UPDATE items SET stock = 7 WHERE bucket >= 'middle';",
            "SELECT COUNT(*) FROM items WHERE stock = 7;",
            "UPDATE items SET stock = 3 WHERE id >= 31;",
            "SELECT COUNT(*) FROM items WHERE stock = 3;",
            "SELECT COUNT(*) FROM items WHERE stock = 7;",
            "UPDATE items SET stock = 5 WHERE price < 111;",
            "SELECT COUNT(*) FROM items WHERE stock = 5;",
            "UPDATE items SET stock = 4 WHERE id = 20;",
            "SELECT COUNT(*) FROM items WHERE stock = 4;",
            "UPDATE items SET stock = 99 WHERE price > 999;",
            "SELECT COUNT(*) FROM items WHERE stock = 99;",
            "DELETE FROM items WHERE id < 1;",
            "SELECT COUNT(*) FROM items;",
            "UPDATE items SET stock = 99 WHERE price >= '151';",
            "DELETE FROM items WHERE bucket < 123;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(first, "Column 'stock' added to table 'items'.")
    require(first, "db > 40\nExecuted.")
    require(first, "db > 30\nExecuted.")
    require_count(first, "db > 10\nExecuted.", 2)
    require(first, "db > 1\nExecuted.")
    require(first, "db > 0\nExecuted.")
    require(first, "db > 60\nExecuted.")
    require_count(first, "Syntax error. Could not parse statement.", 2)
    require(first, "ok")

    second = run_session(
        executable,
        db_file,
        [
            "SELECT COUNT(*) FROM items;",
            "BEGIN;",
            "DELETE FROM items WHERE id > 55;",
            "UPDATE items SET stock = 8 WHERE price <= 105;",
            "SELECT COUNT(*) FROM items;",
            "SELECT COUNT(*) FROM items WHERE stock = 8;",
            "ROLLBACK;",
            "SELECT COUNT(*) FROM items;",
            "SELECT COUNT(*) FROM items WHERE stock = 8;",
            "DELETE FROM items WHERE price <= 110;",
            "DELETE FROM items WHERE bucket > 'middle';",
            "SELECT COUNT(*) FROM items;",
            "SELECT COUNT(*) FROM items WHERE stock = 3;",
            "SELECT COUNT(*) FROM items WHERE stock = 7;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require_count(second, "db > 60\nExecuted.", 2)
    require(second, "db > 55\nExecuted.")
    require(second, "db > 5\nExecuted.")
    require(second, "db > 0\nExecuted.")
    require(second, "db > 30\nExecuted.")
    require_count(second, "db > 10\nExecuted.", 2)
    require(second, "ok")

    third = run_session(
        executable,
        db_file,
        [
            "PRAGMA table_info(items);",
            "SELECT COUNT(*) FROM items;",
            "SELECT COUNT(*) FROM items WHERE stock = 3;",
            "SELECT COUNT(*) FROM items WHERE stock = 7;",
            "SELECT COUNT(*) FROM items WHERE stock = 8;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(third, "stock")
    require(third, "db > 30\nExecuted.")
    require_count(third, "db > 10\nExecuted.", 2)
    require(third, "db > 0\nExecuted.")
    require(third, "ok")

    cleanup(db_file)
    print(
        "PASS: generic UPDATE/DELETE < <= > >= predicates, typed INT/VARCHAR "
        "comparison, equality fallback, two-phase mutation, rollback, appended "
        "columns, reopen durability, and B+ tree integrity verified."
    )


if __name__ == "__main__":
    run_test()
