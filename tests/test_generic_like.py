import glob
import os
import re
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
    process = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if process.returncode != 0:
        raise AssertionError(process.stdout + "\n" + process.stderr)
    return process.stdout + process.stderr


def scalar_results(output):
    return [
        int(value)
        for value in re.findall(r"db > (\d+)\nExecuted\.", output)
    ]


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def require_scalars(output, expected):
    actual = scalar_results(output)
    if actual != expected:
        raise AssertionError(
            f"scalar results {actual!r}, expected {expected!r}\n{output}"
        )


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_like.db")
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE docs (id INT, name VARCHAR, category INT, price INT);",
                "INSERT INTO docs VALUES (1, 'alpha', 1, 10);",
                "INSERT INTO docs VALUES (2, 'alphabet', 1, 20);",
                "INSERT INTO docs VALUES (3, 'beta', 2, 30);",
                "INSERT INTO docs VALUES (4, 'alpine', 3, 40);",
                "INSERT INTO docs VALUES (5, 'omega', 1, 50);",
                "INSERT INTO docs VALUES (6, 'alphanumeric', 2, 60);",
                "CREATE INDEX idx_docs_name ON docs(name);",
                "CREATE INDEX idx_docs_price ON docs(price);",
                "EXPLAIN SELECT id FROM docs WHERE name LIKE 'alp%';",
                "SELECT COUNT(*) FROM docs WHERE name LIKE 'alp%';",
                "SELECT COUNT(*) FROM docs WHERE name LIKE 'a_ph_';",
                "SELECT COUNT(*) FROM docs WHERE name LIKE '%ta';",
                "SELECT COUNT(*) FROM docs WHERE name LIKE 'ALP%';",
                "SELECT COUNT(*) FROM docs WHERE name LIKE '%';",
                "EXPLAIN SELECT id FROM docs WHERE name LIKE 'alp%' AND price >= 20;",
                "SELECT COUNT(*) FROM docs WHERE name LIKE 'alp%' AND price >= 20;",
                "EXPLAIN SELECT id FROM docs WHERE name LIKE 'alp%' AND price = 60;",
                "SELECT COUNT(*) FROM docs WHERE name LIKE 'alp%' AND price = 60;",
                "SELECT COUNT(*) FROM docs WHERE name LIKE 'bet%' OR category = 3;",
                "SELECT COUNT(*) FROM docs WHERE (name LIKE 'alp%' OR name LIKE 'bet%') AND price >= 30;",
                "EXPLAIN ANALYZE SELECT id FROM docs WHERE (name LIKE 'alp%' OR category = 3) AND price >= 20 LIMIT 2;",
                "UPDATE docs SET category = 9 WHERE name LIKE 'alp%';",
                "SELECT COUNT(*) FROM docs WHERE category = 9;",
                "BEGIN;",
                "UPDATE docs SET price = 99 WHERE (name LIKE 'alp%' OR name LIKE 'bet%') AND price >= 30;",
                "SELECT COUNT(*) FROM docs WHERE price = 99;",
                "ROLLBACK;",
                "SELECT COUNT(*) FROM docs WHERE price = 99;",
                "DELETE FROM docs WHERE name LIKE '%ta' OR (category = 9 AND price >= 40);",
                "SELECT COUNT(*) FROM docs;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        if first.count("PLAN: GENERIC SCHEMA-AWARE TABLE SCAN") < 2:
            raise AssertionError("LIKE scan or broad-anchor scan fallback was not selected\n" + first)
        require(first, "FILTER: name LIKE 'alp%'")
        require(first, "FILTER: name LIKE 'alp%' AND price >= 20")
        if "ANCHOR: price >= 20" in first:
            raise AssertionError("broad LIKE residual anchor ignored scan cost\n" + first)
        require(first, "PLAN: GENERIC SECONDARY INDEX + RESIDUAL FILTER")
        require(first, "INDEX: idx_docs_price")
        require(first, "ANCHOR: price = 60")
        require(first, "ESTIMATED ROWS: 1 / 6")
        require(first, "FILTER: name LIKE 'alp%' AND price = 60")
        require(first, "QUERY PLAN")
        require(first, "ACTUAL RESULT")
        require(first, "ok")
        if "PLAN: GENERIC SECONDARY INDEX RANGE SCAN" in first:
            raise AssertionError("LIKE was incorrectly planned as an ordered range scan\n" + first)
        if "ANCHOR: name LIKE" in first:
            raise AssertionError("LIKE was incorrectly selected as an index anchor\n" + first)
        require_scalars(first, [4, 1, 1, 0, 6, 3, 1, 2, 3, 4, 3, 0, 3])

        reopened = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM docs;",
                "SELECT COUNT(*) FROM docs WHERE name LIKE 'alp%';",
                "SELECT COUNT(*) FROM docs WHERE category = 9;",
                "SELECT COUNT(*) FROM docs WHERE name LIKE '%ga';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require_scalars(reopened, [3, 2, 2, 1])
        require(reopened, "ok")

        invalid = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM docs WHERE price LIKE '2%';",
                "SELECT COUNT(*) FROM docs WHERE id LIKE '1%';",
                "SELECT COUNT(*) FROM docs WHERE name LIKE 123;",
                "UPDATE docs SET price = 7 WHERE price LIKE '4%';",
                ".exit",
            ],
        )
        if invalid.count("Syntax error. Could not parse statement.") != 4:
            raise AssertionError("invalid generic LIKE predicates were accepted\n" + invalid)

        print(
            "PASS: generic VARCHAR LIKE supports %/_ wildcards across SELECT, AND/OR, "
            "parenthesized predicates, UPDATE/DELETE, EXPLAIN ANALYZE, rollback and "
            "reopen durability; LIKE stays residual-only while cost-aware planning chooses "
            "scan for a broad ordered predicate and an index anchor for a selective one."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
