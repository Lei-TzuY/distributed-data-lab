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


def reject_fallback(output):
    for marker in (
        "fixed generic record slot",
        "fixed B+ tree value slot",
        "TinyDBRecord",
        "schema row does not fit",
        "Syntax error. Could not parse statement.",
    ):
        if marker in output:
            raise AssertionError(
                f"wide parenthesized SQL hit unexpected guard {marker!r}\n{output}"
            )


def require_integrity(output, minimum=1):
    if output.count("db > ok\nExecuted.") < minimum:
        raise AssertionError("integrity_check failed\n" + output)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_wide_sql_parenthesized_predicates.db"
    )
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                # Bare VARCHAR columns reserve 256 bytes each, forcing all of
                # these grouped predicates through schema-sized payload APIs.
                "CREATE TABLE wide_grouped (id INT, left_text VARCHAR, right_text VARCHAR);",
                "INSERT INTO wide_grouped VALUES (10, 'alpha', 'head');",
                "INSERT INTO wide_grouped VALUES (20, 'beta', 'middle');",
                "INSERT INTO wide_grouped VALUES (30, 'gamma', 'middle');",
                "INSERT INTO wide_grouped VALUES (40, 'delta', 'middle');",
                "INSERT INTO wide_grouped VALUES (50, 'epsilon', 'tail');",
                "INSERT INTO wide_grouped VALUES (60, 'zeta', 'last');",
                "SELECT COUNT(*) FROM wide_grouped WHERE (id < 20 OR id >= 50) AND (left_text = 'alpha' OR left_text = 'zeta');",
                "SELECT COUNT(*) FROM wide_grouped WHERE id = 20 OR (id >= 40 AND (right_text = 'tail' OR right_text = 'last'));",
                "SELECT id FROM wide_grouped WHERE id = 20 OR (id >= 40 AND (right_text = 'tail' OR right_text = 'last')) LIMIT 2;",
                "SELECT * FROM wide_grouped WHERE (id >= 20 AND id <= 50) OR id = 60 LIMIT 2 OFFSET 2;",
                "UPDATE wide_grouped SET right_text = 'grouped' WHERE (id = 10 OR id >= 50) AND (left_text = 'alpha' OR left_text = 'zeta');",
                "SELECT COUNT(*) FROM wide_grouped WHERE right_text = 'grouped';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_fallback(first)
        if scalar_results(first)[:3] != [2, 3, 2]:
            raise AssertionError("wide grouped SELECT/UPDATE counts were wrong\n" + first)
        normalized = first.replace("db > ", "")
        if "20\n50\n" not in normalized:
            raise AssertionError("nested grouped id projection was wrong\n" + first)
        if "(40, delta, middle)" not in first or "(50, epsilon, tail)" not in first:
            raise AssertionError("grouped STAR LIMIT/OFFSET output was wrong\n" + first)
        if "(30, gamma, middle)" in first or "(60, zeta, last)" in first:
            raise AssertionError("grouped STAR LIMIT/OFFSET leaked rows outside its window\n" + first)
        require_integrity(first)

        second = run_session(
            executable,
            db_path,
            [
                "BEGIN;",
                "UPDATE wide_grouped SET left_text = 'rolled' WHERE (id = 20 OR id = 30) AND right_text = 'middle';",
                "DELETE FROM wide_grouped WHERE (id = 40 OR id = 50) AND (right_text = 'middle' OR right_text = 'tail');",
                "SELECT COUNT(*) FROM wide_grouped;",
                "SELECT COUNT(*) FROM wide_grouped WHERE left_text = 'rolled';",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM wide_grouped;",
                "SELECT COUNT(*) FROM wide_grouped WHERE left_text = 'rolled';",
                "SELECT COUNT(*) FROM wide_grouped WHERE right_text = 'grouped';",
                "PRAGMA integrity_check;",
                "BEGIN;",
                "UPDATE wide_grouped SET left_text = 'committed' WHERE (id = 20 OR id >= 50) AND (right_text = 'middle' OR right_text = 'grouped');",
                "DELETE FROM wide_grouped WHERE (id < 20 OR id = 40) AND (right_text = 'grouped' OR right_text = 'middle');",
                "COMMIT;",
                "SELECT COUNT(*) FROM wide_grouped;",
                "SELECT COUNT(*) FROM wide_grouped WHERE left_text = 'committed';",
                "SELECT COUNT(*) FROM wide_grouped WHERE right_text = 'grouped';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_fallback(second)
        if scalar_results(second) != [4, 2, 6, 0, 2, 4, 2, 1]:
            raise AssertionError("wide grouped rollback/commit state was wrong\n" + second)
        require_integrity(second, 2)

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT COUNT(*) FROM wide_grouped;",
                "SELECT COUNT(*) FROM wide_grouped WHERE left_text = 'committed';",
                "SELECT COUNT(*) FROM wide_grouped WHERE right_text = 'grouped';",
                "SELECT COUNT(*) FROM wide_grouped WHERE (id = 20 OR id >= 50) AND (left_text = 'committed' OR right_text = 'tail');",
                "SELECT left_text FROM wide_grouped WHERE (id = 20 OR id = 50) AND (left_text = 'committed' OR right_text = 'tail');",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject_fallback(reopened)
        if scalar_results(reopened)[:4] != [4, 2, 1, 3]:
            raise AssertionError("reopened wide grouped counts were wrong\n" + reopened)
        normalized_reopen = reopened.replace("db > ", "")
        if "committed\nepsilon\n" not in normalized_reopen:
            raise AssertionError("reopened grouped column projection was wrong\n" + reopened)
        require_integrity(reopened)

        print(
            "PASS: 516-byte SQL rows execute payload-native nested/parenthesized "
            "boolean SELECT/UPDATE/DELETE with projection, LIMIT/OFFSET, explicit "
            "rollback/commit, reopen durability, and integrity_check without "
            "narrowing through TinyDBRecord."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
