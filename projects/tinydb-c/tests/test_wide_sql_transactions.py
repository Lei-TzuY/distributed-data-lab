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


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def scalar_results(output):
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def reject_legacy_wide_failures(output):
    for marker in (
        "fixed generic record slot",
        "fixed B+ tree value slot",
        "requires a V2 slotted leaf",
        "unable to initialize the schema-sized V2 table root",
    ):
        if marker in output:
            raise AssertionError(f"wide SQL transaction hit legacy storage guard {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_sql_transactions.db")
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                # Bare VARCHAR reserves 256 bytes, making this 516-byte schema
                # larger than the historical TinyDBRecord carrier. All DML must
                # therefore stay on the payload-native V2 path inside transactions.
                "CREATE TABLE wide_tx (id INT, left_text VARCHAR, right_text VARCHAR);",
                "INSERT INTO wide_tx VALUES (10, 'left-a', 'right-a');",
                "INSERT INTO wide_tx VALUES (20, 'left-b', 'right-b');",
                "BEGIN;",
                "INSERT INTO wide_tx VALUES (30, 'left-c', 'right-c');",
                "UPDATE wide_tx SET right_text = 'rollback-updated' WHERE id = 10;",
                "DELETE FROM wide_tx WHERE id = 20;",
                "SELECT * FROM wide_tx WHERE id = 10;",
                "SELECT COUNT(*) FROM wide_tx WHERE id = 20;",
                "SELECT COUNT(*) FROM wide_tx WHERE id = 30;",
                "ROLLBACK;",
                "SELECT * FROM wide_tx WHERE id = 10;",
                "SELECT * FROM wide_tx WHERE id = 20;",
                "SELECT COUNT(*) FROM wide_tx WHERE id = 30;",
                "PRAGMA integrity_check;",
                "BEGIN;",
                "INSERT INTO wide_tx VALUES (40, 'left-d', 'right-d');",
                "UPDATE wide_tx SET right_text = 'commit-updated' WHERE id = 10;",
                "DELETE FROM wide_tx WHERE id = 20;",
                "COMMIT;",
                "SELECT * FROM wide_tx WHERE id = 10;",
                "SELECT COUNT(*) FROM wide_tx WHERE id = 20;",
                "SELECT * FROM wide_tx WHERE id = 40;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        reject_legacy_wide_failures(first)
        require(first, "(10, left-a, rollback-updated)")
        require(first, "(10, left-a, right-a)")
        require(first, "(20, left-b, right-b)")
        require(first, "(10, left-a, commit-updated)")
        require(first, "(40, left-d, right-d)")
        if scalar_results(first) != [0, 1, 0, 0]:
            raise AssertionError("wide transaction visibility/rollback counts were wrong\n" + first)
        if first.count("db > ok\nExecuted.") < 2:
            raise AssertionError("integrity_check did not pass before and after wide COMMIT\n" + first)

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT * FROM wide_tx WHERE id = 10;",
                "SELECT COUNT(*) FROM wide_tx WHERE id = 20;",
                "SELECT COUNT(*) FROM wide_tx WHERE id = 30;",
                "SELECT * FROM wide_tx WHERE id = 40;",
                # Prove rollback remains correct after a durable reopen too.
                "BEGIN;",
                "UPDATE wide_tx SET left_text = 'second-rollback' WHERE id = 40;",
                "DELETE FROM wide_tx WHERE id = 10;",
                "ROLLBACK;",
                "SELECT * FROM wide_tx WHERE id = 10;",
                "SELECT * FROM wide_tx WHERE id = 40;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        reject_legacy_wide_failures(reopened)
        require(reopened, "(10, left-a, commit-updated)")
        require(reopened, "(40, left-d, right-d)")
        if "second-rollback" in reopened:
            raise AssertionError("rolled-back wide UPDATE leaked after reopen\n" + reopened)
        if scalar_results(reopened) != [0, 0]:
            raise AssertionError("rolled-back/deleted wide rows had wrong durable counts\n" + reopened)
        require(reopened, "ok")

        print(
            "PASS: 516-byte SQL rows participate in explicit Pager transactions: "
            "INSERT/UPDATE/DELETE are visible inside BEGIN, ROLLBACK restores all three "
            "mutation classes, COMMIT survives reopen, a second post-reopen rollback is "
            "isolated, and integrity_check remains clean."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
