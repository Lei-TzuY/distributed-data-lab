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
    process = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if process.returncode != 0:
        raise AssertionError(process.stdout + "\n" + process.stderr)
    return process.stdout + process.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def require_count(output, marker, minimum):
    actual = output.count(marker)
    if actual < minimum:
        raise AssertionError(
            f"marker {marker!r} occurred {actual} times, expected >= {minimum}\n{output}"
        )


def forbid(output, marker):
    if marker in output:
        raise AssertionError(f"unexpected marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_table_index.db")
    cleanup(db_file)

    try:
        inserts = []
        for row_id in range(1, 13):
            if row_id <= 4:
                price = 100
            elif row_id <= 8:
                price = 200
            else:
                price = 300
            inserts.append(
                f"INSERT INTO products VALUES ({row_id}, 'p{row_id}', {price});"
            )

        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                *inserts,
                "CREATE INDEX idx_products_price ON products(price);",
                "EXPLAIN SELECT name FROM products WHERE price = 200;",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "SELECT name FROM products WHERE price = 200 LIMIT 2 OFFSET 1;",
                "EXPLAIN ANALYZE SELECT name FROM products WHERE price = 200 LIMIT 1;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(first, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require(first, "INDEX: idx_products_price")
        require(first, "FILTER: price = 200")
        require(first, "db > 4\nExecuted.")
        require(first, "p6")
        require(first, "p7")
        require(first, "QUERY PLAN")
        require(first, "ACTUAL RESULT")
        require(first, "ok")

        second = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT * FROM products WHERE price = 200;",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "UPDATE products SET price = 200 WHERE id = 1;",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "BEGIN;",
                "UPDATE products SET price = 200 WHERE id = 2;",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "DELETE FROM products WHERE id = 5;",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(second, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require_count(second, "db > 4\nExecuted.", 2)
        require_count(second, "db > 5\nExecuted.", 2)
        require(second, "db > 6\nExecuted.")
        require(second, "ok")

        third = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price = 200;",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "DROP INDEX idx_products_price;",
                "EXPLAIN SELECT name FROM products WHERE price = 200;",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(third, "PLAN: GENERIC SECONDARY INDEX LOOKUP")
        require(third, "INDEX: idx_products_price")
        require_count(third, "db > 4\nExecuted.", 2)
        require(third, "PLAN: GENERIC SCHEMA-AWARE TABLE SCAN")
        require(third, "ok")

        ddl = run_session(
            executable,
            db_file,
            [
                "CREATE INDEX idx_products_combo ON products(price, name);",
                "CREATE INDEX idx_products_missing ON products(missing);",
                "CREATE INDEX idx_products_id ON products(id);",
                "PRAGMA index_list;",
                ".exit",
            ],
        )
        require(
            ddl,
            "Error: generic table secondary indexes currently support one column only.",
        )
        require(ddl, "Error: Column missing does not exist on table products.")
        require(
            ddl,
            "Error: generic table id is already backed by the primary B+ tree index.",
        )
        require(ddl, "(no indexes found)")
        forbid(ddl, "idx_products_combo   |")
        forbid(ddl, "idx_products_missing |")
        forbid(ddl, "idx_products_id      |")

        print(
            "PASS: generic-table secondary index lookup, reopen, mutation invalidation, "
            "transaction rollback safety, scan fallback, and DDL boundaries verified."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
