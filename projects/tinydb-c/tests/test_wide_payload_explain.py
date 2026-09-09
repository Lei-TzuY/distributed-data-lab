import glob
import os
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


def reject_fixed_carrier_error(output):
    if "schema row does not fit the current fixed B+ tree value slot" in output:
        raise AssertionError("wide EXPLAIN fell back to fixed-record schema validation\n" + output)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_wide_payload_explain.db")
    cleanup(db_file)

    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE wide_docs (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_docs VALUES (10, 'left-a', 'right-a');",
                "INSERT INTO wide_docs VALUES (20, 'left-b', 'right-b');",
                "INSERT INTO wide_docs VALUES (30, 'left-c', 'right-c');",
                "CREATE INDEX idx_wide_right ON wide_docs (right_text);",
                ".exit",
            ],
        )
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        point = run_session(
            executable,
            db_file,
            ["EXPLAIN SELECT left_text FROM wide_docs WHERE id = 20;", ".exit"],
        )
        reject_fixed_carrier_error(point)
        require(point, "PLAN: GENERIC PRIMARY KEY LOOKUP")
        require(point, "TABLE: wide_docs (root page ")
        require(point, "PROJECTION: left_text")
        require(point, "FILTER: id = 20")
        if "left-b" in point:
            raise AssertionError("plain wide EXPLAIN executed the query\n" + point)

        indexed = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT left_text FROM wide_docs WHERE right_text = 'right-b';",
                ".exit",
            ],
        )
        reject_fixed_carrier_error(indexed)
        require(indexed, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require(indexed, "INDEX: idx_wide_right")
        require(indexed, "FILTER: right_text = 'right-b'")
        require(indexed, "ESTIMATED ROWS: 1 / 3")
        require(indexed, "ESTIMATED COST:")
        if "left-b" in indexed:
            raise AssertionError("plain indexed wide EXPLAIN executed the query\n" + indexed)

        primary_range = run_session(
            executable,
            db_file,
            ["EXPLAIN SELECT * FROM wide_docs WHERE id >= 20;", ".exit"],
        )
        reject_fixed_carrier_error(primary_range)
        require(primary_range, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(primary_range, "FILTER: id >= 20")

        indexed_range = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT left_text FROM wide_docs WHERE right_text >= 'right-b';",
                ".exit",
            ],
        )
        reject_fixed_carrier_error(indexed_range)
        require(indexed_range, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require(indexed_range, "INDEX: idx_wide_right")
        require(indexed_range, "FILTER: right_text >= 'right-b'")
        require(indexed_range, "ESTIMATED ROWS: 2 / 3")
        require(indexed_range, "ESTIMATED COST:")

        fused_range = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT COUNT(*) FROM wide_docs WHERE right_text >= 'right-a' AND right_text < 'right-c';",
                ".exit",
            ],
        )
        reject_fixed_carrier_error(fused_range)
        require(fused_range, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require(fused_range, "INDEX: idx_wide_right")
        require(fused_range, "right_text >= 'right-a'")
        require(fused_range, "right_text < 'right-c'")
        require(fused_range, "ESTIMATED ROWS: 2 / 3")
        require(fused_range, "ESTIMATED COST:")

        compound = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT right_text FROM wide_docs WHERE id >= 10 AND right_text = 'right-b';",
                ".exit",
            ],
        )
        reject_fixed_carrier_error(compound)
        require(compound, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require(compound, "INDEX: idx_wide_right")
        require(compound, "PROJECTION: right_text")
        require(compound, "id >= 10")
        require(compound, "right_text = 'right-b'")
        require(compound, "ESTIMATED ROWS: 1 / 3")

        reopened = run_session(
            executable,
            db_file,
            [
                "EXPLAIN ANALYZE SELECT left_text FROM wide_docs WHERE right_text >= 'right-b' AND id >= 20;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_fixed_carrier_error(reopened)
        require(reopened, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require(reopened, "INDEX: idx_wide_right")
        require(reopened, "ESTIMATED ROWS: 2 / 3")
        require(reopened, "ESTIMATED COST:")
        require(reopened, "ACTUAL RESULT")
        require(reopened, "left-b")
        require(reopened, "left-c")
        require(reopened, "ok")

        print(
            "PASS: payload-sized tables participate in primary-key, secondary-index equality/range, "
            "fused-bound, residual, costed, and reopened EXPLAIN planning without a fixed-record carrier."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
