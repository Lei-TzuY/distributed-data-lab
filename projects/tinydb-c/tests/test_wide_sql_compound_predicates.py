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


def reject_narrow_or_syntax_failure(output):
    for marker in (
        "Syntax error. Could not parse statement.",
        "fixed generic record slot",
        "fixed B+ tree value slot",
        "schema row does not fit",
        "TinyDBRecord",
        "unable to decode generic record during predicate SELECT",
    ):
        if marker in output:
            raise AssertionError(
                f"wide compound SELECT hit unsupported/narrow path {marker!r}\n{output}"
            )


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_wide_sql_compound_predicates.db"
    )
    cleanup(db_path)

    payload = "R" * 250
    tags = "abcdefghij"

    try:
        commands = [
            "CREATE TABLE wide_compound (id INT, tag VARCHAR, payload VARCHAR);",
        ]
        for index, tag in enumerate(tags, start=1):
            row_id = index * 10
            commands.append(
                f"INSERT INTO wide_compound VALUES ({row_id}, '{tag}', '{payload}');"
            )
        commands.extend(
            [
                "SELECT COUNT(*) FROM wide_compound WHERE id >= 30 AND id <= 70;",
                "SELECT COUNT(*) FROM wide_compound WHERE id > 30 AND id < 70;",
                "SELECT COUNT(*) FROM wide_compound WHERE id >= 30 AND id <= 70 AND tag >= 'e';",
                "SELECT COUNT(*) FROM wide_compound WHERE tag >= 'c' AND tag < 'h';",
                "SELECT COUNT(*) FROM wide_compound WHERE id >= 20 AND tag = 'b';",
                "SELECT COUNT(*) FROM wide_compound WHERE id > 70 AND id < 30;",
                "SELECT COUNT(*) FROM wide_compound WHERE id > 4294967295 AND tag >= 'a';",
                "SELECT COUNT(*) FROM wide_compound WHERE id < 0 AND tag >= 'a';",
                "PRAGMA integrity_check;",
                ".exit",
            ]
        )
        first = run_session(executable, db_path, commands)
        reject_narrow_or_syntax_failure(first)
        counts = scalar_results(first)
        expected = [5, 3, 3, 5, 1, 0, 0, 0]
        if counts != expected:
            raise AssertionError(
                f"unexpected wide compound counts {counts!r}, expected {expected!r}\n{first}"
            )
        if "db > ok\nExecuted." not in first:
            raise AssertionError("integrity_check failed after wide compound SELECTs\n" + first)

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT COUNT(*) FROM wide_compound WHERE id >= 40 AND id <= 90;",
                "SELECT COUNT(*) FROM wide_compound WHERE tag > 'd' AND tag <= 'i';",
                "SELECT COUNT(*) FROM wide_compound WHERE id >= 40 AND id <= 90 AND tag < 'g';",
                "SELECT COUNT(*) FROM wide_compound WHERE id = 50 AND tag = 'e';",
                "SELECT COUNT(*) FROM wide_compound WHERE id = 50 AND tag = 'x';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_narrow_or_syntax_failure(reopened)
        reopened_counts = scalar_results(reopened)
        reopened_expected = [6, 5, 3, 1, 0]
        if reopened_counts != reopened_expected:
            raise AssertionError(
                f"unexpected reopened wide compound counts {reopened_counts!r}, "
                f"expected {reopened_expected!r}\n{reopened}"
            )
        if "db > ok\nExecuted." not in reopened:
            raise AssertionError(
                "reopened integrity_check failed after wide compound SELECTs\n" + reopened
            )

        print(
            "PASS: 516-byte SQL rows support typed AND predicates without narrowing through "
            "TinyDBRecord; primary-key bounds are intersected for payload range scans, VARCHAR "
            "predicates remain residual filters, contradictions are empty, and reopen preserves results."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
