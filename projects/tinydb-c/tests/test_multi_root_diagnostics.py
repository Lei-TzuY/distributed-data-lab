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
    db_path = os.path.join(os.path.dirname(__file__), "test_multi_root_diag.db")
    cleanup(db_path)

    try:
        commands = [
            "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
            "INSERT INTO users VALUES (100, 'main', 'main@example.com');",
        ]
        for row_id in range(1, 21):
            commands.append(
                f"INSERT INTO archive VALUES ({row_id}, 'archive-{row_id}', 'a{row_id}@example.com');"
            )
        commands.extend([
            ".stats",
            ".stats archive",
            ".schema archive",
            ".btree archive",
            ".check all",
            "PRAGMA integrity_check;",
            ".exit",
        ])
        first = run_session(executable, db_path, commands)

        assert "Total Rows: 21" in first, first
        assert "Table: users" in first, first
        assert "Table: archive" in first, first
        assert "Rows: 20" in first, first
        assert "Table: archive (root page " in first, first
        assert "B+ tree for archive (root page " in first, first
        assert "archive: ok: root=" in first, first
        assert "users: ok: root=0" in first, first
        assert "\nok\n" in first or "db > ok\n" in first, first

        second = run_session(
            executable,
            db_path,
            [
                ".stats archive",
                ".check archive",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        assert "Rows: 20" in second, second
        assert "archive: ok: root=" in second, second
        assert "ok" in second, second

        print("PASS: multi-root schema/stats/btree/check diagnostics verified")
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
