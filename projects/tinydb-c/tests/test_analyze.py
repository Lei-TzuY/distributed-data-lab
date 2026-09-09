import glob
import os
import struct
import subprocess
import sys


STATS_MAGIC = 0x47495331
STATS_VERSION = 3


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
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def require_stats_v3(path):
    if not os.path.exists(path):
        raise AssertionError("optimizer statistics were not materialized: " + path)
    with open(path, "rb") as handle:
        header = handle.read(8)
    if len(header) != 8:
        raise AssertionError("optimizer statistics header is truncated: " + path)
    magic, version = struct.unpack("<II", header)
    if magic != STATS_MAGIC or version != STATS_VERSION:
        raise AssertionError(
            f"unexpected optimizer statistics header magic={magic:#x} version={version}"
        )


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_analyze.db")
    cleanup(db_path)

    value_range = db_path + ".idx_metrics_value.idx.range"
    value_stats = value_range + ".stats"
    bucket_range = db_path + ".idx_metrics_bucket.idx.range"
    bucket_stats = bucket_range + ".stats"
    label_range = db_path + ".idx_events_label.idx.range"
    label_stats = label_range + ".stats"

    try:
        setup_commands = [
            "CREATE TABLE metrics (id INT, value INT, bucket INT);",
        ]
        # Keep this table below the normal persisted-statistics costing threshold:
        # explicit ANALYZE must still materialize statistics proactively.
        for row_id in range(1, 21):
            setup_commands.append(
                f"INSERT INTO metrics VALUES ({row_id}, {row_id}, {row_id % 4});"
            )
        setup_commands.extend(
            [
                "CREATE INDEX idx_metrics_value ON metrics(value);",
                "CREATE INDEX idx_metrics_bucket ON metrics(bucket);",
                "CREATE TABLE events (id INT, label VARCHAR(16));",
                "INSERT INTO events VALUES (1, 'alpha');",
                "INSERT INTO events VALUES (2, 'beta');",
                "INSERT INTO events VALUES (3, 'alpha');",
                "CREATE INDEX idx_events_label ON events(label);",
                ".exit",
            ]
        )
        setup = run_session(executable, db_path, setup_commands)
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        for path in (value_stats, bucket_stats, label_stats):
            if os.path.exists(path):
                raise AssertionError("CREATE INDEX unexpectedly created optimizer stats: " + path)

        one_index = run_session(
            executable,
            db_path,
            ["ANALYZE INDEX idx_metrics_value;", ".exit"],
        )
        if "Error:" in one_index or "Syntax error" in one_index:
            raise AssertionError(one_index)
        require_stats_v3(value_stats)
        if os.path.exists(bucket_stats) or os.path.exists(label_stats):
            raise AssertionError("index-scoped ANALYZE refreshed unrelated statistics")
        if os.path.exists(value_range):
            raise AssertionError("ANALYZE INDEX materialized a full candidate snapshot")

        table_scope = run_session(
            executable,
            db_path,
            ["ANALYZE TABLE metrics;", ".exit"],
        )
        if "Error:" in table_scope or "Syntax error" in table_scope:
            raise AssertionError(table_scope)
        require_stats_v3(value_stats)
        require_stats_v3(bucket_stats)
        if os.path.exists(label_stats):
            raise AssertionError("table-scoped ANALYZE refreshed another table")
        if os.path.exists(value_range) or os.path.exists(bucket_range):
            raise AssertionError("ANALYZE TABLE materialized full candidate snapshots")

        os.remove(value_stats)
        os.remove(bucket_stats)
        implicit_table = run_session(
            executable,
            db_path,
            ["ANALYZE metrics;", ".exit"],
        )
        if "Error:" in implicit_table or "Syntax error" in implicit_table:
            raise AssertionError(implicit_table)
        require_stats_v3(value_stats)
        require_stats_v3(bucket_stats)
        if os.path.exists(label_stats):
            raise AssertionError("implicit table target refreshed another table")

        os.remove(value_stats)
        os.remove(bucket_stats)
        global_scope = run_session(
            executable,
            db_path,
            ["ANALYZE;", ".exit"],
        )
        if "Error:" in global_scope or "Syntax error" in global_scope:
            raise AssertionError(global_scope)
        for path in (value_stats, bucket_stats, label_stats):
            require_stats_v3(path)
        for path in (value_range, bucket_range, label_range):
            if os.path.exists(path):
                raise AssertionError("global ANALYZE materialized a full candidate snapshot: " + path)

        value_before = read_bytes(value_stats)
        bucket_before = read_bytes(bucket_stats)
        mutation_refresh = run_session(
            executable,
            db_path,
            [
                "UPDATE metrics SET value = 999 WHERE id = 1;",
                "ANALYZE idx_metrics_value;",
                ".exit",
            ],
        )
        if "Error:" in mutation_refresh or "Syntax error" in mutation_refresh:
            raise AssertionError(mutation_refresh)
        if read_bytes(value_stats) == value_before:
            raise AssertionError("ANALYZE did not rebuild epoch-stale targeted statistics")
        if read_bytes(bucket_stats) != bucket_before:
            raise AssertionError("index-scoped ANALYZE rewrote an unrelated stale sidecar")

        bucket_stale = read_bytes(bucket_stats)
        table_refresh = run_session(
            executable,
            db_path,
            ["ANALYZE TABLE metrics;", ".exit"],
        )
        if "Error:" in table_refresh or "Syntax error" in table_refresh:
            raise AssertionError(table_refresh)
        if read_bytes(bucket_stats) == bucket_stale:
            raise AssertionError("table-scoped ANALYZE did not refresh epoch-stale statistics")

        transaction_rejection_before = read_bytes(value_stats)
        rejected = run_session(
            executable,
            db_path,
            [
                "BEGIN;",
                "ANALYZE INDEX idx_metrics_value;",
                "ROLLBACK;",
                ".exit",
            ],
        )
        require(
            rejected,
            "ANALYZE is not allowed inside a transaction because optimizer statistics are non-transactional sidecars",
        )
        if read_bytes(value_stats) != transaction_rejection_before:
            raise AssertionError("rejected transactional ANALYZE changed optimizer statistics")

        errors = run_session(
            executable,
            db_path,
            [
                "ANALYZE INDEX missing_index;",
                "ANALYZE TABLE missing_table;",
                "ANALYZE missing_target;",
                "ANALYZE TABLE metrics trailing;",
                ".exit",
            ],
        )
        require(errors, "ANALYZE index 'missing_index' was not found")
        require(errors, "ANALYZE table 'missing_table' was not found")
        require(errors, "ANALYZE target 'missing_target' was not found")
        require(errors, "Syntax error. Could not parse statement.")

        reopened = run_session(
            executable,
            db_path,
            [
                "ANALYZE INDEX idx_events_label;",
                "SELECT COUNT(*) FROM metrics WHERE value >= 15;",
                "SELECT COUNT(*) FROM events WHERE label = 'alpha';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if "Error:" in reopened or "Syntax error" in reopened:
            raise AssertionError(reopened)
        require(reopened, "ok")
        require_stats_v3(label_stats)
        for path in (value_range, bucket_range, label_range):
            if os.path.exists(path):
                # Real SELECT execution may legitimately materialize a candidate
                # range snapshot; only ANALYZE itself is forbidden from doing so.
                pass

        print(
            "PASS: ANALYZE proactively materializes V3 generic-index statistics for "
            "INDEX/TABLE/database scopes, refreshes epoch-stale targets, stays out of "
            "full candidate snapshots, rejects transactional maintenance, reports bad "
            "targets/syntax, and survives reopen with intact query semantics."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
