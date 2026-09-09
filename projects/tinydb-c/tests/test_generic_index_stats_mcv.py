import glob
import os
import re
import struct
import subprocess
import sys


STATS_MAGIC = 0x47495331
STATS_VERSION = 3


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


def stats_version(path):
    with open(path, "rb") as handle:
        header = handle.read(8)
    if len(header) != 8:
        raise AssertionError("optimizer statistics header is truncated")
    magic, version = struct.unpack("<II", header)
    if magic != STATS_MAGIC:
        raise AssertionError(f"unexpected optimizer statistics magic {magic:#x}")
    return version


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(
        os.path.dirname(__file__),
        "test_generic_index_stats_mcv.db",
    )
    bucket_range = db_file + ".idx_skew_bucket.idx.range"
    bucket_stats = bucket_range + ".stats"
    tag_range = db_file + ".idx_skew_tag.idx.range"
    tag_stats = tag_range + ".stats"
    cleanup(db_file)

    try:
        setup_commands = [
            "CREATE TABLE skew (id INT, tag VARCHAR, bucket INT);",
        ]
        for row_id in range(1, 161):
            tag = "hot" if row_id <= 100 else f"item{row_id:03d}"
            bucket = 1 if row_id <= 120 else row_id
            setup_commands.append(
                f"INSERT INTO skew VALUES ({row_id}, '{tag}', {bucket});"
            )
        setup_commands.extend(
            [
                "CREATE INDEX idx_skew_bucket ON skew(bucket);",
                "CREATE INDEX idx_skew_tag ON skew(tag);",
                ".exit",
            ]
        )
        setup = run_session(executable, db_file, setup_commands)
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        planned = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM skew WHERE bucket = 1;",
                "EXPLAIN SELECT id FROM skew WHERE bucket = 160;",
                "EXPLAIN SELECT id FROM skew WHERE tag = 'hot';",
                "EXPLAIN SELECT id FROM skew WHERE tag = 'item160';",
                "EXPLAIN SELECT id FROM skew WHERE bucket <= 1;",
                "EXPLAIN SELECT id FROM skew WHERE bucket > 1;",
                "EXPLAIN SELECT id FROM skew WHERE bucket >= 1;",
                "EXPLAIN SELECT id FROM skew WHERE tag < 'hot';",
                "EXPLAIN SELECT id FROM skew WHERE tag <= 'hot';",
                "EXPLAIN SELECT id FROM skew WHERE tag > 'hot';",
                "EXPLAIN SELECT id FROM skew WHERE tag >= 'hot';",
                ".exit",
            ],
        )

        if planned.count("PLAN: GENERIC SCHEMA-AWARE TABLE SCAN") < 6:
            raise AssertionError(
                "MCV frequencies/bounds did not send broad equality/range predicates to scans\n"
                + planned
            )
        if planned.count("PLAN: GENERIC SECONDARY INDEX LOOKUP") < 2:
            raise AssertionError(
                "rare equality predicates did not retain selective indexes\n" + planned
            )
        if planned.count("PLAN: GENERIC SECONDARY INDEX RANGE SCAN") < 3:
            raise AssertionError(
                "selective MCV-bound range predicates did not retain indexes\n" + planned
            )

        if planned.count("ESTIMATED ROWS: 120 / 160") < 2:
            raise AssertionError("INT MCV equality/LTE boundary was not exact\n" + planned)
        if planned.count("ESTIMATED ROWS: 100 / 160") < 2:
            raise AssertionError("VARCHAR MCV equality/LTE boundary was not exact\n" + planned)
        if planned.count("ESTIMATED ROWS: 160 / 160") < 2:
            raise AssertionError("MCV GTE boundary did not include the full tail\n" + planned)
        require(planned, "ESTIMATED ROWS: 40 / 160")
        require(planned, "ESTIMATED ROWS: 60 / 160")
        require(planned, "ESTIMATED ROWS: 0 / 160")
        if planned.count("ESTIMATED ROWS: 1 / 160") < 2:
            raise AssertionError(
                "rare INT/VARCHAR equality selectivity was not kept near one row\n"
                + planned
            )
        require(planned, "ESTIMATED COST: 600 (scan 480)")
        require(planned, "ESTIMATED COST: 500 (scan 480)")
        require(planned, "ESTIMATED COST: 200 (scan 480)")
        require(planned, "ESTIMATED COST: 300 (scan 480)")
        if planned.count("COST CHOICE: table scan cheaper than single secondary index") < 6:
            raise AssertionError("hot-value scan choices were not exposed in EXPLAIN\n" + planned)

        for path in (bucket_stats, tag_stats):
            if not os.path.exists(path):
                raise AssertionError("MCV optimizer statistics were not persisted: " + path)
            if stats_version(path) != STATS_VERSION:
                raise AssertionError("optimizer statistics were not upgraded to V3")
        if os.path.exists(bucket_range) or os.path.exists(tag_range):
            raise AssertionError("MCV EXPLAIN costing materialized full candidate snapshots")

        with open(bucket_stats, "r+b") as handle:
            handle.seek(4)
            handle.write(struct.pack("<I", 2))
        if stats_version(bucket_stats) != 2:
            raise AssertionError("failed to stage a legacy V2 statistics version marker")

        upgraded = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM skew WHERE bucket <= 1;",
                "EXPLAIN SELECT id FROM skew WHERE bucket > 1;",
                ".exit",
            ],
        )
        require(upgraded, "ESTIMATED ROWS: 120 / 160")
        require(upgraded, "ESTIMATED ROWS: 40 / 160")
        if stats_version(bucket_stats) != STATS_VERSION:
            raise AssertionError("legacy/corrupt V2 optimizer statistics were not rebuilt as V3")
        if os.path.exists(bucket_range):
            raise AssertionError("V2 statistics rebuild unnecessarily materialized range data")

        reopened = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM skew WHERE bucket <= 1;",
                "EXPLAIN SELECT id FROM skew WHERE bucket > 1;",
                "EXPLAIN SELECT id FROM skew WHERE tag <= 'hot';",
                "EXPLAIN SELECT id FROM skew WHERE tag > 'hot';",
                ".exit",
            ],
        )
        require(reopened, "ESTIMATED ROWS: 120 / 160")
        require(reopened, "ESTIMATED ROWS: 40 / 160")
        require(reopened, "ESTIMATED ROWS: 100 / 160")
        require(reopened, "ESTIMATED ROWS: 60 / 160")
        if os.path.exists(bucket_range) or os.path.exists(tag_range):
            raise AssertionError("clean reopen failed to reuse MCV optimizer statistics")

        executed = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM skew WHERE bucket = 1;",
                "SELECT COUNT(*) FROM skew WHERE bucket = 160;",
                "SELECT COUNT(*) FROM skew WHERE tag = 'hot';",
                "SELECT COUNT(*) FROM skew WHERE tag = 'item160';",
                "SELECT COUNT(*) FROM skew WHERE bucket <= 1;",
                "SELECT COUNT(*) FROM skew WHERE bucket > 1;",
                "SELECT COUNT(*) FROM skew WHERE bucket >= 1;",
                "SELECT COUNT(*) FROM skew WHERE tag < 'hot';",
                "SELECT COUNT(*) FROM skew WHERE tag <= 'hot';",
                "SELECT COUNT(*) FROM skew WHERE tag > 'hot';",
                "SELECT COUNT(*) FROM skew WHERE tag >= 'hot';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if scalar_results(executed) != [120, 1, 100, 1, 120, 40, 160, 0, 100, 60, 160]:
            raise AssertionError("MCV range costing changed query semantics\n" + executed)
        require(executed, "ok")

        print(
            "PASS: persisted generic-index statistics V3 retain top-frequency INT/VARCHAR "
            "MCVs with exact rows-before boundaries, cost equality and range predicates at "
            "MCV literals exactly, keep selective tails on indexes, rebuild legacy/corrupt "
            "V2 statistics, reuse V3 stats after reopen, and preserve execution correctness."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
