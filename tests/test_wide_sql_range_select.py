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


def reject_fixed_carrier_failure(output):
    for marker in (
        "fixed generic record slot",
        "fixed B+ tree value slot",
        "schema row does not fit",
        "TinyDBRecord",
    ):
        if marker in output:
            raise AssertionError(
                f"schema-sized range SELECT fell back to fixed carrier {marker!r}\n{output}"
            )


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_wide_sql_range_select.db")
    cleanup(db_path)

    left_values = ["alpha", "beta", "delta", "gamma", "omega"]
    right = "R" * 250

    try:
        commands = [
            "CREATE TABLE wide_ranges (id INT, left_text VARCHAR, right_text VARCHAR);",
        ]
        for row_id, left in zip((10, 20, 30, 40, 50), left_values):
            commands.append(
                f"INSERT INTO wide_ranges VALUES ({row_id}, '{left}', '{right}');"
            )
        commands.extend(
            [
                "SELECT COUNT(*) FROM wide_ranges WHERE id > 20;",
                "SELECT COUNT(*) FROM wide_ranges WHERE id >= 30;",
                "SELECT COUNT(*) FROM wide_ranges WHERE id < 30;",
                "SELECT COUNT(*) FROM wide_ranges WHERE id <= 30;",
                "SELECT COUNT(*) FROM wide_ranges WHERE id > 4294967295;",
                "SELECT COUNT(*) FROM wide_ranges WHERE id < 0;",
                "SELECT COUNT(*) FROM wide_ranges WHERE left_text > 'delta';",
                "SELECT COUNT(*) FROM wide_ranges WHERE left_text <= 'delta';",
                "PRAGMA integrity_check;",
                ".exit",
            ]
        )
        first = run_session(executable, db_path, commands)
        reject_fixed_carrier_failure(first)
        expected = [3, 3, 2, 3, 0, 0, 2, 3]
        counts = scalar_results(first)
        if counts != expected:
            raise AssertionError(
                f"unexpected schema-sized range counts {counts!r}, expected {expected!r}\n{first}"
            )
        if "db > ok\nExecuted." not in first:
            raise AssertionError("integrity_check failed after wide range queries\n" + first)

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT COUNT(*) FROM wide_ranges WHERE id >= 20;",
                "SELECT COUNT(*) FROM wide_ranges WHERE id < 50;",
                "SELECT COUNT(*) FROM wide_ranges WHERE left_text >= 'gamma';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_fixed_carrier_failure(reopened)
        reopened_counts = scalar_results(reopened)
        if reopened_counts != [4, 4, 2]:
            raise AssertionError(
                f"unexpected reopened schema-sized range counts {reopened_counts!r}\n{reopened}"
            )
        if "db > ok\nExecuted." not in reopened:
            raise AssertionError("reopened wide range integrity_check failed\n" + reopened)

        print(
            "PASS: 516-byte SQL rows execute integer primary-key and VARCHAR range predicates "
            "through payload-native scans, including empty uint32 boundary ranges, and retain "
            "correct routing after reopen without narrowing through TinyDBRecord."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
