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


def assert_source_contract(repo_root):
    wrapper_path = os.path.join(repo_root, "src", "generic_wide_statement_atomicity.c")
    cmake_path = os.path.join(repo_root, "CMakeLists.txt")
    with open(wrapper_path, "r", encoding="utf-8") as handle:
        wrapper = handle.read()
    with open(cmake_path, "r", encoding="utf-8") as handle:
        cmake = handle.read()

    required = [
        "pager_savepoint(table->pager, savepoint_name)",
        "pager_rollback_to_savepoint(table->pager, savepoint_name)",
        "pager_release_savepoint(table->pager, savepoint_name)",
        "tinydb_generic_sql_try_execute_wide_grouped_base",
        "table->pager->in_transaction",
    ]
    for needle in required:
        if needle not in wrapper:
            raise AssertionError(f"missing wide statement atomicity guard: {needle}")

    if "src/generic_wide_statement_atomicity.c" not in cmake:
        raise AssertionError("wide statement atomicity layer is not compiled")
    rename = (
        "src/generic_wide_grouped_route.c\n"
        "    PROPERTIES COMPILE_DEFINITIONS\n"
        "    \"tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_wide_grouped_base\""
    )
    if rename not in cmake:
        raise AssertionError("wide grouped route is not layered beneath the statement guard")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assert_source_contract(repo_root)
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_wide_statement_savepoint_atomicity.db"
    )
    cleanup(db_path)

    long_text = "x" * 240
    commands = [
        "CREATE TABLE wide_atomic (id INT, left_text VARCHAR, right_text VARCHAR);",
    ]
    for row_id in range(1, 25):
        commands.append(
            f"INSERT INTO wide_atomic VALUES ({row_id}, 's{row_id}', 'r{row_id}');"
        )

    # Prove that at least one large replacement is individually legal. Then
    # roll it back so the following multi-row statement starts from a dense
    # short-row leaf and must exhaust in-place growth capacity part-way through.
    commands.extend(
        [
            "BEGIN;",
            f"UPDATE wide_atomic SET left_text = '{long_text}' WHERE (id = 1);",
            f"SELECT COUNT(*) FROM wide_atomic WHERE left_text = '{long_text}';",
            "ROLLBACK;",
            "BEGIN;",
            f"UPDATE wide_atomic SET left_text = '{long_text}' WHERE (id >= 1 AND id <= 24);",
            f"SELECT COUNT(*) FROM wide_atomic WHERE left_text = '{long_text}';",
            "SELECT COUNT(*) FROM wide_atomic;",
            "UPDATE wide_atomic SET right_text = 'still-live' WHERE (id = 1 OR id = 2);",
            "SELECT COUNT(*) FROM wide_atomic WHERE right_text = 'still-live';",
            "ROLLBACK;",
            f"SELECT COUNT(*) FROM wide_atomic WHERE left_text = '{long_text}';",
            "SELECT COUNT(*) FROM wide_atomic WHERE right_text = 'still-live';",
            "SELECT COUNT(*) FROM wide_atomic;",
            "PRAGMA integrity_check;",
            ".exit",
        ]
    )

    try:
        output = run_session(executable, db_path, commands)
        values = scalar_results(output)
        expected = [1, 0, 24, 2, 0, 0, 24]
        if values != expected:
            raise AssertionError(
                "wide statement savepoint did not preserve statement/outer transaction "
                f"atomicity; expected {expected}, got {values}\n{output}"
            )
        if output.count("db > ok\nExecuted.") < 1:
            raise AssertionError("integrity_check failed after statement rollback\n" + output)

        reopened = run_session(
            executable,
            db_path,
            [
                "SELECT COUNT(*) FROM wide_atomic;",
                f"SELECT COUNT(*) FROM wide_atomic WHERE left_text = '{long_text}';",
                "SELECT COUNT(*) FROM wide_atomic WHERE right_text = 'still-live';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if scalar_results(reopened) != [24, 0, 0]:
            raise AssertionError("reopen exposed partial failed wide mutation\n" + reopened)
        if reopened.count("db > ok\nExecuted.") < 1:
            raise AssertionError("reopened integrity_check failed\n" + reopened)

        print(
            "PASS: wide payload mutations reserve a Pager savepoint inside an outer "
            "transaction; a capacity failure after partial row growth rolls back the "
            "entire SQL statement while keeping the outer transaction usable, and "
            "ROLLBACK/reopen preserve the pre-statement rows."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
