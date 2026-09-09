import glob
import os
import subprocess
import sys


def find_binary(repo_root, stem):
    candidates = [
        os.path.join(repo_root, "build", "Debug", stem + ".exe"),
        os.path.join(repo_root, "build", "Release", stem + ".exe"),
        os.path.join(repo_root, "build", stem + ".exe"),
        os.path.join(repo_root, "build", stem),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError(f"Could not find {stem}")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_repl(tinydb, db_path, commands):
    result = subprocess.run(
        [tinydb, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout + result.stderr


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tinydb = find_binary(repo_root, "tinydb")
    probe = find_binary(repo_root, "tinydb_committed_crash_probe")
    db_path = os.path.join(
        os.path.dirname(__file__), "test_committed_multi_root_crash.db"
    )
    cleanup(db_path)

    try:
        # 180 rows per root forces many B+ tree leaf pages across two roots,
        # comfortably exceeding the 16-frame buffer pool before COMMIT. The C
        # probe waits for COMMIT to return, checks that the WAL is durable and
        # non-empty, then _exit()s without tinydb_close/checkpoint.
        result = subprocess.run(
            [probe, db_path, "1", "180", "archive"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        assert result.returncode == 0, result.stdout + "\n" + result.stderr

        wal_path = db_path + ".wal"
        assert os.path.exists(wal_path), "committed WAL missing before recovery"
        assert os.path.getsize(wal_path) > 0, "committed WAL is empty before recovery"

        output = run_repl(
            tinydb,
            db_path,
            [
                "SELECT COUNT(*) FROM users;",
                "SELECT COUNT(*) FROM archive;",
                "SELECT * FROM users WHERE id = 1;",
                "SELECT * FROM users WHERE id = 180;",
                "SELECT * FROM archive WHERE id = 1;",
                "SELECT * FROM archive WHERE id = 180;",
                "SELECT * FROM users JOIN archive ON users.id = archive.id LIMIT 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        if output.count("db > 180\nExecuted.") < 2:
            raise AssertionError("both roots did not recover all 180 committed rows\n" + output)
        for marker in [
            "(1, durable_1, durable1@crash.test)",
            "(180, durable_180, durable180@crash.test)",
            "(1, archive_1, archive1@crash.test)",
            "(180, archive_180, archive180@crash.test)",
            "(1, durable_1, durable1@crash.test) | (1, archive_1, archive1@crash.test)",
            "ok",
        ]:
            if marker not in output:
                raise AssertionError(f"missing recovery marker {marker!r}\n{output}")

        # Recovery/checkpoint on open must consume the committed WAL.
        if os.path.exists(wal_path) and os.path.getsize(wal_path) > 0:
            raise AssertionError("committed WAL was not checkpointed after successful recovery")

        print(
            "PASS: post-COMMIT hard exit recovers 180 users + 180 archive rows "
            "atomically across buffer-pool pressure and preserves cross-root integrity."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
