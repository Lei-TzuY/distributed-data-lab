import json
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
    return next((path for path in candidates if os.path.exists(path)), None)


def cleanup(db_file):
    for suffix in ("", ".wal", ".catalog", ".catalog.wal", ".username.idx", ".username.idx.wal"):
        path = db_file + suffix
        if os.path.exists(path):
            os.remove(path)


def run_test():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    if not executable:
        print("Could not find the tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_page_inspect.db")
    inspector = os.path.join(repo_root, "tools", "page_inspect.py")
    cleanup(db_file)

    try:
        commands = "".join(
            f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@inspect.test');\n"
            for i in range(1, 21)
        ) + ".exit\n"
        seed = subprocess.run(
            [executable, db_file],
            input=commands,
            capture_output=True,
            text=True,
        )
        if seed.returncode != 0:
            print(f"FAIL: seed database creation exited with {seed.returncode}")
            print(seed.stdout)
            print(seed.stderr)
            sys.exit(1)

        result = subprocess.run(
            [sys.executable, inspector, db_file, "--json", "--strict"],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print("FAIL: offline page inspector reported a problem on a clean database")
            print(result.stdout)
            print(result.stderr)
            sys.exit(1)

        report = json.loads(result.stdout)
        if report["total_rows"] != 20:
            print(f"FAIL: inspector expected 20 rows, got {report['total_rows']}")
            sys.exit(1)
        if report["checksum_failures"] != 0 or not report["ok"]:
            print(f"FAIL: inspector reported checksum/header issues: {report['problems']}")
            sys.exit(1)
        if report["leaf_pages"] < 2 or report["internal_pages"] < 1:
            print(
                "FAIL: expected 20 rows to produce a split B+ tree with leaf and internal pages"
            )
            print(result.stdout)
            sys.exit(1)
        if report["root_pages"] != 1:
            print(f"FAIL: expected exactly one root page, got {report['root_pages']}")
            sys.exit(1)

        print("PASS: offline inspector decodes split B+ tree pages and validates checksums.")
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    run_test()
