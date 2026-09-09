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


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(
        os.path.dirname(__file__), "test_multi_table_prepared_routing.db"
    )
    cleanup(db_file)

    try:
        output = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                "INSERT INTO archive VALUES (1, 'archive1', 'a1@test.com');",
                "INSERT INTO archive VALUES (2, 'archive2', 'a2@test.com');",
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                "INSERT INTO products VALUES (10, 'cheap', 10);",
                "INSERT INTO products VALUES (20, 'mid', 20);",
                "INSERT INTO products VALUES (30, 'high', 30);",
                "CREATE INDEX idx_products_price ON products(price);",
                "PREPARE archive_lookup FROM SELECT * FROM archive WHERE id = ?;",
                "EXECUTE archive_lookup USING 2;",
                "PREPARE product_floor FROM SELECT name FROM products WHERE price >= ?;",
                "EXECUTE product_floor USING 20;",
                "PREPARE product_plan FROM EXPLAIN SELECT name FROM products WHERE price >= ?;",
                "EXECUTE product_plan USING 20;",
                "PREPARE archive_delete FROM DELETE FROM archive WHERE id = ?;",
                "BEGIN;",
                "EXECUTE archive_delete USING 1;",
                "ROLLBACK;",
                "EXECUTE archive_lookup USING 1;",
                "PREPARE blocked_vacuum FROM VACUUM;",
                "EXECUTE blocked_vacuum;",
                # Bare VARCHAR makes this a 516-byte executable generic schema.
                # Prepared execution must re-enter the current public CREATE TABLE
                # policy and initialize a payload-native V2 root, not preserve the
                # obsolete fixed TinyDBRecord rejection.
                "PREPARE wide_create FROM CREATE TABLE wide_prepared (id INT, a VARCHAR, b VARCHAR);",
                "EXECUTE wide_create;",
                "INSERT INTO wide_prepared VALUES (77, 'prepared-left', 'prepared-right');",
                "SELECT * FROM wide_prepared WHERE id = 77;",
                "PREPARE recur FROM EXECUTE recur USING ?;",
                "EXECUTE recur USING 1;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(output, "Statement 'archive_lookup' prepared.")
        require(output, "(2, archive2, a2@test.com)")
        require(output, "Statement 'product_floor' prepared.")
        require(output, "mid")
        require(output, "high")
        require(output, "PLAN: GENERIC SECONDARY INDEX RANGE SCAN")
        require(output, "INDEX: idx_products_price")
        require(output, "Statement 'archive_delete' prepared.")
        require(output, "(1, archive1, a1@test.com)")
        require(
            output,
            "VACUUM/VACUUM INTO is disabled for multi-table databases",
        )
        require(output, "Statement 'wide_create' prepared.")
        require(output, "(77, prepared-left, prepared-right)")
        if "fixed generic record slot" in output or "schema-sized payload limit" in output:
            raise AssertionError("prepared wide CREATE hit an obsolete storage guard\n" + output)
        require(output, "prepared statement nesting exceeds the safe execution limit")
        require(output, "ok")

        if "EXECUTE PREPARED is disabled for multi-table databases" in output:
            raise AssertionError("prepared execution still hit the old multi-root policy guard\n" + output)

        print(
            "PASS: prepared SQL is rebound through the public engine pipeline, routes to "
            "non-zero fixed-row and generic roots, reuses generic indexed EXPLAIN/execution, "
            "participates in transaction rollback, keeps VACUUM policy enforcement, admits "
            "schema-sized prepared CREATE through V2 payload storage, and rejects recursive "
            "EXECUTE chains safely."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
