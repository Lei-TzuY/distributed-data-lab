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
    result = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        print("FAIL: tinydb returned", result.returncode)
        print(result.stdout)
        print(result.stderr)
        sys.exit(1)
    return result.stdout + result.stderr


def require(output, marker):
    if marker not in output:
        print(f"FAIL: missing marker: {marker}")
        print(output)
        sys.exit(1)


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), "..")
    executable = find_tinydb(base_dir)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_mutation_stress.db")
    cleanup(db_file)

    commands = ["CREATE TABLE metrics (id INT, value INT, tag VARCHAR);"]
    commands.extend(
        f"INSERT INTO metrics VALUES ({i}, {i * 10}, 'tag-{i}');"
        for i in range(1, 61)
    )
    commands.extend(
        f"UPDATE metrics SET value = {i * 100}, tag = 'updated-{i}' WHERE id = {i};"
        for i in range(50, 61)
    )
    commands.extend(
        f"DELETE FROM metrics WHERE id = {i};"
        for i in range(1, 56)
    )
    commands.extend([
        "SELECT COUNT(*) FROM metrics;",
        "SELECT * FROM metrics WHERE id = 60;",
        ".stats metrics",
        "PRAGMA integrity_check;",
        ".exit",
    ])

    first = run_session(executable, db_file, commands)
    require(first, "db > 5\nExecuted.")
    require(first, "(60, 6000, updated-60)")
    require(first, "Rows: 5")
    require(first, "Height: 1")
    require(first, "ok")

    second = run_session(
        executable,
        db_file,
        [
            "SELECT COUNT(*) FROM metrics;",
            "SELECT * FROM metrics WHERE id = 56;",
            "SELECT * FROM metrics WHERE id = 60;",
            "PRAGMA integrity_check;",
            "DELETE FROM metrics;",
            "SELECT COUNT(*) FROM metrics;",
            ".stats metrics",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )
    require(second, "db > 5\nExecuted.")
    require(second, "(56, 5600, updated-56)")
    require(second, "(60, 6000, updated-60)")
    require(second, "db > 0\nExecuted.")
    require(second, "Rows: 0")
    if second.count("ok") < 2:
        print("FAIL: expected integrity_check before and after generic delete-all")
        print(second)
        cleanup(db_file)
        sys.exit(1)

    third = run_session(
        executable,
        db_file,
        [
            "SELECT COUNT(*) FROM metrics;",
            ".stats metrics",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )
    require(third, "db > 0\nExecuted.")
    require(third, "Rows: 0")
    require(third, "Height: 1")
    require(third, "ok")

    cleanup(db_file)
    print("PASS: generic records survive split, update, merge, root collapse, delete-all and reopen.")


if __name__ == "__main__":
    run_test()
