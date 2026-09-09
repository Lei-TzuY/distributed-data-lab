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

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_or_cost.db")
    cleanup(db_file)

    try:
        setup = ["CREATE TABLE products (id INT, name VARCHAR, price INT, stock INT);"]
        setup.extend(
            f"INSERT INTO products VALUES ({i}, 'p{i:02d}', {i}, {i % 2});"
            for i in range(1, 41)
        )
        setup.extend(
            [
                "CREATE INDEX idx_products_price ON products(price);",
                "CREATE INDEX idx_products_stock ON products(stock);",
                ".exit",
            ]
        )
        created = run_session(executable, db_file, setup)
        if "Error:" in created or "Syntax error" in created:
            raise AssertionError(created)

        planned = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM products WHERE price = 39 OR price = 40;",
                "SELECT COUNT(*) FROM products WHERE price = 39 OR price = 40;",
                "EXPLAIN SELECT id FROM products WHERE price >= 1 OR stock >= 0;",
                "SELECT COUNT(*) FROM products WHERE price >= 1 OR stock >= 0;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(planned, "PLAN: GENERIC INDEX UNION")
        require(planned, "BRANCHES: 2")
        require(planned, "ESTIMATED ROWS: 2 / 40")
        require(planned, "ESTIMATED COST: 10 (scan 120)")

        require(planned, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(planned, "ESTIMATED ROWS: 40 / 40")
        require(planned, "ESTIMATED COST: 240 (scan 120)")
        require(planned, "COST CHOICE: table scan cheaper than OR index union")

        if scalar_results(planned) != [2, 40]:
            raise AssertionError("cost-aware OR routing changed SELECT semantics\n" + planned)
        require(planned, "ok")

        reopened = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM products WHERE price = 39 OR price = 40;",
                "EXPLAIN SELECT id FROM products WHERE price >= 1 OR stock >= 0;",
                "SELECT COUNT(*) FROM products WHERE price = 39 OR price = 40;",
                "SELECT COUNT(*) FROM products WHERE price >= 1 OR stock >= 0;",
                ".exit",
            ],
        )
        require(reopened, "PLAN: GENERIC INDEX UNION")
        require(reopened, "ESTIMATED COST: 10 (scan 120)")
        require(reopened, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(reopened, "COST CHOICE: table scan cheaper than OR index union")
        if scalar_results(reopened) != [2, 40]:
            raise AssertionError("OR cost decision changed after clean reopen\n" + reopened)

        print(
            "PASS: flat OR planning retains selective index union, falls back to a "
            "schema-aware scan when union materialization/random fetch is costlier, and "
            "keeps the same decisions and results after reopen."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
