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


def total_pages(output):
    return [int(value) for value in re.findall(r"Total Pages:\s*(\d+)", output)]


def reject_storage_failure(output):
    for marker in (
        "fixed generic record slot",
        "fixed B+ tree value slot",
        "requires a V2 slotted leaf",
        "slotted V2 insert requires existing free space and cannot split the leaf",
        "payload-native INSERT requires a V2 slotted leaf with enough free space",
        "unable to initialize the schema-sized V2 table root",
    ):
        if marker in output:
            raise AssertionError(f"wide SQL split transaction hit storage guard {marker!r}\n{output}")


def long_value(prefix):
    # Bare VARCHAR reserves 256 bytes including the terminator. 250-character
    # values make each compact V2 row large enough that seven rows fit in the
    # root leaf but the eighth deterministically requires a root split.
    return prefix + (prefix[-1] * (250 - len(prefix)))


def insert_sql(row_id, left, right):
    return f"INSERT INTO wide_split VALUES ({row_id}, '{left}', '{right}');"


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_sql_split_transaction.db")
    cleanup(db_path)

    left = long_value("L")
    right = long_value("R")

    try:
        commands = [
            "CREATE TABLE wide_split (id INT, left_text VARCHAR, right_text VARCHAR);",
        ]
        for row_id in range(1, 8):
            commands.append(insert_sql(row_id, left, right))
        commands.extend(
            [
                ".stats",
                "SELECT COUNT(*) FROM wide_split;",
                "BEGIN;",
                insert_sql(8, left, right),
                "SELECT COUNT(*) FROM wide_split;",
                ".stats",
                "PRAGMA integrity_check;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM wide_split;",
                "SELECT COUNT(*) FROM wide_split WHERE id = 8;",
                ".stats",
                "PRAGMA integrity_check;",
                "BEGIN;",
                insert_sql(8, left, right),
                "COMMIT;",
                "SELECT COUNT(*) FROM wide_split;",
                "SELECT COUNT(*) FROM wide_split WHERE id = 8;",
                ".stats",
                "PRAGMA integrity_check;",
                ".exit",
            ]
        )
        first = run_session(executable, db_path, commands)
        reject_storage_failure(first)

        counts = scalar_results(first)
        if counts != [7, 8, 7, 0, 8, 1]:
            raise AssertionError(
                f"unexpected split transaction row counts {counts!r}\n{first}"
            )

        pages = total_pages(first)
        if len(pages) != 4:
            raise AssertionError(f"expected four page-count snapshots, got {pages!r}\n{first}")
        baseline, inside_split, after_rollback, after_commit = pages
        if inside_split != baseline + 2:
            raise AssertionError(
                f"root split should allocate exactly two child pages: {pages!r}\n{first}"
            )
        if after_rollback != baseline:
            raise AssertionError(
                f"ROLLBACK did not restore allocator page count: {pages!r}\n{first}"
            )
        if after_commit != baseline + 2:
            raise AssertionError(
                f"committed root split page count was not durable in-session: {pages!r}\n{first}"
            )
        if first.count("db > ok\nExecuted.") < 3:
            raise AssertionError("integrity_check did not pass around split rollback/commit\n" + first)

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT COUNT(*) FROM wide_split;",
                "SELECT COUNT(*) FROM wide_split WHERE id = 8;",
                ".stats",
                "PRAGMA integrity_check;",
                # Exercise another transaction against the now-height-two tree;
                # this must not corrupt the committed split topology.
                "BEGIN;",
                "DELETE FROM wide_split WHERE id = 8;",
                "SELECT COUNT(*) FROM wide_split;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM wide_split;",
                "SELECT COUNT(*) FROM wide_split WHERE id = 8;",
                ".stats",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_storage_failure(reopened)

        reopened_counts = scalar_results(reopened)
        if reopened_counts != [8, 1, 7, 8, 1]:
            raise AssertionError(
                f"unexpected reopened split transaction counts {reopened_counts!r}\n{reopened}"
            )
        reopened_pages = total_pages(reopened)
        if reopened_pages != [baseline + 2, baseline + 2]:
            raise AssertionError(
                f"committed split topology/page count changed across reopen or rollback: "
                f"{reopened_pages!r}, baseline={baseline}\n{reopened}"
            )
        require(reopened, "ok")

        print(
            "PASS: a 516-byte SQL table forces a V2 root-leaf split inside BEGIN; "
            "the split allocates exactly two child pages, ROLLBACK restores rows and "
            "allocator topology, COMMIT survives reopen, and a post-reopen DELETE "
            "rollback preserves the height-two tree with clean integrity checks."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
