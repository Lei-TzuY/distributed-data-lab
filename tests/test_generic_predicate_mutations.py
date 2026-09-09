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
        os.path.dirname(__file__), "test_generic_predicate_mutations.db"
    )
    cleanup(db_file)

    inserts = []
    for row_id in range(1, 81):
        bucket = "obsolete" if row_id % 2 == 0 else "keep"
        inserts.append(
            f"INSERT INTO items VALUES ({row_id}, '{bucket}', {row_id + 100});"
        )

    first = run_session(
        executable,
        db_file,
        [
            "CREATE TABLE items (id INT, bucket VARCHAR, price INT);",
            *inserts,
            "UPDATE items SET bucket = 'active', price = 42 WHERE bucket = 'keep';",
            "SELECT COUNT(*) FROM items WHERE bucket = 'active';",
            "SELECT COUNT(*) FROM items WHERE price = 42;",
            "DELETE FROM items WHERE bucket = 'obsolete';",
            "SELECT COUNT(*) FROM items;",
            "SELECT COUNT(*) FROM items WHERE bucket = 'obsolete';",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require_count(first, "db > 40\nExecuted.", 3)
    require(first, "db > 0\nExecuted.")
    require(first, "ok")

    second = run_session(
        executable,
        db_file,
        [
            "SELECT COUNT(*) FROM items;",
            "BEGIN;",
            "UPDATE items SET price = 7 WHERE price = 42;",
            "DELETE FROM items WHERE price = 7;",
            "SELECT COUNT(*) FROM items;",
            "ROLLBACK;",
            "SELECT COUNT(*) FROM items;",
            "SELECT COUNT(*) FROM items WHERE price = 42;",
            "UPDATE items SET price = 1 WHERE bucket = 7;",
            "DELETE FROM items WHERE price = '42';",
            "UPDATE items SET id = 500 WHERE bucket = 'active';",
            "ALTER TABLE items ADD COLUMN stock INT;",
            "SELECT COUNT(*) FROM items WHERE stock = 0;",
            "UPDATE items SET stock = 3 WHERE price = 42;",
            "SELECT COUNT(*) FROM items WHERE stock = 3;",
            "DELETE FROM items WHERE stock = 3;",
            "SELECT COUNT(*) FROM items;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require_count(second, "db > 40\nExecuted.", 5)
    require_count(second, "db > 0\nExecuted.", 2)
    require_count(second, "Syntax error. Could not parse statement.", 3)
    require(second, "Column 'stock' added to table 'items'.")
    require(second, "ok")

    third = run_session(
        executable,
        db_file,
        [
            "PRAGMA table_info(items);",
            "SELECT COUNT(*) FROM items;",
            "SELECT COUNT(*) FROM items WHERE stock = 3;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(third, "stock")
    require_count(third, "db > 0\nExecuted.", 2)
    require(third, "ok")

    cleanup(db_file)
    print(
        "PASS: schema-aware generic UPDATE/DELETE predicates, two-phase mutation, "
        "rollback, fixed-slot appended columns, reopen durability, and B+ tree "
        "integrity verified."
    )


if __name__ == "__main__":
    run_test()
