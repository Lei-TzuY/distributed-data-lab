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
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def reject_unexpected_storage_error(output):
    for marker in (
        "fixed generic record slot",
        "fixed B+ tree value slot",
        "schema-sized payload limit",
        "requires a V2 slotted leaf",
        "unable to initialize the schema-sized V2 table root",
    ):
        if marker in output:
            raise AssertionError(f"unexpected wide-table storage failure {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_create_table_policy.db")
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                # Bare VARCHAR reserves the historical 256-byte generic width,
                # so this executable schema is 516 bytes and must use payload/V2
                # storage instead of the 293-byte TinyDBRecord carrier.
                "CREATE TABLE wide_docs (id INT, left_text VARCHAR, right_text VARCHAR);",
                "INSERT INTO wide_docs VALUES (101, 'left-a', 'right-a');",
                "INSERT INTO wide_docs VALUES (202, 'left-b', 'right-b');",
                # Exercise both the payload range-seek primary-key path and a
                # decoded payload table scan with projection/filtering.
                "SELECT * FROM wide_docs WHERE id = 202;",
                "SELECT left_text FROM wide_docs WHERE right_text = 'right-a';",
                # Mutation must stay on schema-sized payloads too. Cover a point
                # UPDATE, a scan-selected UPDATE, and point DELETE before reopen.
                "UPDATE wide_docs SET right_text = 'right-b-updated' WHERE id = 202;",
                "UPDATE wide_docs SET left_text = 'left-a-updated' WHERE right_text = 'right-a';",
                "SELECT right_text FROM wide_docs WHERE id = 202;",
                "SELECT left_text FROM wide_docs WHERE id = 101;",
                "DELETE FROM wide_docs WHERE id = 202;",
                "SELECT COUNT(*) FROM wide_docs WHERE id = 202;",
                "CREATE TABLE metadata_only (key INT, name VARCHAR);",
                ".tables",
                "CREATE TABLE metrics (id INT, name VARCHAR, price INT, stock INT);",
                "INSERT INTO metrics VALUES (1, 'alpha', 10, 3);",
                "INSERT INTO metrics VALUES (2, 'beta', 25, 0);",
                "INSERT INTO metrics VALUES (3, 'gamma', 40, 7);",
                "SELECT COUNT(*) FROM metrics WHERE price >= 20 AND stock >= 0;",
                "SELECT name FROM metrics WHERE id = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        reject_unexpected_storage_error(first)
        require(first, "wide_docs")
        require(first, "metadata_only")
        require(first, "(202, left-b, right-b)")
        require(first, "left-a")
        require(first, "right-b-updated")
        require(first, "left-a-updated")
        require(first, "beta")
        require(first, "ok")
        if scalar_results(first) != [0, 2]:
            raise AssertionError("wide mutation or mixed-layout query returned wrong count\n" + first)

        reopened = run_session(
            executable,
            db_file,
            [
                ".tables",
                "PRAGMA table_info(wide_docs);",
                "SELECT * FROM wide_docs WHERE id = 101;",
                "SELECT COUNT(*) FROM wide_docs WHERE id = 202;",
                # A post-reopen INSERT proves both the wide catalog layout and
                # V2 root format survived durable mutation/reopen.
                "INSERT INTO wide_docs VALUES (303, 'left-c', 'right-c');",
                "SELECT right_text FROM wide_docs WHERE left_text = 'left-c';",
                "PRAGMA table_info(metrics);",
                "SELECT COUNT(*) FROM metrics WHERE stock >= 0;",
                "SELECT COUNT(*) FROM metrics WHERE name LIKE '%a';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_unexpected_storage_error(reopened)
        require(reopened, "wide_docs")
        require(reopened, "metrics")
        require(reopened, "metadata_only")
        require(reopened, "left_text")
        require(reopened, "right_text")
        require(reopened, "(101, left-a-updated, right-a)")
        require(reopened, "right-c")
        require(reopened, "name")
        require(reopened, "price")
        require(reopened, "stock")
        if scalar_results(reopened) != [0, 3, 3]:
            raise AssertionError("wide mutation or mixed-layout rows did not survive reopen\n" + reopened)
        require(reopened, "ok")

        print(
            "PASS: CREATE TABLE admits schema-sized executable generic rows beyond the "
            "legacy TinyDBRecord carrier, initializes their root as V2, persists payload-native "
            "SQL INSERT/SELECT/UPDATE/DELETE before and after reopen, preserves metadata-only "
            "DDL, and keeps narrow mixed-layout queries durable."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
