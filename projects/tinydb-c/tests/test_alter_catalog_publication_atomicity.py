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


def run_session(executable, db_path, commands, env=None):
    process_env = os.environ.copy()
    if env:
        process_env.update(env)
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
        env=process_env,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError(output)
    return output


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def reject(output, marker):
    if marker in output:
        raise AssertionError(f"unexpected marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_alter_catalog_publication_atomicity.db"
    )
    cleanup(db_path)

    try:
        setup = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE wide_docs (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_docs VALUES (1, 'left-a', 'right-a');",
                "CREATE INDEX idx_wide_right ON wide_docs(right_text);",
                "SELECT id FROM wide_docs WHERE right_text = 'right-a';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(setup, "1")
        require(setup, "ok")

        interrupted = run_session(
            executable,
            db_path,
            [
                "ALTER TABLE wide_docs ADD COLUMN tag VARCHAR(5);",
                "PRAGMA table_info(wide_docs);",
                "INSERT INTO wide_docs VALUES (2, 'left-b', 'right-b');",
                "SELECT * FROM wide_docs WHERE id = 1;",
                "SELECT * FROM wide_docs WHERE id = 2;",
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
            env={"TINYDB_TEST_FAIL_ALTER_BEFORE_CATALOG_PERSIST": "1"},
        )
        require(
            interrupted,
            "ALTER TABLE ADD COLUMN interrupted before schema catalog publication",
        )
        require(interrupted, "(1, left-a, right-a)")
        require(interrupted, "(2, left-b, right-b)")
        require(interrupted, "2")
        require(interrupted, "ok")
        reject(interrupted, "tag | VARCHAR")
        reject(interrupted, "(1, left-a, right-a, )")

        reopened = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(wide_docs);",
                "SELECT * FROM wide_docs WHERE id = 1;",
                "SELECT * FROM wide_docs WHERE id = 2;",
                "SELECT id FROM wide_docs WHERE right_text >= 'right-a';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(reopened, "(1, left-a, right-a)")
        require(reopened, "(2, left-b, right-b)")
        require(reopened, "ok")
        reject(reopened, "tag | VARCHAR")
        reject(reopened, "(1, left-a, right-a, )")

        save_failed = run_session(
            executable,
            db_path,
            [
                "ALTER TABLE wide_docs ADD COLUMN tag VARCHAR(5);",
                "PRAGMA table_info(wide_docs);",
                "INSERT INTO wide_docs VALUES (3, 'left-c', 'right-c');",
                "SELECT * FROM wide_docs WHERE id = 3;",
                "SELECT id FROM wide_docs WHERE right_text = 'right-c';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
            env={"TINYDB_TEST_FAIL_ALTER_CATALOG_SAVE": "1"},
        )
        require(save_failed, "schema catalog could not be persisted")
        require(save_failed, "(3, left-c, right-c)")
        require(save_failed, "3")
        require(save_failed, "ok")
        reject(save_failed, "tag | VARCHAR")
        reject(save_failed, "Column 'tag' added to table 'wide_docs'.")

        save_failed_reopen = run_session(
            executable,
            db_path,
            [
                "PRAGMA table_info(wide_docs);",
                "SELECT * FROM wide_docs WHERE id = 3;",
                "SELECT id FROM wide_docs WHERE right_text = 'right-c';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(save_failed_reopen, "(3, left-c, right-c)")
        require(save_failed_reopen, "3")
        require(save_failed_reopen, "ok")
        reject(save_failed_reopen, "tag | VARCHAR")

        duplicate = run_session(
            executable,
            db_path,
            [
                "ALTER TABLE wide_docs ADD COLUMN right_text VARCHAR(5);",
                "PRAGMA table_info(wide_docs);",
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(duplicate, "ALTER TABLE ADD COLUMN cannot reuse an existing column name")
        require(duplicate, "2")
        require(duplicate, "ok")

        print(
            "PASS: append-only ALTER rolls the in-memory schema back both before "
            "catalog publication and when catalog save fails, suppresses premature "
            "success output, preserves old-arity DML/indexes across reopen, and "
            "rejects duplicate-column DDL during preflight."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
