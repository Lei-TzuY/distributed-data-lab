import glob
import os
import re
import subprocess
import sys


def find_tinydb(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find tinydb executable")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_path, commands):
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def scalar_results(output):
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def reject_fallback(output):
    for marker in (
        "fixed generic record slot",
        "fixed B+ tree value slot",
        "TinyDBRecord",
        "schema row does not fit",
        "Syntax error. Could not parse statement.",
    ):
        if marker in output:
            raise AssertionError(f"wide OR mutation hit unexpected guard {marker!r}\n{output}")


def require_integrity(output, minimum=1):
    if output.count("db > ok\nExecuted.") < minimum:
        raise AssertionError("integrity_check failed\n" + output)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_sql_or_mutations.db")
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE wide_or_mut (id INT, left_text VARCHAR, right_text VARCHAR);",
                "INSERT INTO wide_or_mut VALUES (10, 'alpha', 'orig');",
                "INSERT INTO wide_or_mut VALUES (20, 'beta', 'orig');",
                "INSERT INTO wide_or_mut VALUES (30, 'gamma', 'orig');",
                "INSERT INTO wide_or_mut VALUES (40, 'delta', 'orig');",
                "INSERT INTO wide_or_mut VALUES (50, 'epsilon', 'orig');",
                "INSERT INTO wide_or_mut VALUES (60, 'zeta', 'orig');",
                "UPDATE wide_or_mut SET right_text = 'orhit' WHERE id < 20 OR id >= 50;",
                "SELECT COUNT(*) FROM wide_or_mut WHERE right_text = 'orhit';",
                "UPDATE wide_or_mut SET left_text = 'grouped' WHERE id >= 20 AND id <= 30 OR right_text = 'orhit';",
                "SELECT COUNT(*) FROM wide_or_mut WHERE left_text = 'grouped';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_fallback(first)
        if scalar_results(first) != [3, 5]:
            raise AssertionError("autocommit wide OR mutation counts were wrong\n" + first)
        require_integrity(first)

        second = run_session(
            executable,
            db_path,
            [
                "BEGIN;",
                "UPDATE wide_or_mut SET right_text = 'rolled' WHERE id = 20 OR id = 40;",
                "DELETE FROM wide_or_mut WHERE id = 30 OR id = 50;",
                "SELECT COUNT(*) FROM wide_or_mut;",
                "SELECT COUNT(*) FROM wide_or_mut WHERE right_text = 'rolled';",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM wide_or_mut;",
                "SELECT COUNT(*) FROM wide_or_mut WHERE right_text = 'rolled';",
                "SELECT COUNT(*) FROM wide_or_mut WHERE left_text = 'grouped';",
                "PRAGMA integrity_check;",
                "BEGIN;",
                "UPDATE wide_or_mut SET left_text = 'committed' WHERE id = 20 OR id >= 50;",
                "DELETE FROM wide_or_mut WHERE id < 20 OR id = 40;",
                "COMMIT;",
                "SELECT COUNT(*) FROM wide_or_mut;",
                "SELECT COUNT(*) FROM wide_or_mut WHERE left_text = 'committed';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_fallback(second)
        if scalar_results(second) != [4, 2, 6, 0, 5, 4, 3]:
            raise AssertionError("wide OR rollback/commit counts were wrong\n" + second)
        require_integrity(second, 2)

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT COUNT(*) FROM wide_or_mut;",
                "SELECT COUNT(*) FROM wide_or_mut WHERE left_text = 'committed';",
                "SELECT COUNT(*) FROM wide_or_mut WHERE left_text = 'grouped';",
                "SELECT COUNT(*) FROM wide_or_mut WHERE right_text = 'orhit';",
                "SELECT COUNT(*) FROM wide_or_mut WHERE id = 20 OR id >= 50;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_fallback(reopened)
        if scalar_results(reopened) != [4, 3, 1, 2, 3]:
            raise AssertionError("reopened wide OR mutation state was wrong\n" + reopened)
        require_integrity(reopened)

        print(
            "PASS: 516-byte SQL rows support payload-native OR-of-AND UPDATE/DELETE "
            "with autocommit, explicit rollback/commit, reopen durability, and "
            "integrity_check without narrowing through TinyDBRecord."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
