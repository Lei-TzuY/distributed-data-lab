import glob
import os
import re
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
        timeout=90,
    )
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_join_explain.db")
    cleanup(db_path)

    try:
        setup = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE profiles (id INT, username VARCHAR, email VARCHAR);",
                "INSERT INTO users VALUES (1, 'alice', 'alice@users.test');",
                "INSERT INTO users VALUES (2, 'bob', 'bob@users.test');",
                "INSERT INTO profiles VALUES (2, 'bob-profile', 'bob@profiles.test');",
                ".exit",
            ],
        )
        assert "Error:" not in setup, setup

        explained = run_session(
            executable,
            db_path,
            [
                "EXPLAIN SELECT * FROM users JOIN profiles ON users.id = profiles.id;",
                ".exit",
            ],
        )
        assert "PLAN: CROSS-ROOT PRIMARY KEY NESTED LOOP JOIN" in explained, explained
        assert "LEFT: FULL TABLE SCAN users (root page 0)" in explained, explained
        assert "RIGHT: PRIMARY KEY LOOKUP profiles.id (root page " in explained, explained
        assert "(2, bob, bob@users.test) |" not in explained, explained

        analyzed = run_session(
            executable,
            db_path,
            [
                "EXPLAIN ANALYZE SELECT * FROM users JOIN profiles ON users.id = profiles.id;",
                ".exit",
            ],
        )
        assert "QUERY PLAN" in analyzed, analyzed
        assert "PLAN: CROSS-ROOT PRIMARY KEY NESTED LOOP JOIN" in analyzed, analyzed
        assert "ACTUAL RESULT" in analyzed, analyzed
        assert "(2, bob, bob@users.test) | (2, bob-profile, bob@profiles.test)" in analyzed, analyzed
        match = re.search(
            r"ANALYZE: execution_time_ms=([0-9.]+) cache_hits=(\d+) cache_misses=(\d+) evictions=(\d+) page_accesses=(\d+)",
            analyzed,
        )
        assert match is not None, analyzed
        hits = int(match.group(2))
        misses = int(match.group(3))
        accesses = int(match.group(5))
        assert accesses == hits + misses, analyzed

        unsupported = run_session(
            executable,
            db_path,
            [
                "EXPLAIN SELECT * FROM users JOIN profiles ON users.username = profiles.username;",
                ".exit",
            ],
        )
        assert "query requires a cross-table/index path that is not routed safely yet" in unsupported, unsupported

        print("PASS: cross-root JOIN EXPLAIN/EXPLAIN ANALYZE verified")
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
