import glob
import os
import subprocess
import sys


PAGE_SIZE = 4096


def find_tinydb(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find tinydb")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_repl(executable, db_path, commands):
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tinydb = find_tinydb(repo_root)
    db_file = os.path.join(
        os.path.dirname(__file__), "test_single_table_integrity_ownership.db"
    )
    cleanup(db_file)

    try:
        clean = run_repl(
            tinydb,
            db_file,
            [
                "INSERT INTO users VALUES (1, 'main', 'main@example.com');",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if "ok" not in clean:
            raise AssertionError("clean single-table integrity check failed\n" + clean)

        with open(db_file, "ab") as handle:
            handle.write(b"\0" * PAGE_SIZE)

        corrupt = run_repl(
            tinydb,
            db_file,
            [
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if "allocated but unreachable from every catalog root" not in corrupt:
            raise AssertionError(
                "single-table PRAGMA did not run global ownership validation\n" + corrupt
            )
        if "page ownership:" not in corrupt:
            raise AssertionError("ownership failure was not identified\n" + corrupt)
        if "database integrity check failed" not in corrupt:
            raise AssertionError("engine did not surface integrity failure status\n" + corrupt)
        if "Key not found" in corrupt:
            raise AssertionError(
                "integrity failure leaked the historical key-not-found artifact\n" + corrupt
            )

        print(
            "PASS: single-table PRAGMA integrity_check includes global page ownership "
            "and reports integrity failure without a key-not-found artifact."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
