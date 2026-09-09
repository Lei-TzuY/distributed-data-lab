import os
import subprocess
import sys
import time


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

    db_file = os.path.join(os.path.dirname(__file__), 'test_wal_atomicity.db')
    remove_database(db_file)

    try:
        rc, _ = run_commands(executable, db_file, (
            "INSERT INTO users VALUES (1, 'stable', 'stable@example.com');\n"
            ".exit\n"
        ))
        if rc != 0:
            print(f'FAIL: initial insert exited with {rc}')
            sys.exit(1)

        process = subprocess.Popen(
            [executable, db_file],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        process.stdin.write("INSERT INTO users VALUES (2, 'partial', 'partial@example.com');\n")
        process.stdin.flush()
        time.sleep(0.5)
        process.kill()
        process.wait()

        wal_file = db_file + '.wal'
        if not os.path.exists(wal_file):
            print('FAIL: WAL file was not created for atomicity test.')
            sys.exit(1)

        wal_size = os.path.getsize(wal_file)
        if wal_size <= 4:
            print('FAIL: WAL file is too small to truncate commit marker.')
            sys.exit(1)
        with open(wal_file, 'r+b') as wal:
            wal.truncate(wal_size - 4)

        rc, stdout = run_commands(executable, db_file, "SELECT * FROM users;\n.exit\n")
        if rc != 0:
            print(f'FAIL: recovery process exited with {rc}')
            sys.exit(1)
        if 'Ignoring incomplete WAL transaction.' not in stdout:
            print('FAIL: incomplete WAL transaction was not detected.')
            sys.exit(1)
        if '(1, stable, stable@example.com)' not in stdout:
            print('FAIL: stable committed row is missing after incomplete WAL recovery.')
            sys.exit(1)
        if '(2, partial, partial@example.com)' in stdout:
            print('FAIL: incomplete WAL transaction was applied.')
            sys.exit(1)

        print('PASS: Incomplete WAL transactions are ignored atomically.')
    finally:
        remove_database(db_file)


if __name__ == '__main__':
    run_test()
