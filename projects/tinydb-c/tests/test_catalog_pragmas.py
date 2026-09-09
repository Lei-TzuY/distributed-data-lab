import glob
import os
import subprocess
import sys


def find_executable(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find tinydb executable")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_path, commands):
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=90,
    )
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_catalog_pragmas.db")
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE products (name VARCHAR, price INT);",
                "ALTER TABLE products ADD COLUMN quantity INT;",
                "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                "CREATE INDEX idx_users_username ON users(username);",
                "PRAGMA table_info;",
                "PRAGMA table_info(products);",
                "PRAGMA table_info(archive);",
                "PRAGMA index_list(users);",
                "PRAGMA index_list(archive);",
                "PRAGMA table_info(missing_table);",
                ".exit",
            ],
        )

        # Bare id/username/email remains the historical Row ABI and its
        # catalog metadata is normalized to the physical serialized widths.
        assert "username | VARCHAR(32)" in first, first
        assert "email | VARCHAR(255)" in first, first
        assert "name | VARCHAR(255)" in first, first
        assert "price | INT" in first, first
        assert "quantity | INT" in first, first
        assert "idx_users_username" in first, first
        assert "(no indexes found)" in first, first
        assert "table 'missing_table' not found" in first, first

        second = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(products);",
                "PRAGMA table_info(archive);",
                "PRAGMA index_list(users);",
                ".exit",
            ],
        )
        assert "quantity | INT" in second, second
        assert "username | VARCHAR(32)" in second, second
        assert "email | VARCHAR(255)" in second, second
        assert "idx_users_username" in second, second

        print("PASS: catalog-aware PRAGMA table_info/index_list verified")
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
