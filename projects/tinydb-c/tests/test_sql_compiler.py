import os
import subprocess
import sys


def find_executable(base_dir):
    possible_paths = [
        os.path.join(base_dir, 'build', 'Debug', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb'),
    ]
    return next((path for path in possible_paths if os.path.exists(path)), None)


def remove_database(db_file):
    for path in (db_file, db_file + '.wal'):
        if os.path.exists(path):
            os.remove(path)


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    executable_path = find_executable(base_dir)
    if not executable_path:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_sql.db')
    remove_database(db_file)

    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    commands = """INSERT INTO users VALUES (1, 'alice', 'alice@example.com');
INSERT INTO users VALUES (2, 'O''Brien', 'obrien@example.com');
SELECT * FROM users WHERE id = 2;
SELECT * FROM users WHERE id = 99;
EXPLAIN SELECT * FROM users WHERE id = 2;
EXPLAIN SELECT * FROM users;
INSERT INTO users VALUES (-1, 'invalid', 'invalid@example.com');
.exit
"""
    stdout, stderr = process.communicate(input=commands)

    if process.returncode != 0:
        print(f'FAIL: tinydb exited with {process.returncode}: {stderr}')
        sys.exit(1)
    if stdout.count("(2, O'Brien, obrien@example.com)") != 1:
        print('FAIL: Expected one primary-key lookup result.')
        sys.exit(1)
    if '(1, alice, alice@example.com)' in stdout:
        print('FAIL: Point lookup unexpectedly scanned unrelated rows.')
        sys.exit(1)
    if 'PLAN: PRIMARY KEY LOOKUP (id = 2)' not in stdout:
        print('FAIL: Expected primary-key lookup plan.')
        sys.exit(1)
    if 'PLAN: FULL TABLE SCAN' not in stdout:
        print('FAIL: Expected full-table scan plan.')
        sys.exit(1)
    if 'Syntax error. Could not parse statement.' not in stdout:
        print('FAIL: Expected negative id to be rejected.')
        sys.exit(1)

    remove_database(db_file)
    print('PASS: SQL compiler, point lookup, and EXPLAIN work correctly.')


if __name__ == '__main__':
    run_test()
