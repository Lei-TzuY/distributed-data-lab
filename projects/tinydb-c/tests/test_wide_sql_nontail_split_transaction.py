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
        "payload-native non-tail split must preserve the existing child upper boundary",
        "payload-native non-root split requires a valid sibling key range",
        "payload-native non-root split found inconsistent parent/sibling topology",
        "payload-native V2 leaf split staging rejected parent overflow or inconsistent topology",
        "unable to initialize the schema-sized V2 table root",
    ):
        if marker in output:
            raise AssertionError(f"wide SQL non-tail split hit storage guard {marker!r}\n{output}")


def insert_sql(row_id, left, right):
    return f"INSERT INTO wide_nontail VALUES ({row_id}, '{left}', '{right}');"


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_sql_nontail_split_transaction.db")
    cleanup(db_path)

    left = "L" * 250
    right = "R" * 250

    try:
        commands = [
            "CREATE TABLE wide_nontail (id INT, left_text VARCHAR, right_text VARCHAR);",
        ]
        # Seven equal-size rows fill the original V2 root leaf. Row 80 forces
        # the first root split. The byte-balanced seven-row split is 3/4, so
        # the resulting left non-root leaf owns keys 10,20,30 and the existing
        # next sibling starts at key 40.
        for row_id in range(10, 81, 10):
            commands.append(insert_sql(row_id, left, right))
        commands.extend(
            [
                "SELECT COUNT(*) FROM wide_nontail;",
                ".stats",
                "PRAGMA integrity_check;",
                "BEGIN;",
                # Keys 11..14 fill the non-tail left leaf to seven rows while
                # preserving its old upper boundary (30). Key 15 must split
                # that leaf between its previous/next siblings, update the
                # parent separators, and repair the old-next leaf's prev link.
                insert_sql(11, left, right),
                insert_sql(12, left, right),
                insert_sql(13, left, right),
                insert_sql(14, left, right),
                insert_sql(15, left, right),
                "SELECT COUNT(*) FROM wide_nontail;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 11;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 15;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 20;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 30;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 40;",
                ".stats",
                "PRAGMA integrity_check;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM wide_nontail;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 11;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 15;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 20;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 30;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 40;",
                ".stats",
                "PRAGMA integrity_check;",
                "BEGIN;",
                insert_sql(11, left, right),
                insert_sql(12, left, right),
                insert_sql(13, left, right),
                insert_sql(14, left, right),
                insert_sql(15, left, right),
                "COMMIT;",
                "SELECT COUNT(*) FROM wide_nontail;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 11;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 15;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 20;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 30;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 40;",
                ".stats",
                "PRAGMA integrity_check;",
                ".exit",
            ]
        )
        first = run_session(executable, db_path, commands)
        reject_storage_failure(first)

        counts = scalar_results(first)
        expected = [
            8,
            13, 1, 1, 1, 1, 1,
            8, 0, 0, 1, 1, 1,
            13, 1, 1, 1, 1, 1,
        ]
        if counts != expected:
            raise AssertionError(
                f"unexpected non-tail split transaction counts {counts!r}, expected {expected!r}\n{first}"
            )

        pages = total_pages(first)
        if len(pages) != 4:
            raise AssertionError(f"expected four page-count snapshots, got {pages!r}\n{first}")
        baseline, inside_split, after_rollback, after_commit = pages
        if baseline < 4:
            raise AssertionError(
                f"eight long sparse-key rows did not establish the expected height-two tree: {pages!r}\n{first}"
            )
        if inside_split != baseline + 1:
            raise AssertionError(
                f"non-tail leaf split should allocate exactly one page: {pages!r}\n{first}"
            )
        if after_rollback != baseline:
            raise AssertionError(
                f"ROLLBACK did not restore non-tail split allocator/topology state: {pages!r}\n{first}"
            )
        if after_commit != baseline + 1:
            raise AssertionError(
                f"committed non-tail split page count was wrong: {pages!r}\n{first}"
            )
        if first.count("db > ok\nExecuted.") < 4:
            raise AssertionError(
                "integrity_check failed around non-tail split transaction; parent separator or sibling backlink may be inconsistent\n"
                + first
            )

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT COUNT(*) FROM wide_nontail;",
                # Point probes straddle the two split children and the untouched
                # old-next sibling, exercising the rewritten parent separators.
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 11;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 15;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 20;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 30;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 40;",
                ".stats",
                "PRAGMA integrity_check;",
                # Exercise both the newly split range and the untouched old
                # next sibling, then roll both changes back without altering
                # the committed three-leaf allocation.
                "BEGIN;",
                "DELETE FROM wide_nontail WHERE id = 15;",
                "UPDATE wide_nontail SET left_text = 'temporary' WHERE id = 40;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM wide_nontail;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 15;",
                "SELECT COUNT(*) FROM wide_nontail WHERE id = 40;",
                "SELECT COUNT(*) FROM wide_nontail WHERE left_text = 'temporary';",
                ".stats",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_storage_failure(reopened)

        reopened_counts = scalar_results(reopened)
        reopened_expected = [13, 1, 1, 1, 1, 1, 13, 1, 1, 0]
        if reopened_counts != reopened_expected:
            raise AssertionError(
                f"unexpected reopened non-tail split counts {reopened_counts!r}, "
                f"expected {reopened_expected!r}\n{reopened}"
            )
        reopened_pages = total_pages(reopened)
        if reopened_pages != [baseline + 1, baseline + 1]:
            raise AssertionError(
                f"non-tail split topology changed across reopen/rollback: "
                f"{reopened_pages!r}, baseline={baseline}\n{reopened}"
            )
        if reopened.count("db > ok\nExecuted.") < 2:
            raise AssertionError(
                "reopened non-tail split integrity_check failed; sibling backlink or parent routing did not persist\n"
                + reopened
            )

        print(
            "PASS: a 516-byte SQL table performs a non-tail V2 leaf split inside BEGIN; "
            "the split adds exactly one page, point routing across both rewritten child ranges "
            "and the old-next sibling remains correct, ROLLBACK restores rows/allocation/topology, "
            "and COMMIT plus reopen preserves sibling backlinks and integrity."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
