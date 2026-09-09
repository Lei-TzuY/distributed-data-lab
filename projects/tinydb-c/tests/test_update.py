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
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
    )
    stdout, _ = process.communicate(input=commands)
    return process.returncode, stdout


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    executable = find_executable(base_dir)
    if not executable:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_update.db')
    remove_database(db_file)

    # Basic UPDATE of a single field
    rc, stdout = run_commands(executable, db_file, (
        "INSERT INTO users VALUES (1, 'alice', 'alice@example.com');\n"
        "INSERT INTO users VALUES (2, 'bob',   'bob@example.com');\n"
        "UPDATE users SET username = 'alicia' WHERE id = 1;\n"
        "SELECT * FROM users WHERE id = 1;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}')
        sys.exit(1)
    if '(1, alicia, alice@example.com)' not in stdout:
        print('FAIL: username not updated.')
        sys.exit(1)
    if '(1, alice, alice@example.com)' in stdout:
        print('FAIL: old username still present after UPDATE.')
        sys.exit(1)
    print('PASS: UPDATE changes username correctly.')

    # UPDATE email only
    rc, stdout = run_commands(executable, db_file, (
        "UPDATE users SET email = 'new@example.com' WHERE id = 2;\n"
        "SELECT * FROM users WHERE id = 2;\n"
        ".exit\n"
    ))
    if '(2, bob, new@example.com)' not in stdout:
        print('FAIL: email not updated.')
        sys.exit(1)
    print('PASS: UPDATE changes email correctly.')

    # UPDATE both fields
    rc, stdout = run_commands(executable, db_file, (
        "UPDATE users SET username = 'bobby', email = 'bobby@example.com' WHERE id = 2;\n"
        "SELECT * FROM users WHERE id = 2;\n"
        ".exit\n"
    ))
    if '(2, bobby, bobby@example.com)' not in stdout:
        print('FAIL: both fields not updated.')
        sys.exit(1)
    print('PASS: UPDATE changes both fields in one statement.')

    # UPDATE non-existent key
    rc, stdout = run_commands(executable, db_file, (
        "UPDATE users SET username = 'nobody' WHERE id = 99;\n"
        ".exit\n"
    ))
    if 'Error: Key not found.' not in stdout:
        print('FAIL: Expected "Key not found" for UPDATE on missing id.')
        sys.exit(1)
    print('PASS: UPDATE on non-existent key returns error.')

    # UPDATE persists across reopens
    rc, stdout = run_commands(executable, db_file, (
        "SELECT * FROM users WHERE id = 1;\n"
        ".exit\n"
    ))
    if '(1, alicia, alice@example.com)' not in stdout:
        print('FAIL: UPDATE did not persist after DB reopen.')
        sys.exit(1)
    print('PASS: UPDATE persists to disk correctly.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
