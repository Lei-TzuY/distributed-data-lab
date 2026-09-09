import glob
import os
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


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tinydb = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_generic_varchar_width.db")
    cleanup(db_path)

    try:
        commands = [
            "CREATE TABLE contacts (id INT, first VARCHAR(40), last VARCHAR(40), note VARCHAR(120));",
        ]
        for row_id in range(1, 31):
            first = f"first{row_id:02d}"
            last = f"last{row_id:02d}"
            note = "n" * min(120, 20 + row_id)
            commands.append(
                f"INSERT INTO contacts VALUES ({row_id}, '{first}', '{last}', '{note}');"
            )
        commands.extend(
            [
                "PRAGMA table_info(contacts);",
                "SELECT * FROM contacts WHERE id = 1;",
                "SELECT COUNT(*) FROM contacts;",
                "INSERT INTO contacts VALUES (99, 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx', 'last99', 'too long');",
                "SELECT COUNT(*) FROM contacts;",
                # 4 + (145 + 1) + (145 + 1) = 296 bytes: larger than
                # TinyDBRecord, but valid for schema-sized payload/V2 storage.
                "CREATE TABLE oversized (id INT, a VARCHAR(145), b VARCHAR(145));",
                "PRAGMA table_info(oversized);",
                "INSERT INTO oversized VALUES (501, 'wide-a', 'wide-b');",
                "SELECT * FROM oversized WHERE id = 501;",
                "CREATE TABLE invalid_zero (id INT, a VARCHAR(0));",
                "CREATE TABLE invalid_large (id INT, a VARCHAR(256));",
                # Explicit widths that do not match the historical Row ABI are
                # ordinary schema-aware generic layouts even when column names
                # happen to be id/username/email.
                "CREATE TABLE archive_compact (id INT, username VARCHAR(20), email VARCHAR(40));",
                "INSERT INTO archive_compact VALUES (6, 'compact', 'compact@example.com');",
                "PRAGMA table_info(archive_compact);",
                "SELECT * FROM archive_compact WHERE id = 6;",
                # Exact serialized widths still opt into the legacy Row ABI.
                "CREATE TABLE archive_exact (id INT, username VARCHAR(32), email VARCHAR(255));",
                "INSERT INTO archive_exact VALUES (7, 'archived', 'archive@example.com');",
                "SELECT * FROM archive_exact WHERE id = 7;",
                "PRAGMA integrity_check;",
                ".exit",
            ]
        )
        first = run_session(tinydb, db_path, commands)

        require(first, "first | VARCHAR(40)")
        require(first, "last | VARCHAR(40)")
        require(first, "note | VARCHAR(120)")
        require(first, "(1, first01, last01,")
        if first.count("db > 30\nExecuted.") < 2:
            raise AssertionError("oversized value changed row count\n" + first)
        require(first, "a | VARCHAR(145)")
        require(first, "b | VARCHAR(145)")
        require(first, "(501, wide-a, wide-b)")
        if "fixed generic record slot" in first or "schema-sized payload limit" in first:
            raise AssertionError("valid 296-byte V2 schema was rejected\n" + first)
        if first.count("Syntax error. Could not parse statement.") < 2:
            raise AssertionError("VARCHAR(0)/VARCHAR(256) were not rejected\n" + first)
        require(first, "username | VARCHAR(20)")
        require(first, "email | VARCHAR(40)")
        require(first, "(6, compact, compact@example.com)")
        require(first, "(7, archived, archive@example.com)")
        require(first, "ok")

        second = run_session(
            tinydb,
            db_path,
            [
                "PRAGMA table_info(contacts);",
                "SELECT COUNT(*) FROM contacts;",
                "SELECT * FROM contacts WHERE id = 30;",
                "PRAGMA table_info(oversized);",
                "SELECT * FROM oversized WHERE id = 501;",
                "INSERT INTO oversized VALUES (502, 'wide-c', 'wide-d');",
                "SELECT * FROM oversized WHERE id = 502;",
                "PRAGMA table_info(archive_compact);",
                "SELECT * FROM archive_compact WHERE id = 6;",
                "INSERT INTO archive_compact VALUES (8, 'compact2', 'second@example.com');",
                "SELECT * FROM archive_compact WHERE id = 8;",
                "SELECT * FROM archive_exact WHERE id = 7;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(second, "first | VARCHAR(40)")
        require(second, "last | VARCHAR(40)")
        require(second, "note | VARCHAR(120)")
        require(second, "db > 30\nExecuted.")
        require(second, "(30, first30, last30,")
        require(second, "a | VARCHAR(145)")
        require(second, "b | VARCHAR(145)")
        require(second, "(501, wide-a, wide-b)")
        require(second, "(502, wide-c, wide-d)")
        require(second, "username | VARCHAR(20)")
        require(second, "email | VARCHAR(40)")
        require(second, "(6, compact, compact@example.com)")
        require(second, "(8, compact2, second@example.com)")
        require(second, "(7, archived, archive@example.com)")
        require(second, "ok")

        print(
            "PASS: VARCHAR(n) capacities drive schema-aware layouts, reject oversized "
            "values atomically, admit rows beyond TinyDBRecord through V2 payload "
            "storage, preserve compact id/username/email layouts across reopen, and "
            "reserve legacy Row routing for compatible serialized widths."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
