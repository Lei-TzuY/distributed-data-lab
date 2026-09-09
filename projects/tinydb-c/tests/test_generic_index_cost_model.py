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

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_cost_model.db")
    cleanup(db_file)

    try:
        setup_commands = [
            "CREATE TABLE products (id INT, name VARCHAR, price INT, stock INT);",
        ]
        setup_commands.extend(
            f"INSERT INTO products VALUES ({i}, 'p{i:02d}', {i}, {i % 2});"
            for i in range(1, 21)
        )
        setup_commands.extend(
            [
                "CREATE INDEX idx_products_price ON products(price);",
                "CREATE INDEX idx_products_stock ON products(stock);",
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
                "EXPLAIN SELECT id FROM products WHERE price = 20 AND name LIKE 'p%';",
                "SELECT COUNT(*) FROM products WHERE price = 20 AND name LIKE 'p%';",
                "EXPLAIN SELECT id FROM products WHERE price >= 11 AND stock = 1;",
                "SELECT COUNT(*) FROM products WHERE price >= 11 AND stock = 1;",
                "EXPLAIN SELECT id FROM products WHERE price = 20 AND stock >= 0;",
                "SELECT COUNT(*) FROM products WHERE price = 20 AND stock >= 0;",
                "EXPLAIN SELECT id FROM products WHERE price >= 1 AND stock >= 0;",
                "SELECT COUNT(*) FROM products WHERE price >= 1 AND stock >= 0;",
                "EXPLAIN SELECT id FROM products WHERE price >= 18 AND price <= 20;",
                "SELECT COUNT(*) FROM products WHERE price >= 18 AND price <= 20;",
                "EXPLAIN SELECT id FROM products WHERE price >= 1 AND price <= 20;",
                "SELECT COUNT(*) FROM products WHERE price >= 1 AND price <= 20;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(planned, "PLAN: GENERIC SECONDARY INDEX + RESIDUAL FILTER")
        require(planned, "INDEX: idx_products_price")
        require(planned, "ANCHOR: price = 20")
        require(planned, "ESTIMATED ROWS: 1 / 20")
        require(planned, "ESTIMATED COST: 5 (scan 60)")

        require(planned, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(planned, "INDEXES: 2")
        require(planned, "ESTIMATED ROWS: 5 / 20")
        require(planned, "ESTIMATED COST: 40 (scan 60)")
        require(planned, "FILTER: price >= 11 AND stock = 1")

        if planned.count("PLAN: GENERIC SECONDARY INDEX INTERSECTION") != 1:
            raise AssertionError(
                "cost model used intersection when a narrower anchor or scan was cheaper\n"
                + planned
            )

        if planned.count("PLAN: GENERIC SCHEMA-AWARE TABLE SCAN") < 2:
            raise AssertionError(
                "broad indexed predicates were not allowed to fall back to table scan\n"
                + planned
            )

        require(planned, "PLAN: GENERIC SECONDARY INDEX FUSED RANGE")
        require(planned, "RANGE TERMS: 2 on price")
        require(planned, "ESTIMATED ROWS: 3 / 20")
        require(planned, "ESTIMATED COST: 15 (scan 60)")

        if scalar_results(planned) != [1, 5, 1, 20, 3, 20]:
            raise AssertionError("cost-based routing changed SELECT semantics\n" + planned)
        require(planned, "ok")

        reopened = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM products WHERE price >= 11 AND stock = 1;",
                "EXPLAIN SELECT id FROM products WHERE price >= 1 AND stock >= 0;",
                "SELECT COUNT(*) FROM products WHERE price >= 11 AND stock = 1;",
                "SELECT COUNT(*) FROM products WHERE price >= 1 AND stock >= 0;",
                ".exit",
            ],
        )
        require(reopened, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(reopened, "ESTIMATED COST: 40 (scan 60)")
        require(reopened, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        if scalar_results(reopened) != [5, 20]:
            raise AssertionError("cost decisions changed after clean reopen\n" + reopened)

        print(
            "PASS: generic flat-AND planning uses exact typed-index candidate counts to "
            "choose selective anchors, fused ranges, multi-index intersection, or a "
            "schema-aware table scan, with stable decisions after reopen."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
