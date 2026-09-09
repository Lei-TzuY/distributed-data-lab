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


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


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


def write_orphan_temp(path, payload):
    temporary = path + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(payload)
        handle.flush()
        os.fsync(handle.fileno())
    return temporary


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_generic_index_epoch_temp_recovery.db"
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

        epoch_path = db_path + ".gidx.epoch"
        if not os.path.exists(epoch_path):
            raise AssertionError("generic-index epoch authority was not created")
        before_read = read_bytes(epoch_path)
        snapshots_before = set(glob.glob(db_path + "*.range"))
        if not snapshots_before:
            raise AssertionError("expected a persistent generic-index snapshot")
        snapshot_path = next(iter(snapshots_before))
        snapshot_bytes = read_bytes(snapshot_path)

        orphan_epoch = write_orphan_temp(
            epoch_path, b"interrupted-epoch-publication"
        )
        orphan_snapshot = write_orphan_temp(
            snapshot_path, b"interrupted-range-publication"
        )
        second = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if 20 not in projected_ids(second):
            raise AssertionError(second)
        require(second, "ok")
        if os.path.exists(orphan_epoch):
            raise AssertionError(
                "read-side recovery must remove an orphan epoch publication temp file"
            )
        if os.path.exists(orphan_snapshot):
            raise AssertionError(
                "read-side recovery must remove an orphan range publication temp file"
            )
        if read_bytes(epoch_path) != before_read:
            raise AssertionError(
                "discarding orphan temps beside valid metadata must not replace epoch authority"
            )
        if read_bytes(snapshot_path) != snapshot_bytes:
            raise AssertionError(
                "discarding a non-authoritative range temp must preserve the valid final snapshot"
            )
        if not snapshots_before.issubset(set(glob.glob(db_path + "*.range"))):
            raise AssertionError(
                "valid snapshots must survive cleanup of non-authoritative temp files"
            )

        before_interrupted_mutation = read_bytes(epoch_path)
        interrupted = run_session(
            executable,
            db_path,
            [
                "UPDATE wide_docs SET right_text = 'interrupted' WHERE id = 20;",
                ".exit",
            ],
            {
                "TINYDB_TEST_FAIL_GENERIC_INDEX_EPOCH_BEFORE_REPLACE": "1",
            },
        )
        interrupted_temp = epoch_path + ".tmp"
        if not os.path.exists(interrupted_temp):
            raise AssertionError(
                "failpoint must leave the fully synced pre-publication epoch temp behind"
            )
        if read_bytes(epoch_path) != before_interrupted_mutation:
            raise AssertionError(
                "interruption before atomic replacement must preserve the old epoch authority"
            )
        if "Executed." in interrupted:
            raise AssertionError(
                "indexed mutation must not report success when epoch publication is interrupted"
            )

        recovered = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                "SELECT id FROM wide_docs WHERE right_text = 'interrupted';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        recovered_ids = projected_ids(recovered)
        if recovered_ids != [20]:
            raise AssertionError(
                "failed epoch publication must leave the indexed row unchanged\n" + recovered
            )
        require(recovered, "ok")
        if os.path.exists(interrupted_temp):
            raise AssertionError(
                "next read barrier must discard the interrupted durable epoch temp"
            )
        if read_bytes(epoch_path) != before_interrupted_mutation:
            raise AssertionError(
                "recovering a pre-replace interruption must retain the old epoch authority"
            )

        orphan_epoch = write_orphan_temp(epoch_path, b"stale-epoch-temp")
        orphan_snapshot = write_orphan_temp(snapshot_path, b"stale-range-temp")
        before_mutation = read_bytes(epoch_path)
        third = run_session(
            executable,
            db_path,
            [
                "UPDATE wide_docs SET right_text = 'right-c' WHERE id = 20;",
                "SELECT id FROM wide_docs WHERE right_text = 'right-c';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if 20 not in projected_ids(third):
            raise AssertionError(third)
        require(third, "ok")
        if os.path.exists(orphan_epoch):
            raise AssertionError(
                "mutation-side epoch barrier must remove an orphan epoch publication temp"
            )
        if os.path.exists(orphan_snapshot):
            raise AssertionError(
                "mutation-side epoch barrier must remove an orphan range publication temp"
            )
        if read_bytes(epoch_path) == before_mutation:
            raise AssertionError("indexed mutation must still advance durable epoch authority")

        fourth = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE right_text = 'right-c';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if 20 not in projected_ids(fourth):
            raise AssertionError(fourth)
        require(fourth, "ok")

        print(
            "PASS: generic-index epoch barriers clean interrupted epoch and range snapshot "
            "publications without replacing valid authority; failed epoch publication remains "
            "fail-closed and indexed rows stay correct across reopen."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
