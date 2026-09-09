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


def scalar_results(output):
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def reject(output, marker):
    if marker in output:
        raise AssertionError(f"unexpected marker {marker!r}\n{output}")


def require_scalars(output, expected):
    actual = scalar_results(output)
    if actual != expected:
        raise AssertionError(f"scalar results {actual!r}, expected {expected!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    source_path = os.path.join(repo_root, "src", "generic_wide_boolean_cost_route.c")
    with open(source_path, "r", encoding="utf-8") as handle:
        source = handle.read()
    for marker in [
        "WIDE_BOOLEAN_COST_MIN_TABLE_ROWS",
        "tinydb_generic_index_estimate_candidates",
        "candidate_cost > estimate->scan_cost",
        "tinydb_generic_sql_try_execute_wide_atomic_base",
        "table scan cheaper than wide boolean candidate plan",
    ]:
        if marker not in source:
            raise AssertionError(f"wide boolean cost seam missing {marker}")

    db_file = os.path.join(os.path.dirname(__file__), "test_wide_boolean_index_cost.db")
    cleanup(db_file)
    try:
        commands = [
            "CREATE TABLE wide_cost (id INT, left_text VARCHAR(145), right_text VARCHAR(145), price INT);"
        ]
        commands.extend(
            f"INSERT INTO wide_cost VALUES ({i}, 'left-{i}', 'right-{i}', {i});"
            for i in range(1, 41)
        )
        commands.extend([
            "CREATE INDEX idx_wide_cost_price ON wide_cost (price);",
            ".exit",
        ])
        setup = run_session(executable, db_file, commands)
        reject(setup, "Error:")
        reject(setup, "Syntax error")

        first = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT COUNT(*) FROM wide_cost WHERE price <= 35 OR price >= 5;",
                "SELECT COUNT(*) FROM wide_cost WHERE price <= 35 OR price >= 5;",
                "EXPLAIN SELECT COUNT(*) FROM wide_cost WHERE price <= 2 OR price >= 39;",
                "SELECT COUNT(*) FROM wide_cost WHERE price <= 2 OR price >= 39;",
                "EXPLAIN SELECT COUNT(*) FROM wide_cost WHERE (price <= 2 OR price >= 39) AND id >= 1;",
                "SELECT COUNT(*) FROM wide_cost WHERE (price <= 2 OR price >= 39) AND id >= 1;",
                "EXPLAIN SELECT id FROM wide_cost WHERE price <= 35 OR price >= 5 LIMIT 1;",
                "SELECT id FROM wide_cost WHERE price <= 35 OR price >= 5 LIMIT 1;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject(first, "fixed B+ tree value slot")
        reject(first, "unable to decode")
        reject(first, "COST CHOICE: table scan cheaper than single secondary index")
        require(first, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(first, "COST CHOICE: table scan cheaper than wide boolean candidate plan")
        require(first, "PLAN: GENERIC INDEX UNION")
        require(first, "ESTIMATED COST:")
        require_scalars(first, [40, 4, 4, 1])
        require(first, "db > 1\nExecuted.")
        require(first, "ok")

        reopened = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT COUNT(*) FROM wide_cost WHERE price <= 35 OR price >= 5;",
                "SELECT COUNT(*) FROM wide_cost WHERE price <= 35 OR price >= 5;",
                "EXPLAIN SELECT COUNT(*) FROM wide_cost WHERE price <= 2 OR price >= 39;",
                "SELECT COUNT(*) FROM wide_cost WHERE price <= 2 OR price >= 39;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject(reopened, "COST CHOICE: table scan cheaper than single secondary index")
        require(reopened, "COST CHOICE: table scan cheaper than wide boolean candidate plan")
        require(reopened, "PLAN: GENERIC INDEX UNION")
        require_scalars(reopened, [40, 4])
        require(reopened, "ok")

        print(
            "PASS: wide boolean candidate routing applies the shared cost model, "
            "falls back to scans for broad OR predicates, keeps selective unions, "
            "preserves LIMIT early-stop routing, emits one unambiguous scan-choice "
            "diagnostic, and survives reopen."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
