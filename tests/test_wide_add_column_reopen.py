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
    raise AssertionError("Could not find tinydb executable")


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
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout + result.stderr


def require(output, marker):
    assert marker in output, f"missing marker {marker!r}\n{output}"


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_file = os.path.join(os.path.dirname(__file__), "test_wide_add_column_reopen.db")
    cleanup(db_file)

    try:
        commands = [
            "CREATE TABLE archive (id INT, title VARCHAR(255), body VARCHAR(255));",
        ]
        for row_id in range(1, 81):
            commands.append(
                "INSERT INTO archive VALUES "
                f"({row_id}, 'title-{row_id}', 'body-{row_id}');"
            )
        commands.extend(
            [
                "ALTER TABLE archive ADD COLUMN generation INT;",
                ".schema archive",
                "SELECT generation FROM archive WHERE id = 1;",
                "SELECT generation FROM archive WHERE id = 80;",
                "UPDATE archive SET generation = 7 WHERE id = 1;",
                "INSERT INTO archive VALUES (81, 'title-81', 'body-81', 19);",
                "SELECT * FROM archive WHERE id = 1;",
                "SELECT * FROM archive WHERE id = 81;",
                "PRAGMA integrity_check;",
                ".exit",
            ]
        )
        first = run_session(executable, db_file, commands)
        require(first, "Column 'generation' added to table 'archive'.")
        require(first, "generation       INT          offset=516 size=4")
        require(first, "(1, title-1, body-1, 7)")
        require(first, "(81, title-81, body-81, 19)")
        require(first, "ok")

        second = run_session(
            executable,
            db_file,
            [
                ".schema archive",
                "SELECT * FROM archive WHERE id = 1;",
                "SELECT * FROM archive WHERE id = 2;",
                "SELECT * FROM archive WHERE id = 80;",
                "SELECT * FROM archive WHERE id = 81;",
                "SELECT COUNT(*) FROM archive;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(second, "generation       INT          offset=516 size=4")
        require(second, "(1, title-1, body-1, 7)")
        require(second, "(2, title-2, body-2, 0)")
        require(second, "(80, title-80, body-80, 0)")
        require(second, "(81, title-81, body-81, 19)")
        require(second, "db > 81\nExecuted.")
        require(second, "ok")

        print(
            "PASS: populated 516-byte multi-leaf V2 table supports append-only ADD COLUMN, "
            "historical-row defaults, new-row writes, and reopen persistence"
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
