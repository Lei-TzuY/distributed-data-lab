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
        timeout=60,
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
    base_dir = os.path.join(os.path.dirname(__file__), "..")
    executable = find_executable(base_dir)
    db_file = os.path.join(os.path.dirname(__file__), "test_join.db")
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE profiles (id INT, username VARCHAR, email VARCHAR);",
                "INSERT INTO users VALUES (1, 'alice', 'alice@test.com');",
                "INSERT INTO users VALUES (2, 'bob', 'bob@test.com');",
                "INSERT INTO profiles VALUES (1, 'alice-profile', 'profile1@test.com');",
                "INSERT INTO profiles VALUES (3, 'carol-profile', 'profile3@test.com');",
                "SELECT * FROM users JOIN profiles ON users.id = profiles.id;",
                ".exit",
            ],
        )

        expected = (
            "(1, alice, alice@test.com) | "
            "(1, alice-profile, profile1@test.com)"
        )
        assert expected in first, first
        assert "bob@test.com) |" not in first, first
        assert "carol-profile" not in first, first

        second = run_session(
            executable,
            db_file,
            [
                "SELECT * FROM profiles JOIN users ON profiles.id = users.id LIMIT 1;",
                ".exit",
            ],
        )
        reverse_expected = (
            "(1, alice-profile, profile1@test.com) | "
            "(1, alice, alice@test.com)"
        )
        assert reverse_expected in second, second

        unsupported = run_session(
            executable,
            db_file,
            [
                "SELECT * FROM users JOIN profiles ON users.username = profiles.username;",
                "SELECT * FROM users JOIN missing_table ON users.id = missing_table.id;",
                ".exit",
            ],
        )
        assert "query requires a cross-table/index path that is not routed safely yet" in unsupported, unsupported
        assert "table or view not found" in unsupported, unsupported

        print("PASS: real cross-root primary-key INNER JOIN verified")
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
