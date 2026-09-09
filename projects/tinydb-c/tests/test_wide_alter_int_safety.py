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
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError(output)
    return output


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_alter_int_safety.db")
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE wide_nonempty (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_nonempty VALUES (1, 'left-a', 'right-a');",
                "ALTER TABLE wide_nonempty ADD COLUMN score INT;",
                "PRAGMA table_info(wide_nonempty);",
                "SELECT * FROM wide_nonempty WHERE id = 1;",
                "UPDATE wide_nonempty SET score = 11 WHERE id = 1;",
                "SELECT * FROM wide_nonempty WHERE id = 1;",
                "INSERT INTO wide_nonempty VALUES (3, 'left-c', 'right-c', 13);",
                "SELECT * FROM wide_nonempty WHERE id = 3;",
                "CREATE TABLE wide_empty (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "ALTER TABLE wide_empty ADD COLUMN score INT;",
                "INSERT INTO wide_empty VALUES (2, 'left-b', 'right-b', 7);",
                "PRAGMA table_info(wide_empty);",
                "SELECT * FROM wide_empty WHERE id = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(first, "Column 'score' added to table 'wide_nonempty'.")
        require(first, "Column 'score' added to table 'wide_empty'.")
        require(first, "(1, left-a, right-a, 0)")
        require(first, "(1, left-a, right-a, 11)")
        require(first, "(3, left-c, right-c, 13)")
        require(first, "(2, left-b, right-b, 7)")
        require(first, "ok")
        if first.count("score | INT") != 2:
            raise AssertionError(
                "both wide tables must persist the appended INT schema\n" + first
            )

        second = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(wide_nonempty);",
                "SELECT * FROM wide_nonempty WHERE id = 1;",
                "SELECT * FROM wide_nonempty WHERE id = 3;",
                "INSERT INTO wide_nonempty VALUES (5, 'left-e', 'right-e', 15);",
                "SELECT * FROM wide_nonempty WHERE id = 5;",
                "PRAGMA table_info(wide_empty);",
                "SELECT * FROM wide_empty WHERE id = 2;",
                "INSERT INTO wide_empty VALUES (4, 'left-d', 'right-d', 9);",
                "SELECT * FROM wide_empty WHERE id = 4;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(second, "(1, left-a, right-a, 11)")
        require(second, "(3, left-c, right-c, 13)")
        require(second, "(5, left-e, right-e, 15)")
        require(second, "(2, left-b, right-b, 7)")
        require(second, "(4, left-d, right-d, 9)")
        require(second, "ok")
        if second.count("score | INT") != 2:
            raise AssertionError(
                "appended INT schemas did not persist across reopen\n" + second
            )

        print(
            "PASS: populated compact V2 rows accept append-only INT schema evolution, "
            "old rows materialize a zero trailing default, UPDATE upgrades them to the "
            "current schema generation, and reopen preserves both old and new rows."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
