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


def run_commands(executable, db_file, commands):
    process = subprocess.Popen(
        [executable, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, _ = process.communicate(input=commands)
    return process.returncode, stdout


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    executable = find_executable(base_dir)
    if not executable:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_agg.db')
    remove_database(db_file)

    # Seed: ids 3, 7, 10, 15, 20
    _, _ = run_commands(executable, db_file, (
        "INSERT INTO users VALUES (3,  'alice',  'alice@example.com');\n"
        "INSERT INTO users VALUES (7,  'bob',    'bob@example.com');\n"
        "INSERT INTO users VALUES (10, 'carol',  'carol@example.com');\n"
        "INSERT INTO users VALUES (15, 'dave',   'dave@example.com');\n"
        "INSERT INTO users VALUES (20, 'eve',    'eve@example.com');\n"
        ".exit\n"
    ))

    # COUNT(*) full table
    _, stdout = run_commands(executable, db_file, "SELECT COUNT(*) FROM users;\n.exit\n")
    if '5' not in stdout:
        print(f'FAIL: COUNT(*) expected 5, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: COUNT(*) returns correct row count.')

    # MIN(id) full table → 3
    _, stdout = run_commands(executable, db_file, "SELECT MIN(id) FROM users;\n.exit\n")
    if '3' not in stdout:
        print(f'FAIL: MIN(id) expected 3, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: MIN(id) returns smallest key.')

    # MAX(id) full table → 20
    _, stdout = run_commands(executable, db_file, "SELECT MAX(id) FROM users;\n.exit\n")
    if '20' not in stdout:
        print(f'FAIL: MAX(id) expected 20, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: MAX(id) returns largest key.')

    # MIN(id) with lower-bound WHERE id >= 8 → 10
    _, stdout = run_commands(executable, db_file,
                             "SELECT MIN(id) FROM users WHERE id >= 8;\n.exit\n")
    if '10' not in stdout:
        print(f'FAIL: MIN(id) WHERE id >= 8 expected 10, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: MIN(id) with WHERE id >= 8 returns 10.')

    # MAX(id) with upper-bound WHERE id <= 12 → 10
    _, stdout = run_commands(executable, db_file,
                             "SELECT MAX(id) FROM users WHERE id <= 12;\n.exit\n")
    if '10' not in stdout:
        print(f'FAIL: MAX(id) WHERE id <= 12 expected 10, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: MAX(id) with WHERE id <= 12 returns 10.')

    # COUNT(*) with range
    _, stdout = run_commands(executable, db_file,
                             "SELECT COUNT(*) FROM users WHERE id >= 7;\n.exit\n")
    if '4' not in stdout:
        print(f'FAIL: COUNT(*) WHERE id >= 7 expected 4, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: COUNT(*) with WHERE id >= 7 returns 4.')

    # MIN/MAX on empty table → NULL
    empty_db = os.path.join(os.path.dirname(__file__), 'test_agg_empty.db')
    remove_database(empty_db)
    _, stdout = run_commands(executable, empty_db, "SELECT MIN(id) FROM users;\n.exit\n")
    if 'NULL' not in stdout:
        print(f'FAIL: MIN(id) on empty table expected NULL, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: MIN(id) on empty table returns NULL.')

    _, stdout = run_commands(executable, empty_db, "SELECT MAX(id) FROM users;\n.exit\n")
    if 'NULL' not in stdout:
        print(f'FAIL: MAX(id) on empty table expected NULL, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: MAX(id) on empty table returns NULL.')
    remove_database(empty_db)

    # EXPLAIN shows correct aggregate labels
    _, stdout = run_commands(executable, db_file,
                             "EXPLAIN SELECT MIN(id) FROM users;\n.exit\n")
    if 'AGGREGATE: MIN(id)' not in stdout:
        print(f'FAIL: EXPLAIN MIN(id) missing aggregate label, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: EXPLAIN SELECT MIN(id) shows AGGREGATE: MIN(id).')

    _, stdout = run_commands(executable, db_file,
                             "EXPLAIN SELECT MAX(id) FROM users WHERE id > 5;\n.exit\n")
    if 'AGGREGATE: MAX(id)' not in stdout or 'RANGE SCAN' not in stdout:
        print(f'FAIL: EXPLAIN MAX(id)+RANGE SCAN missing labels, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: EXPLAIN SELECT MAX(id) WHERE id > 5 shows RANGE SCAN + AGGREGATE: MAX(id).')

    # Persist: MIN/MAX survive a DB reopen
    _, stdout = run_commands(executable, db_file, "SELECT MIN(id) FROM users;\n.exit\n")
    if '3' not in stdout:
        print(f'FAIL: MIN(id) after reopen expected 3, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: MIN(id) returns correct value after DB reopen.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
