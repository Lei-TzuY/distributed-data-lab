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

    db_file = os.path.join(os.path.dirname(__file__), 'test_del_all.db')
    remove_database(db_file)

    # Insert rows, truncate, verify empty
    rc, stdout = run_commands(executable, db_file, (
        "INSERT INTO users VALUES (1, 'alice', 'a@x.com');\n"
        "INSERT INTO users VALUES (2, 'bob',   'b@x.com');\n"
        "INSERT INTO users VALUES (3, 'carol', 'c@x.com');\n"
        "DELETE FROM users;\n"
        "SELECT * FROM users;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}')
        sys.exit(1)
    for name in ('alice', 'bob', 'carol'):
        if name in stdout.split("DELETE FROM users;\n", 1)[-1]:
            print(f'FAIL: Row for {name} still visible after DELETE FROM users.')
            sys.exit(1)
    print('PASS: DELETE FROM users (without WHERE) truncates the table.')

    # After truncate, inserts should work normally
    rc, stdout = run_commands(executable, db_file, (
        "INSERT INTO users VALUES (10, 'dave', 'd@x.com');\n"
        "SELECT * FROM users;\n"
        ".exit\n"
    ))
    if '(10, dave, d@x.com)' not in stdout:
        print('FAIL: INSERT after truncate did not work.')
        sys.exit(1)
    print('PASS: INSERT works normally after truncate.')

    # Truncate via transaction then rollback → data must not be erased
    rc, stdout = run_commands(executable, db_file, (
        "BEGIN;\n"
        "DELETE FROM users;\n"
        "ROLLBACK;\n"
        "SELECT * FROM users;\n"
        ".exit\n"
    ))
    if '(10, dave, d@x.com)' not in stdout:
        print('FAIL: Data erased by rolled-back truncate.')
        sys.exit(1)
    print('PASS: Rolled-back truncate does not erase committed data.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
