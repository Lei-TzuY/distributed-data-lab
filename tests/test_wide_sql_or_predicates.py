import glob
import os
import re
import subprocess
import sys


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
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def scalar_results(output):
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def reject_legacy_fallback(output):
    for marker in (
        "fixed generic record slot",
        "fixed B+ tree value slot",
        "TinyDBRecord",
        "schema row does not fit",
        "Syntax error. Could not parse statement.",
    ):
        if marker in output:
            raise AssertionError(f"wide OR SELECT hit unexpected legacy/parser guard {marker!r}\n{output}")


def require_integrity(output):
    if "db > ok\nExecuted." not in output:
        raise AssertionError("integrity_check failed\n" + output)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_sql_or_predicates.db")
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                # Bare VARCHAR columns reserve 256 bytes each, keeping the table
                # above the historical 293-byte TinyDBRecord compatibility seam.
                "CREATE TABLE wide_or (id INT, left_text VARCHAR, right_text VARCHAR);",
                "INSERT INTO wide_or VALUES (10, 'alpha', 'head');",
                "INSERT INTO wide_or VALUES (20, 'beta', 'middle');",
                "INSERT INTO wide_or VALUES (30, 'gamma', 'middle');",
                "INSERT INTO wide_or VALUES (40, 'delta', 'middle');",
                "INSERT INTO wide_or VALUES (50, 'epsilon', 'tail');",
                "INSERT INTO wide_or VALUES (60, 'zeta', 'last');",
                "SELECT COUNT(*) FROM wide_or WHERE id < 20 OR id > 50;",
                "SELECT COUNT(*) FROM wide_or WHERE id >= 20 AND id <= 30 OR right_text = 'tail';",
                "SELECT COUNT(*) FROM wide_or WHERE left_text < 'beta' OR left_text >= 'zeta';",
                "SELECT id FROM wide_or WHERE id = 20 OR id = 50;",
                "SELECT * FROM wide_or WHERE id >= 20 AND id <= 40 OR id = 60 LIMIT 2 OFFSET 1;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_legacy_fallback(first)
        if scalar_results(first)[:3] != [2, 3, 2]:
            raise AssertionError("wide OR COUNT results were wrong\n" + first)
        if "20\n50\n" not in first.replace("db > ", ""):
            raise AssertionError("wide OR column projection did not emit ids 20 and 50\n" + first)
        if "(30, gamma, middle)" not in first or "(40, delta, middle)" not in first:
            raise AssertionError("wide OR LIMIT/OFFSET projection was wrong\n" + first)
        if "(20, beta, middle)" in first or "(60, zeta, last)" in first:
            raise AssertionError("wide OR LIMIT/OFFSET emitted rows outside the requested window\n" + first)
        require_integrity(first)

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT COUNT(*) FROM wide_or WHERE id <= 20 OR right_text = 'tail';",
                "SELECT COUNT(*) FROM wide_or WHERE id >= 20 AND id < 40 OR left_text = 'zeta';",
                "SELECT left_text FROM wide_or WHERE id = 10 OR id = 60;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_legacy_fallback(reopened)
        if scalar_results(reopened)[:2] != [3, 3]:
            raise AssertionError("reopened wide OR counts were wrong\n" + reopened)
        if "alpha\nzeta\n" not in reopened.replace("db > ", ""):
            raise AssertionError("reopened wide OR projection was wrong\n" + reopened)
        require_integrity(reopened)

        print(
            "PASS: 516-byte SQL rows execute payload-native OR-of-AND predicates "
            "across INT/VARCHAR comparisons with COUNT, column/STAR projection, "
            "LIMIT/OFFSET, reopen durability, and integrity_check without narrowing "
            "through TinyDBRecord."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
