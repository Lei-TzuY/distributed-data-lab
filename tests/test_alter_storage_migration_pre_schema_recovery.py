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
        os.path.dirname(__file__),
        "test_alter_storage_migration_pre_schema_recovery.db",
    )
    cleanup(db_path)

    edge = "z" * 249
    try:
        setup = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE docs (id INT, payload VARCHAR(250));",
                "INSERT INTO docs VALUES (1, 'alpha');",
                f"INSERT INTO docs VALUES (2, '{edge}');",
                "ALTER TABLE docs ADD COLUMN tail VARCHAR(37);",
                "UPDATE docs SET tail = 'edge' WHERE id = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(setup, "Column 'tail' added to table 'docs'.")
        require(setup, "ok")

        interrupted = run_session(
            executable,
            db_path,
            [
                "ALTER TABLE docs ADD COLUMN overflow VARCHAR(1);",
                "PRAGMA table_info(docs);",
                "SELECT * FROM docs WHERE id = 1;",
                "SELECT * FROM docs WHERE id = 2;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
            {"TINYDB_TEST_FAIL_ALTER_AFTER_STORAGE_MIGRATION": "1"},
        )
        require(
            interrupted,
            "interrupted after physical row migration before schema mutation",
        )
        require(interrupted, "(1, alpha, )")
        require(interrupted, f"(2, {edge}, edge)")
        require(interrupted, "ok")
        if "overflow | VARCHAR(1)" in interrupted:
            raise AssertionError("pre-schema interruption leaked the new schema\n" + interrupted)
        if "Column 'overflow' added to table 'docs'." in interrupted:
            raise AssertionError("pre-schema interruption emitted success\n" + interrupted)

        recovered = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(docs);",
                "SELECT * FROM docs WHERE id = 1;",
                "SELECT * FROM docs WHERE id = 2;",
                "ALTER TABLE docs ADD COLUMN overflow VARCHAR(1);",
                "SELECT * FROM docs WHERE id = 1;",
                "SELECT * FROM docs WHERE id = 2;",
                "INSERT INTO docs VALUES (3, 'gamma', 'new', 'x');",
                "SELECT * FROM docs WHERE id = 3;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(recovered, "(1, alpha, )")
        require(recovered, f"(2, {edge}, edge)")
        require(recovered, "Column 'overflow' added to table 'docs'.")
        require(recovered, "(1, alpha, , )")
        require(recovered, f"(2, {edge}, edge, )")
        require(recovered, "(3, gamma, new, x)")
        require(recovered, "ok")

        reopened = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(docs);",
                "SELECT * FROM docs WHERE id = 1;",
                "SELECT * FROM docs WHERE id = 2;",
                "SELECT * FROM docs WHERE id = 3;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        for marker in [
            "overflow | VARCHAR(1)",
            "(1, alpha, , )",
            f"(2, {edge}, edge, )",
            "(3, gamma, new, x)",
            "ok",
        ]:
            require(reopened, marker)

        print(
            "PASS: a durable fixed-to-compact V2 storage migration remains readable "
            "under the old schema when ALTER is interrupted before schema mutation, "
            "and the same ALTER safely retries and persists after reopen."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
