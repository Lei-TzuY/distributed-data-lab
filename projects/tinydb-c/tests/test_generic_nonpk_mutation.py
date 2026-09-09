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
        timeout=180,
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

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_nonpk_mutation.db")
    cleanup(db_file)

    try:
        setup = ["CREATE TABLE metrics (id INT, tag VARCHAR, value INT);"]
        setup.extend(
            f"INSERT INTO metrics VALUES ({i}, '{'hot' if i <= 50 else 'cold'}', {i});"
            for i in range(1, 61)
        )
        setup.extend([
            "UPDATE metrics SET value = 999 WHERE tag = 'hot';",
            "SELECT COUNT(*) FROM metrics WHERE value = 999;",
            "SELECT * FROM metrics WHERE id = 1;",
            "DELETE FROM metrics WHERE value = 999;",
            "SELECT COUNT(*) FROM metrics;",
            ".stats metrics",
            "BEGIN;",
            "UPDATE metrics SET value = 777 WHERE tag = 'cold';",
            "DELETE FROM metrics WHERE value = 777;",
            "SELECT COUNT(*) FROM metrics;",
            "ROLLBACK;",
            "SELECT COUNT(*) FROM metrics;",
            "SELECT * FROM metrics WHERE id = 60;",
            "UPDATE metrics SET value = 123 WHERE tag = 'missing';",
            "DELETE FROM metrics WHERE tag = 'missing';",
            "UPDATE metrics SET value = 5 WHERE value = '60';",
            "DELETE FROM metrics WHERE tag = 123;",
            "PRAGMA integrity_check;",
            ".exit",
        ])
        first = run_session(executable, db_file, setup)

        require(first, "db > 50\nExecuted.")
        require(first, "(1, hot, 999)")
        require(first, "db > 10\nExecuted.")
        require(first, "Height: 1")
        require(first, "db > 0\nExecuted.")
        require(first, "(60, cold, 60)")
        if first.count("Syntax error. Could not parse statement.") < 2:
            raise AssertionError("typed non-PK mutation errors were not rejected\n" + first)
        require(first, "ok")

        second = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM metrics;",
                "SELECT COUNT(*) FROM metrics WHERE tag = 'cold';",
                "SELECT * FROM metrics WHERE id = 51;",
                ".stats metrics",
                "PRAGMA integrity_check;",
                "DELETE FROM metrics WHERE tag = 'cold';",
                "SELECT COUNT(*) FROM metrics;",
                ".stats metrics",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if second.count("db > 10\nExecuted.") < 2:
            raise AssertionError("reopen did not preserve the ten cold rows\n" + second)
        require(second, "(51, cold, 51)")
        require(second, "db > 0\nExecuted.")
        if second.count("Height: 1") < 2:
            raise AssertionError("generic non-PK delete did not leave a collapsed root\n" + second)
        if second.count("ok") < 2:
            raise AssertionError("integrity_check failed around non-PK delete\n" + second)

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

        print(
            "PASS: generic non-PK UPDATE/DELETE survive split, batch mutation, rollback, merge, root collapse and reopen."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
