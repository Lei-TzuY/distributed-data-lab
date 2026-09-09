import os
import random
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

    db_file = os.path.join(os.path.dirname(__file__), 'test_random_split.db')
    remove_database(db_file)

    ids = list(range(1, 71))
    random.Random(42).shuffle(ids)
    commands = ''.join(
        f"INSERT INTO users VALUES ({row_id}, 'user{row_id}', 'person{row_id}@example.com');\n"
        for row_id in ids
    )
    commands += ''.join(
        f'SELECT * FROM users WHERE id = {row_id};\n'
        for row_id in range(1, 71)
    )
    commands += 'SELECT * FROM users;\n.exit\n'

    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, stderr = process.communicate(input=commands)

    if process.returncode != 0:
        print(f'FAIL: tinydb exited with {process.returncode}: {stderr}')
        sys.exit(1)

    for row_id in range(1, 71):
        row = f'({row_id}, user{row_id}, person{row_id}@example.com)'
        if stdout.count(row) != 2:
            print(f'FAIL: Expected point lookup and scan results for row {row_id}.')
            sys.exit(1)

    positions = [
        stdout.rfind(f'({row_id}, user{row_id}, person{row_id}@example.com)')
        for row_id in range(1, 71)
    ]
    if positions != sorted(positions):
        print('FAIL: Full scan is not sorted after randomized leaf splits.')
        sys.exit(1)

    remove_database(db_file)
    print('PASS: Randomized leaf splits preserve lookup routing and scan order.')


if __name__ == '__main__':
    run_test()
