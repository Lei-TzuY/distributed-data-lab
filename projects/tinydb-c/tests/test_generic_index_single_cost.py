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


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_single_cost.db")
    cleanup(db_file)

    try:
        setup_commands = [
            "CREATE TABLE products (id INT, bucket INT, price INT);",
        ]
        setup_commands.extend(
            f"INSERT INTO products VALUES ({i}, {1 if i > 36 else 0}, {i});"
            for i in range(1, 41)
        )
        setup_commands.extend(
            [
                "CREATE INDEX idx_products_bucket ON products(bucket);",
                "CREATE INDEX idx_products_price ON products(price);",
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
                "EXPLAIN SELECT id FROM products WHERE bucket = 1;",
                "SELECT COUNT(*) FROM products WHERE bucket = 1;",
                "EXPLAIN SELECT id FROM products WHERE bucket = 0;",
                "SELECT COUNT(*) FROM products WHERE bucket = 0;",
                "EXPLAIN SELECT id FROM products WHERE price >= 37;",
                "SELECT COUNT(*) FROM products WHERE price >= 37;",
                "EXPLAIN SELECT id FROM products WHERE price >= 1;",
                "SELECT COUNT(*) FROM products WHERE price >= 1;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(planned, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require(planned, "INDEX: idx_products_bucket")
        require(planned, "PLAN: GENERIC SECONDARY INDEX RANGE SCAN")
        require(planned, "INDEX: idx_products_price")
        require(planned, "ESTIMATED ROWS: 4 / 40")
        require(planned, "ESTIMATED COST: 20 (scan 120)")
        if planned.count("PLAN: GENERIC SCHEMA-AWARE TABLE SCAN") < 2:
            raise AssertionError(
                "broad single-predicate equality/range indexes did not fall back to scan\n"
                + planned
            )
        if planned.count("COST CHOICE: table scan cheaper than single secondary index") < 2:
            raise AssertionError("single-index scan decisions were not exposed in EXPLAIN\n" + planned)
        require(planned, "ESTIMATED ROWS: 36 / 40")
        require(planned, "ESTIMATED COST: 180 (scan 120)")
        require(planned, "ESTIMATED ROWS: 40 / 40")
        require(planned, "ESTIMATED COST: 200 (scan 120)")
        if scalar_results(planned) != [4, 36, 4, 40]:
            raise AssertionError("single-predicate cost routing changed SELECT semantics\n" + planned)
        require(planned, "ok")

        reopened = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM products WHERE bucket = 1;",
                "EXPLAIN SELECT id FROM products WHERE bucket = 0;",
                "EXPLAIN SELECT id FROM products WHERE price >= 37;",
                "EXPLAIN SELECT id FROM products WHERE price >= 1;",
                "SELECT COUNT(*) FROM products WHERE bucket = 1;",
                "SELECT COUNT(*) FROM products WHERE bucket = 0;",
                "SELECT COUNT(*) FROM products WHERE price >= 37;",
                "SELECT COUNT(*) FROM products WHERE price >= 1;",
                ".exit",
            ],
        )
        require(reopened, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require(reopened, "PLAN: GENERIC SECONDARY INDEX RANGE SCAN")
        if reopened.count("PLAN: GENERIC SCHEMA-AWARE TABLE SCAN") < 2:
            raise AssertionError("single-predicate cost choices changed after reopen\n" + reopened)
        if scalar_results(reopened) != [4, 36, 4, 40]:
            raise AssertionError("single-predicate results changed after reopen\n" + reopened)

        print(
            "PASS: single generic equality/range predicates use typed candidate cardinality "
            "to retain selective secondary indexes, reject broad indexes for table scans, "
            "preserve SQL results, and keep the same decisions after reopen."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
