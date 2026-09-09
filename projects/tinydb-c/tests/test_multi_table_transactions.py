import glob
import os
import subprocess
import sys


def find_executable(repo_root):
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
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_multi_tx.db")
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                "BEGIN;",
                "INSERT INTO users VALUES (10, 'rollback-user', 'u10@test');",
                "INSERT INTO archive VALUES (10, 'rollback-archive', 'a10@test');",
                "ROLLBACK;",
                "SELECT * FROM users WHERE id = 10;",
                "SELECT * FROM archive WHERE id = 10;",
                "BEGIN;",
                "INSERT INTO users VALUES (11, 'commit-user', 'u11@test');",
                "INSERT INTO archive VALUES (11, 'commit-archive', 'a11@test');",
                "COMMIT;",
                "BEGIN;",
                "INSERT INTO users VALUES (12, 'before-savepoint', 'u12@test');",
                "SAVEPOINT both_roots;",
                "INSERT INTO archive VALUES (12, 'after-savepoint', 'a12@test');",
                "INSERT INTO users VALUES (13, 'after-savepoint-user', 'u13@test');",
                "ROLLBACK TO both_roots;",
                "COMMIT;",
                ".exit",
            ],
        )
        assert "Error:" not in first, first
        assert "rollback-user" not in first, first
        assert "rollback-archive" not in first, first

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT * FROM users WHERE id = 10;",
                "SELECT * FROM archive WHERE id = 10;",
                "SELECT * FROM users WHERE id = 11;",
                "SELECT * FROM archive WHERE id = 11;",
                "SELECT * FROM users WHERE id = 12;",
                "SELECT * FROM archive WHERE id = 12;",
                "SELECT * FROM users WHERE id = 13;",
                "SELECT * FROM users JOIN archive ON users.id = archive.id;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        assert "rollback-user" not in reopened, reopened
        assert "rollback-archive" not in reopened, reopened
        assert "(11, commit-user, u11@test)" in reopened, reopened
        assert "(11, commit-archive, a11@test)" in reopened, reopened
        assert "(12, before-savepoint, u12@test)" in reopened, reopened
        assert "after-savepoint" not in reopened, reopened
        assert "after-savepoint-user" not in reopened, reopened
        assert "(11, commit-user, u11@test) | (11, commit-archive, a11@test)" in reopened, reopened
        assert "ok" in reopened, reopened

        print("PASS: transactions and savepoints span independent table roots")
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
