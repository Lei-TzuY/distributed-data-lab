import glob
import os
import subprocess
import sys


def find_tinydb(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_file, commands):
    result = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def reject(output, marker):
    if marker in output:
        raise AssertionError(f"unexpected marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    source_path = os.path.join(repo_root, "src", "generic_wide_statement_atomicity.c")
    with open(source_path, "r", encoding="utf-8") as handle:
        source = handle.read()
    for marker in [
        "tinydb_generic_index_collect_candidates",
        "tinydb_record_payload_find",
        "tinydb_record_payload_decode_values",
        "tinydb_generic_predicate_matches",
        "tinydb_generic_index_candidates_free",
    ]:
        if marker not in source:
            raise AssertionError(f"wide index execution seam missing {marker}")

    db_file = os.path.join(os.path.dirname(__file__), "test_wide_index_candidate_execution.db")
    cleanup(db_file)
    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE wide_docs (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_docs VALUES (10, 'left-a', 'bucket-x');",
                "INSERT INTO wide_docs VALUES (20, 'left-b', 'bucket-y');",
                "INSERT INTO wide_docs VALUES (30, 'left-c', 'bucket-x');",
                "INSERT INTO wide_docs VALUES (40, 'left-d', 'bucket-z');",
                "CREATE INDEX idx_wide_right ON wide_docs (right_text);",
                ".exit",
            ],
        )
        reject(setup, "Error:")
        reject(setup, "Syntax error")

        first = run_session(
            executable,
            db_file,
            [
                "SELECT left_text FROM wide_docs WHERE right_text = 'bucket-x';",
                "SELECT COUNT(*) FROM wide_docs WHERE right_text = 'bucket-x';",
                ".exit",
            ],
        )
        reject(first, "fixed B+ tree value slot")
        reject(first, "unable to decode")
        require(first, "left-a")
        require(first, "left-c")
        require(first, "2")

        reopened = run_session(
            executable,
            db_file,
            [
                "SELECT * FROM wide_docs WHERE right_text = 'bucket-y';",
                "SELECT left_text FROM wide_docs WHERE right_text = 'missing';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        reject(reopened, "fixed B+ tree value slot")
        reject(reopened, "unable to decode")
        require(reopened, "left-b")
        require(reopened, "bucket-y")
        require(reopened, "ok")

        print(
            "PASS: schema-sized equality SELECT uses secondary-index candidates, "
            "payload-native row fetch/decode, residual recheck, and survives reopen."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
