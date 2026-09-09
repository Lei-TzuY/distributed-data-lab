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


def total_pages(output):
    return [int(value) for value in re.findall(r"Total Pages:\s*(\d+)", output)]


def reject_storage_failure(output):
    for marker in (
        "fixed generic record slot",
        "fixed B+ tree value slot",
        "requires a V2 slotted leaf",
        "slotted V2 insert requires existing free space and cannot split the leaf",
        "payload-native INSERT requires a V2 slotted leaf with enough free space",
        "payload-native INSERT reached a full internal parent",
        "unable to initialize the schema-sized V2 table root",
    ):
        if marker in output:
            raise AssertionError(f"wide SQL non-root split hit storage guard {marker!r}\n{output}")


def insert_sql(row_id, left, right):
    return f"INSERT INTO wide_nonroot VALUES ({row_id}, '{left}', '{right}');"


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_sql_nonroot_split_transaction.db")
    cleanup(db_path)

    left = "L" * 250
    right = "R" * 250

    try:
        commands = [
            "CREATE TABLE wide_nonroot (id INT, left_text VARCHAR, right_text VARCHAR);",
        ]
        # Seven rows fill the original V2 root leaf; row 8 grows the table to
        # height two. The resulting rightmost child contains five rows.
        for row_id in range(1, 9):
            commands.append(insert_sql(row_id, left, right))
        commands.extend(
            [
                "SELECT COUNT(*) FROM wide_nonroot;",
                ".stats",
                "PRAGMA integrity_check;",
                "BEGIN;",
                # Rows 9 and 10 fill the rightmost non-root leaf. Row 11 must
                # split that tail leaf under the existing non-full root.
                insert_sql(9, left, right),
                insert_sql(10, left, right),
                insert_sql(11, left, right),
                "SELECT COUNT(*) FROM wide_nonroot;",
                "SELECT COUNT(*) FROM wide_nonroot WHERE id = 11;",
                ".stats",
                "PRAGMA integrity_check;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM wide_nonroot;",
                "SELECT COUNT(*) FROM wide_nonroot WHERE id = 9;",
                "SELECT COUNT(*) FROM wide_nonroot WHERE id = 11;",
                ".stats",
                "PRAGMA integrity_check;",
                "BEGIN;",
                insert_sql(9, left, right),
                insert_sql(10, left, right),
                insert_sql(11, left, right),
                "COMMIT;",
                "SELECT COUNT(*) FROM wide_nonroot;",
                "SELECT COUNT(*) FROM wide_nonroot WHERE id = 11;",
                ".stats",
                "PRAGMA integrity_check;",
                ".exit",
            ]
        )
        first = run_session(executable, db_path, commands)
        reject_storage_failure(first)

        counts = scalar_results(first)
        if counts != [8, 11, 1, 8, 0, 0, 11, 1]:
            raise AssertionError(
                f"unexpected non-root split transaction counts {counts!r}\n{first}"
            )

        pages = total_pages(first)
        if len(pages) != 4:
            raise AssertionError(f"expected four page-count snapshots, got {pages!r}\n{first}")
        baseline, inside_split, after_rollback, after_commit = pages
        if baseline < 4:
            raise AssertionError(
                f"eight long rows did not establish the expected height-two tree: {pages!r}\n{first}"
            )
        if inside_split != baseline + 1:
            raise AssertionError(
                f"non-root tail split should allocate exactly one page: {pages!r}\n{first}"
            )
        if after_rollback != baseline:
            raise AssertionError(
                f"ROLLBACK did not restore non-root split allocator state: {pages!r}\n{first}"
            )
        if after_commit != baseline + 1:
            raise AssertionError(
                f"committed non-root split page count was wrong: {pages!r}\n{first}"
            )
        if first.count("db > ok\nExecuted.") < 4:
            raise AssertionError("integrity_check failed around non-root split transaction\n" + first)

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT COUNT(*) FROM wide_nonroot;",
                "SELECT COUNT(*) FROM wide_nonroot WHERE id = 9;",
                "SELECT COUNT(*) FROM wide_nonroot WHERE id = 11;",
                ".stats",
                "PRAGMA integrity_check;",
                # Verify a transaction against the three-leaf topology rolls
                # back ordinary mutations without changing its allocation.
                "BEGIN;",
                "DELETE FROM wide_nonroot WHERE id = 11;",
                "UPDATE wide_nonroot SET left_text = 'temporary' WHERE id = 9;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM wide_nonroot;",
                "SELECT COUNT(*) FROM wide_nonroot WHERE id = 11;",
                ".stats",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_storage_failure(reopened)

        reopened_counts = scalar_results(reopened)
        if reopened_counts != [11, 1, 1, 11, 1]:
            raise AssertionError(
                f"unexpected reopened non-root split counts {reopened_counts!r}\n{reopened}"
            )
        reopened_pages = total_pages(reopened)
        if reopened_pages != [baseline + 1, baseline + 1]:
            raise AssertionError(
                f"non-root split topology changed across reopen/rollback: "
                f"{reopened_pages!r}, baseline={baseline}\n{reopened}"
            )
        if "temporary" in reopened:
            raise AssertionError("rolled-back post-reopen UPDATE leaked into output\n" + reopened)
        if reopened.count("db > ok\nExecuted.") < 2:
            raise AssertionError("reopened non-root split integrity_check failed\n" + reopened)

        print(
            "PASS: a 516-byte SQL table grows from a committed root split into a "
            "three-leaf tree via a non-root tail split inside BEGIN; the split adds "
            "exactly one page, ROLLBACK restores rows and allocator state, COMMIT "
            "survives reopen, and later rollback preserves topology/integrity."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
