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

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_alter.db")
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                "INSERT INTO products VALUES (1, 'keyboard', 2599);",
                "ALTER TABLE products ADD COLUMN stock INT;",
                ".schema products",
                "SELECT * FROM products WHERE id = 1;",
                "INSERT INTO products VALUES (2, 'mouse', 1299, 7);",
                "UPDATE products SET stock = 5 WHERE id = 1;",
                "SELECT stock FROM products WHERE id = 1;",
                "SELECT * FROM products WHERE stock = 7;",
                "ALTER TABLE products ADD COLUMN notes VARCHAR;",
                ".schema products",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(first, "Column 'stock' added to table 'products'.")
        require(first, "stock            INT          offset=264 size=4")
        require(first, "(1, keyboard, 2599, 0)")
        require(first, "db > 5\nExecuted.")
        require(first, "(2, mouse, 1299, 7)")
        require(
            first,
            "ALTER TABLE ADD COLUMN would exceed the fixed generic record slot; variable-size row migration is not implemented",
        )
        require(first, "ok")
        if "notes            VARCHAR" in first:
            raise AssertionError("oversized column was added despite fixed-slot guard\n" + first)

        second = run_session(
            executable,
            db_file,
            [
                ".schema products",
                "SELECT * FROM products WHERE id = 1;",
                "SELECT * FROM products WHERE id = 2;",
                "SELECT COUNT(*) FROM products WHERE stock = 7;",
                "INSERT INTO products VALUES (3, 'cable', 399, 11);",
                "SELECT stock FROM products WHERE id = 3;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        require(second, "stock            INT          offset=264 size=4")
        require(second, "(1, keyboard, 2599, 5)")
        require(second, "(2, mouse, 1299, 7)")
        require(second, "db > 1\nExecuted.")
        require(second, "db > 11\nExecuted.")
        require(second, "ok")
        if "notes            VARCHAR" in second:
            raise AssertionError("rejected oversized column persisted in catalog\n" + second)

        print(
            "PASS: generic ADD COLUMN uses zero-filled fixed-slot tail and rejects oversized schema expansion."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
