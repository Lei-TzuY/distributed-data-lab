import glob
import os
import re
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
    return None


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
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def scalar_results(output):
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_stats.db")
    value_range = db_file + ".idx_metrics_value.idx.range"
    value_stats = value_range + ".stats"
    bucket_range = db_file + ".idx_metrics_bucket.idx.range"
    bucket_stats = bucket_range + ".stats"
    label_range = db_file + ".idx_metrics_label.idx.range"
    label_stats = label_range + ".stats"
    cleanup(db_file)

    try:
        setup_commands = [
            "CREATE TABLE metrics (id INT, label VARCHAR, value INT, bucket INT);"
        ]
        for row_id in range(1, 129):
            setup_commands.append(
                "INSERT INTO metrics VALUES "
                f"({row_id}, 'item{row_id:03d}', {row_id}, {row_id % 4});"
            )
        setup_commands.extend(
            [
                "CREATE INDEX idx_metrics_value ON metrics(value);",
                "CREATE INDEX idx_metrics_bucket ON metrics(bucket);",
                "CREATE INDEX idx_metrics_label ON metrics(label);",
                ".exit",
            ]
        )
        setup = run_session(executable, db_file, setup_commands)
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        for path in (
            value_range,
            value_stats,
            bucket_range,
            bucket_stats,
            label_range,
            label_stats,
        ):
            if os.path.exists(path):
                raise AssertionError("CREATE INDEX unexpectedly materialized optimizer sidecars: " + path)

        planned = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM metrics WHERE value >= 120;",
                "EXPLAIN SELECT id FROM metrics WHERE value >= 100 AND bucket = 3;",
                "EXPLAIN SELECT id FROM metrics WHERE label >= 'item120';",
                ".exit",
            ],
        )
        require(planned, "ESTIMATED ROWS:")
        require(planned, "ESTIMATED COST:")
        if not os.path.exists(value_stats):
            raise AssertionError("value optimizer statistics were not persisted")
        if not os.path.exists(bucket_stats):
            raise AssertionError("bucket optimizer statistics were not persisted")
        if not os.path.exists(label_stats):
            raise AssertionError("VARCHAR optimizer statistics were not persisted")
        for path in (value_range, bucket_range, label_range):
            if os.path.exists(path):
                raise AssertionError("EXPLAIN costing materialized a full range snapshot: " + path)

        reopened = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM metrics WHERE value >= 120;",
                "EXPLAIN SELECT id FROM metrics WHERE label >= 'item120';",
                ".exit",
            ],
        )
        require(reopened, "ESTIMATED ROWS:")
        if os.path.exists(value_range) or os.path.exists(label_range):
            raise AssertionError("clean reopen failed to reuse optimizer statistics")

        with open(label_stats, "wb") as handle:
            handle.write(b"corrupt")
        repaired = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM metrics WHERE label >= 'item120';",
                ".exit",
            ],
        )
        require(repaired, "ESTIMATED ROWS:")
        if os.path.getsize(label_stats) <= len(b"corrupt"):
            raise AssertionError("corrupt optimizer statistics were not rebuilt")
        if os.path.exists(label_range):
            raise AssertionError("statistics repair unnecessarily built a full VARCHAR range snapshot")

        value_stats_before = read_bytes(value_stats)
        bucket_stats_before = read_bytes(bucket_stats)
        mutation = run_session(
            executable,
            db_file,
            [
                "UPDATE metrics SET value = 1000 WHERE id = 1;",
                "EXPLAIN SELECT id FROM metrics WHERE value >= 100 AND bucket = 3;",
                ".exit",
            ],
        )
        require(mutation, "ESTIMATED ROWS:")
        if read_bytes(value_stats) == value_stats_before:
            raise AssertionError("epoch-stale value statistics were reused after mutation")
        if read_bytes(bucket_stats) == bucket_stats_before:
            raise AssertionError("database-global epoch did not invalidate bucket statistics")
        if os.path.exists(value_range) or os.path.exists(bucket_range):
            raise AssertionError("stale statistics refresh unnecessarily built range snapshots")

        executed = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM metrics WHERE value >= 120;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if scalar_results(executed) != [10]:
            raise AssertionError("statistics layer changed SELECT correctness\n" + executed)
        require(executed, "ok")
        if not os.path.exists(value_range):
            raise AssertionError("real indexed execution did not materialize its candidate snapshot")

        dropped = run_session(
            executable,
            db_file,
            [
                "DROP INDEX idx_metrics_value;",
                "DROP INDEX idx_metrics_bucket;",
                "DROP INDEX idx_metrics_label;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(dropped, "ok")
        for path in (
            value_range,
            value_stats,
            bucket_range,
            bucket_stats,
            label_range,
            label_stats,
        ):
            if os.path.exists(path):
                raise AssertionError("DROP INDEX left optimizer sidecar behind: " + path)

        print(
            "PASS: large generic-index costing persists checksummed epoch-bound INT/VARCHAR "
            "statistics, reuses them across reopen without materializing full candidate "
            "snapshots, repairs corrupt/stale statistics, preserves execution correctness, "
            "and removes all optimizer sidecars on DROP INDEX."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
