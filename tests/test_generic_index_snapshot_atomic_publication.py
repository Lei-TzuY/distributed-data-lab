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
            if os.path.isfile(path) or os.path.islink(path):
                os.remove(path)
        except OSError:
            pass


def run_session(executable, db_path, commands, extra_env=None):
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
        env=env,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError(output)
    return output


def projected_ids(output):
    values = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if line.startswith("db > "):
            line = line[5:].strip()
        if line.isdigit():
            values.append(int(line))
    return values


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_generic_index_snapshot_atomic_publication.db"
    )
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE wide_docs (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_docs VALUES (10, 'left-a', 'right-a');",
                "INSERT INTO wide_docs VALUES (20, 'left-b', 'right-b');",
                "CREATE INDEX idx_wide_right ON wide_docs (right_text);",
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                ".exit",
            ],
        )
        if 20 not in projected_ids(first):
            raise AssertionError(first)

        snapshots = glob.glob(db_path + "*.range")
        if len(snapshots) != 1:
            raise AssertionError(f"expected one range snapshot, found {snapshots!r}")
        snapshot_path = snapshots[0]
        before = read_bytes(snapshot_path)

        interrupted = run_session(
            executable,
            db_path,
            [
                "UPDATE wide_docs SET right_text = 'right-c' WHERE id = 20;",
                "SELECT id FROM wide_docs WHERE right_text = 'right-c';",
                ".exit",
            ],
            {
                "TINYDB_TEST_FAIL_GENERIC_INDEX_SNAPSHOT_BEFORE_REPLACE": "1",
            },
        )
        if 20 not in projected_ids(interrupted):
            raise AssertionError(interrupted)

        temporary = snapshot_path + ".tmp"
        if not os.path.exists(temporary):
            raise AssertionError(
                "pre-replace failpoint must leave the fully synced snapshot temp behind"
            )
        if read_bytes(snapshot_path) != before:
            raise AssertionError(
                "interrupted snapshot publication must preserve the previous complete final"
            )

        recovered = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE right_text = 'right-c';",
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if projected_ids(recovered) != [20]:
            raise AssertionError(
                "reopen must rebuild from authoritative rows after interrupted publication\n"
                + recovered
            )
        if "ok" not in recovered:
            raise AssertionError(recovered)
        if os.path.exists(temporary):
            raise AssertionError("reopen must discard the orphan snapshot temp")
        if read_bytes(snapshot_path) == before:
            raise AssertionError(
                "successful recovery rebuild must atomically replace the stale final snapshot"
            )

        print(
            "PASS: generic-index range snapshots publish through a durable temp and atomic "
            "replace; interruption preserves the old final and reopen rebuilds cleanly."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
