import glob
import os
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


def run_session(executable, db_path, commands, extra_env=None):
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
        env=env,
    )
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout + result.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_alter_recovered_v2_multileaf_retry.db"
    )
    cleanup(db_path)

    payload = "p" * 240
    tail = "t" * 30

    try:
        setup = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE docs (id INT, payload VARCHAR(250));",
                "INSERT INTO docs VALUES (1, 'alpha');",
                "INSERT INTO docs VALUES (2, 'beta');",
                "ALTER TABLE docs ADD COLUMN tail VARCHAR(37);",
                ".exit",
            ],
        )
        require(setup, "Column 'tail' added to table 'docs'.")

        interrupted = run_session(
            executable,
            db_path,
            [
                "ALTER TABLE docs ADD COLUMN overflow VARCHAR(1);",
                "PRAGMA table_info(docs);",
                "PRAGMA integrity_check;",
                ".exit",
            ],
            {"TINYDB_TEST_FAIL_ALTER_CATALOG_SAVE": "1"},
        )
        require(interrupted, "schema catalog could not be persisted")
        require(interrupted, "ok")
        if "overflow | VARCHAR(1)" in interrupted:
            raise AssertionError("failed ALTER leaked the new schema\n" + interrupted)

        growth_commands = []
        for row_id in range(3, 35):
            growth_commands.append(
                f"INSERT INTO docs VALUES ({row_id}, '{payload}', '{tail}');"
            )
        growth_commands.extend(
            [
                "SELECT * FROM docs WHERE id = 34;",
                "PRAGMA integrity_check;",
                ".exit",
            ]
        )
        grown = run_session(executable, db_path, growth_commands)
        require(grown, f"(34, {payload}, {tail})")
        require(grown, "ok")

        retried = run_session(
            executable,
            db_path,
            [
                "ALTER TABLE docs ADD COLUMN overflow VARCHAR(1);",
                "SELECT * FROM docs WHERE id = 1;",
                "SELECT * FROM docs WHERE id = 34;",
                "INSERT INTO docs VALUES (35, 'after', 'retry', 'x');",
                "SELECT * FROM docs WHERE id = 35;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(retried, "Column 'overflow' added to table 'docs'.")
        require(retried, "(1, alpha, , )")
        require(retried, f"(34, {payload}, {tail}, )")
        require(retried, "(35, after, retry, x)")
        require(retried, "ok")
        if "table-rebuild migration for multi-leaf fixed storage" in retried:
            raise AssertionError(
                "recovered compact V2 multi-leaf tree was misclassified as fixed storage\n"
                + retried
            )

        reopened = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(docs);",
                "SELECT * FROM docs WHERE id = 34;",
                "SELECT * FROM docs WHERE id = 35;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        for marker in [
            "overflow | VARCHAR(1)",
            f"(34, {payload}, {tail}, )",
            "(35, after, retry, x)",
            "ok",
        ]:
            require(reopened, marker)

        print(
            "PASS: a failed wide ALTER can leave a backward-readable compact V2 "
            "physical upgrade, the old schema can grow that tree through leaf splits, "
            "and a later append-only ALTER retry succeeds across the recovered multi-leaf tree."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
