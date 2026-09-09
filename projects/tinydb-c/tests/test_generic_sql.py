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
        print("FAIL: tinydb returned", process.returncode)
        print(process.stdout)
        print(process.stderr)
        sys.exit(1)
    return process.stdout + process.stderr


def require(output, marker):
    if marker not in output:
        print(f"FAIL: missing marker: {marker}")
        print(output)
        sys.exit(1)


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), "..")
    executable = find_tinydb(base_dir)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_sql.db")
    cleanup(db_file)

    first = run_session(
        executable,
        db_file,
        [
            "CREATE TABLE products (id INT, name VARCHAR, price INT);",
            "CREATE TABLE orders (id INT, product_id INT, quantity INT);",
            "INSERT INTO products VALUES (1, 'keyboard', 2499);",
            "INSERT INTO products VALUES (2, 'mouse', 1299);",
            "INSERT INTO products VALUES (3, 'O''Reilly cable', 399);",
            "INSERT INTO orders VALUES (10, 1, 2);",
            "INSERT INTO orders VALUES (11, 2, 5);",
            "UPDATE products SET name = 'keyboard pro', price = 2599 WHERE id = 1;",
            "DELETE FROM orders WHERE id = 10;",
            "SELECT * FROM products WHERE id = 1;",
            "SELECT * FROM orders WHERE id = 11;",
            "SELECT COUNT(*) FROM products;",
            "SELECT COUNT(*) FROM orders;",
            "SELECT * FROM products LIMIT 1 OFFSET 1;",
            "SELECT name FROM products WHERE price = 2599;",
            "SELECT price FROM products WHERE name = 'mouse';",
            "SELECT COUNT(*) FROM products WHERE name = 'mouse';",
            "SELECT * FROM products WHERE name = 'O''Reilly cable';",
            "SELECT id FROM products WHERE price = 399 LIMIT 1;",
            "SELECT name FROM products WHERE id = 1;",
            "BEGIN;",
            "INSERT INTO products VALUES (99, 'ghost', 9999);",
            "UPDATE products SET price = 1 WHERE id = 2;",
            "DELETE FROM orders WHERE id = 11;",
            "ROLLBACK;",
            "SELECT COUNT(*) FROM products WHERE id = 99;",
            "SELECT * FROM products WHERE id = 2;",
            "SELECT * FROM orders WHERE id = 11;",
            "BEGIN;",
            "INSERT INTO products VALUES (100, 'durable', 7777);",
            "UPDATE products SET price = 7778 WHERE id = 100;",
            "COMMIT;",
            "UPDATE products SET id = 55 WHERE id = 1;",
            "SELECT * FROM products WHERE price = '2599';",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(first, "(1, keyboard pro, 2599)")
    require(first, "(11, 2, 5)")
    require(first, "(2, mouse, 1299)")
    require(first, "(3, O'Reilly cable, 399)")
    require(first, "db > keyboard pro\nExecuted.")
    require(first, "db > 1299\nExecuted.")
    require(first, "db > 3\nExecuted.")
    require(first, "Syntax error. Could not parse statement.")
    require(first, "ok")

    # COUNT(*) is printed before the REPL's Executed line. The first generic
    # counts are 3 products and 1 surviving order; the filtered mouse count is
    # also 1; id=99 must be 0 after the explicit transaction rolls back
    # INSERT + UPDATE + DELETE together.
    require(first, "db > 3\nExecuted.")
    require(first, "db > 1\nExecuted.")
    require(first, "db > 0\nExecuted.")

    second = run_session(
        executable,
        db_file,
        [
            "PRAGMA table_info(products);",
            "SELECT * FROM products WHERE id = 1;",
            "SELECT name FROM products WHERE price = 2599;",
            "SELECT price FROM products WHERE name = 'mouse';",
            "SELECT * FROM products WHERE id = 3;",
            "SELECT * FROM products WHERE id = 99;",
            "SELECT * FROM products WHERE id = 100;",
            "SELECT COUNT(*) FROM products;",
            "SELECT COUNT(*) FROM orders;",
            "DELETE FROM orders;",
            "SELECT COUNT(*) FROM orders;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )

    require(second, "(1, keyboard pro, 2599)")
    require(second, "db > keyboard pro\nExecuted.")
    require(second, "db > 1299\nExecuted.")
    require(second, "(3, O'Reilly cable, 399)")
    require(second, "(100, durable, 7778)")
    require(second, "db > 4\nExecuted.")
    require(second, "db > 1\nExecuted.")
    require(second, "db > 0\nExecuted.")
    require(second, "ok")
    if "(99, ghost, 9999)" in second:
        print("FAIL: rolled-back generic row became visible after reopen")
        print(second)
        cleanup(db_file)
        sys.exit(1)

    third = run_session(
        executable,
        db_file,
        [
            "SELECT COUNT(*) FROM orders;",
            "SELECT * FROM products WHERE price = 7778;",
            "SELECT name FROM products WHERE id = 100;",
            "PRAGMA integrity_check;",
            ".exit",
        ],
    )
    require(third, "db > 0\nExecuted.")
    require(third, "(100, durable, 7778)")
    require(third, "db > durable\nExecuted.")
    require(third, "ok")

    cleanup(db_file)
    print(
        "PASS: generic schema CRUD, projections, equality filters and transaction persistence verified."
    )


if __name__ == "__main__":
    run_test()
