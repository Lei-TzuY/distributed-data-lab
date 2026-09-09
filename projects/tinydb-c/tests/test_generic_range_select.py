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
        print(f"FAIL: missing marker: {marker!r}")
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

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_range_select.db")
    cleanup(db_file)

    first = run_session(
        executable,
        db_file,
        [
            "CREATE TABLE metrics (id INT, name VARCHAR, score INT);",
            "INSERT INTO metrics VALUES (1, 'alpha', 10);",
            "INSERT INTO metrics VALUES (2, 'beta', 20);",
            "INSERT INTO metrics VALUES (3, 'delta', 30);",
            "INSERT INTO metrics VALUES (4, 'kappa', 40);",
            "INSERT INTO metrics VALUES (5, 'mu', 50);",
            "INSERT INTO metrics VALUES (6, 'omega', 60);",
            "INSERT INTO metrics VALUES (7, 'sigma', 70);",
            "INSERT INTO metrics VALUES (8, 'tau', 80);",
            "INSERT INTO metrics VALUES (9, 'upsilon', 90);",
            "INSERT INTO metrics VALUES (10, 'zeta', 100);",
            "SELECT COUNT(*) FROM metrics WHERE score >= 50;",
            "SELECT COUNT(*) FROM metrics WHERE score < 40;",
            "SELECT COUNT(*) FROM metrics WHERE id <= 3;",
            "SELECT COUNT(*) FROM metrics WHERE name > 'omega';",
            "SELECT score FROM metrics WHERE score > 40 LIMIT 2 OFFSET 1;",
            "SELECT name FROM metrics WHERE id = 2;",
            "ALTER TABLE metrics ADD COLUMN stock INT;",
            "SELECT COUNT(*) FROM metrics WHERE stock >= 0;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(first, "db > 6\nExecuted.")
    require_count(first, "db > 3\nExecuted.", 2)
    require(first, "db > 4\nExecuted.")
    require(first, "db > 60\n70\nExecuted.")
    require(first, "db > beta\nExecuted.")
    require(first, "db > 10\nExecuted.")
    require(first, "ok")

    second = run_session(
        executable,
        db_file,
        [
            "SELECT COUNT(*) FROM metrics WHERE score > 90;",
            "SELECT COUNT(*) FROM metrics WHERE name <= 'delta';",
            "SELECT COUNT(*) FROM metrics WHERE stock > 0;",
            "SELECT COUNT(*) FROM metrics WHERE score >= '50';",
            "SELECT COUNT(*) FROM metrics WHERE name < 50;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(second, "db > 1\nExecuted.")
    require(second, "db > 3\nExecuted.")
    require(second, "db > 0\nExecuted.")
    require_count(second, "Syntax error. Could not parse statement.", 2)
    require(second, "ok")

    cleanup(db_file)
    print(
        "PASS: generic SELECT range predicates cover INT/VARCHAR columns, primary "
        "keys, projection with LIMIT/OFFSET, equality fallback, ADD COLUMN, typed "
        "fail-closed behavior, reopen durability, and integrity checks."
    )


if __name__ == "__main__":
    run_test()
