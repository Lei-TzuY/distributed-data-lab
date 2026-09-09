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

    db_file = os.path.join(os.path.dirname(__file__), 'test_delete.db')
    remove_database(db_file)

    # Insert three rows then delete the middle one
    rc, stdout = run_commands(executable, db_file, (
        "INSERT INTO users VALUES (1, 'alice', 'alice@example.com');\n"
        "INSERT INTO users VALUES (2, 'bob', 'bob@example.com');\n"
        "INSERT INTO users VALUES (3, 'carol', 'carol@example.com');\n"
        "DELETE FROM users WHERE id = 2;\n"
        "SELECT * FROM users;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}')
        sys.exit(1)
    if '(2, bob, bob@example.com)' in stdout:
        print('FAIL: Deleted row still appears in SELECT.')
        sys.exit(1)
    if '(1, alice, alice@example.com)' not in stdout:
        print('FAIL: Row 1 missing after DELETE of row 2.')
        sys.exit(1)
    if '(3, carol, carol@example.com)' not in stdout:
        print('FAIL: Row 3 missing after DELETE of row 2.')
        sys.exit(1)
    print('PASS: DELETE removes the correct row and leaves others intact.')

    # Delete a key that does not exist
    rc, stdout = run_commands(executable, db_file, (
        "DELETE FROM users WHERE id = 99;\n"
        ".exit\n"
    ))
    if 'Error: Key not found.' not in stdout:
        print('FAIL: Expected "Key not found" for non-existent DELETE.')
        sys.exit(1)
    print('PASS: DELETE of non-existent key returns error.')

    # Delete then re-insert the same key
    rc, stdout = run_commands(executable, db_file, (
        "DELETE FROM users WHERE id = 1;\n"
        "INSERT INTO users VALUES (1, 'alice2', 'alice2@example.com');\n"
        "SELECT * FROM users WHERE id = 1;\n"
        ".exit\n"
    ))
    if '(1, alice2, alice2@example.com)' not in stdout:
        print('FAIL: Re-inserted row after DELETE not found.')
        sys.exit(1)
    print('PASS: Key can be re-inserted after DELETE.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
