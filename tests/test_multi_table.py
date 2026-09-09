import glob
import os
import subprocess
import sys


def find_executable(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find the tinydb executable")


def run_session(executable, db_file, commands):
    result = subprocess.run(
        [executable, db_file],
        input="".join(command + "\n" for command in commands),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=90,
    )
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    db_file = os.path.join(os.path.dirname(__file__), "test_multi_tbl.db")
    cleanup(db_file)

    try:
        archive_inserts = [
            "INSERT INTO archive VALUES ({0}, 'archive-{0}', 'archive{0}@example.com');".format(i)
            for i in range(1, 21)
        ]
        first_commands = [
            "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
            "CREATE TABLE posts (id INT, title VARCHAR, user_id INT);",
            "INSERT INTO users VALUES (1, 'main-user', 'main@example.com');",
        ]
        first_commands.extend(archive_inserts)
        first_commands.extend(
            [
                "UPDATE archive SET username = 'archive-twenty-updated' WHERE id = 20;",
                "DELETE FROM archive WHERE id = 1;",
                ".tables",
                ".exit",
            ]
        )

        first = run_session(executable, db_file, first_commands)

        assert "users" in first, first
        assert "archive" in first, first
        assert "posts" in first, first
        assert "Duplicate key" not in first, first
        assert "table or view not found" not in first, first

        second = run_session(
            executable,
            db_file,
            [
                ".tables",
                "SELECT * FROM users WHERE id = 1;",
                "SELECT COUNT(*) FROM archive;",
                "SELECT * FROM archive WHERE id = 20;",
                "DELETE FROM archive;",
                ".exit",
            ],
        )

        assert "archive" in second and "posts" in second, second
        assert "(1, main-user, main@example.com)" in second, second
        assert "19" in second, second
        assert "(20, archive-twenty-updated, archive20@example.com)" in second, second

        third = run_session(
            executable,
            db_file,
            [
                "SELECT * FROM users WHERE id = 1;",
                "SELECT COUNT(*) FROM archive;",
                ".tables",
                ".exit",
            ],
        )

        assert "(1, main-user, main@example.com)" in third, third
        assert "archive-twenty-updated" not in third, third
        assert "archive" in third and "posts" in third, third
        assert "0" in third, third

        # `posts` is now a real schema-aware generic-record table. The third
        # value intentionally has the wrong type (VARCHAR for user_id INT),
        # so the generic path must reject it without leaking bytes into users.
        typed_generic_guard = run_session(
            executable,
            db_file,
            [
                "INSERT INTO posts VALUES (9, 'not-a-row-layout', 'ignored');",
                "SELECT * FROM posts WHERE id = 9;",
                "SELECT * FROM users WHERE id = 9;",
                ".exit",
            ],
        )
        assert "Syntax error. Could not parse statement." in typed_generic_guard, typed_generic_guard
        assert "(9," not in typed_generic_guard, typed_generic_guard

        safety = run_session(
            executable,
            db_file,
            [
                "VACUUM;",
                "ALTER TABLE archive ADD COLUMN extra VARCHAR;",
                "SELECT * FROM archive UNION SELECT * FROM users;",
                "SELECT * FROM missing_table;",
                "INSERT INTO archive VALUES (99, 'still-safe', 'safe@example.com');",
                "SELECT * FROM archive WHERE id = 99;",
                ".exit",
            ],
        )
        assert "VACUUM/VACUUM INTO is disabled for multi-table databases" in safety, safety
        assert "ALTER TABLE ADD COLUMN is disabled" in safety, safety
        assert "query requires a cross-table/index path that is not routed safely yet" in safety, safety
        assert "table or view not found" in safety, safety
        assert "(99, still-safe, safe@example.com)" in safety, safety

        final_reopen = run_session(
            executable,
            db_file,
            [
                "SELECT * FROM users WHERE id = 1;",
                "SELECT * FROM archive WHERE id = 99;",
                ".tables",
                ".exit",
            ],
        )
        assert "(1, main-user, main@example.com)" in final_reopen, final_reopen
        assert "(99, still-safe, safe@example.com)" in final_reopen, final_reopen
        assert "archive" in final_reopen and "posts" in final_reopen, final_reopen

        print("PASS: persistent independent multi-table B+ tree roots verified")
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
