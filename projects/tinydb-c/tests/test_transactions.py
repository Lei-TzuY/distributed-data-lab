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

    db_file = os.path.join(os.path.dirname(__file__), 'test_txn.db')
    remove_database(db_file)

    # COMMIT: data should persist
    rc, stdout = run_commands(executable, db_file, (
        "BEGIN;\n"
        "INSERT INTO users VALUES (1, 'alice', 'alice@example.com');\n"
        "INSERT INTO users VALUES (2, 'bob', 'bob@example.com');\n"
        "COMMIT;\n"
        "SELECT * FROM users;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}')
        sys.exit(1)
    if '(1, alice, alice@example.com)' not in stdout or '(2, bob, bob@example.com)' not in stdout:
        print('FAIL: Committed rows not visible after COMMIT.')
        sys.exit(1)
    print('PASS: BEGIN/COMMIT makes inserts visible.')

    # Verify data was written to disk (reload)
    rc2, stdout2 = run_commands(executable, db_file, "SELECT * FROM users;\n.exit\n")
    if '(1, alice, alice@example.com)' not in stdout2:
        print('FAIL: Committed data not persisted after DB reopen.')
        sys.exit(1)
    print('PASS: COMMIT flushes data to WAL so it survives a reopen.')

    # ROLLBACK: inserts inside a rolled-back transaction must not appear
    rc, stdout = run_commands(executable, db_file, (
        "BEGIN;\n"
        "INSERT INTO users VALUES (10, 'ghost', 'ghost@example.com');\n"
        "ROLLBACK;\n"
        "SELECT * FROM users;\n"
        ".exit\n"
    ))
    if '(10, ghost, ghost@example.com)' in stdout:
        print('FAIL: Rolled-back row is visible after ROLLBACK.')
        sys.exit(1)
    if '(1, alice, alice@example.com)' not in stdout:
        print('FAIL: Previously committed row disappeared after ROLLBACK.')
        sys.exit(1)
    print('PASS: ROLLBACK discards uncommitted inserts without touching committed data.')

    # ROLLBACK in the same process must preserve earlier autocommitted rows.
    remove_database(db_file)
    rc, stdout = run_commands(executable, db_file, (
        "INSERT INTO users VALUES (1, 'committed', 'committed@example.com');\n"
        "BEGIN;\n"
        "INSERT INTO users VALUES (2, 'ghost', 'ghost@example.com');\n"
        "ROLLBACK;\n"
        "SELECT * FROM users;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc} after same-process rollback.')
        sys.exit(1)
    if '(1, committed, committed@example.com)' not in stdout:
        print('FAIL: Autocommitted row disappeared after same-process ROLLBACK.')
        sys.exit(1)
    if '(2, ghost, ghost@example.com)' in stdout:
        print('FAIL: Rolled-back same-process row is visible.')
        sys.exit(1)
    print('PASS: ROLLBACK preserves earlier autocommitted rows in the same process.')

    # .exit inside an active transaction should roll back the uncommitted work.
    remove_database(db_file)
    rc, _ = run_commands(executable, db_file, (
        "BEGIN;\n"
        "INSERT INTO users VALUES (20, 'exit_ghost', 'exit_ghost@example.com');\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc} during transactional .exit.')
        sys.exit(1)
    rc, stdout = run_commands(executable, db_file, "SELECT * FROM users;\n.exit\n")
    if '(20, exit_ghost, exit_ghost@example.com)' in stdout:
        print('FAIL: .exit checkpointed an uncommitted transaction.')
        sys.exit(1)
    print('PASS: .exit rolls back an active transaction.')

    # Crash inside an active transaction should leave only committed data.
    remove_database(db_file)
    process = subprocess.Popen(
        [executable, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    process.stdin.write("INSERT INTO users VALUES (30, 'durable', 'durable@example.com');\n")
    process.stdin.write("BEGIN;\n")
    process.stdin.write("INSERT INTO users VALUES (31, 'crash_ghost', 'crash_ghost@example.com');\n")
    process.stdin.flush()
    time.sleep(0.5)
    process.kill()
    process.wait()
    rc, stdout = run_commands(executable, db_file, "SELECT * FROM users;\n.exit\n")
    if '(30, durable, durable@example.com)' not in stdout:
        print('FAIL: Committed row missing after crash during transaction.')
        sys.exit(1)
    if '(31, crash_ghost, crash_ghost@example.com)' in stdout:
        print('FAIL: Uncommitted row survived crash during transaction.')
        sys.exit(1)
    print('PASS: Crash during transaction keeps committed data and discards uncommitted data.')

    # Error: nested BEGIN
    rc, stdout = run_commands(executable, db_file, (
        "BEGIN;\n"
        "BEGIN;\n"
        "ROLLBACK;\n"
        ".exit\n"
    ))
    if 'Error: Transaction already active.' not in stdout:
        print('FAIL: Expected error for nested BEGIN.')
        sys.exit(1)
    print('PASS: Nested BEGIN is rejected with an error.')

    # Error: COMMIT without BEGIN
    rc, stdout = run_commands(executable, db_file, "COMMIT;\n.exit\n")
    if 'Error: No active transaction.' not in stdout:
        print('FAIL: Expected error for COMMIT outside transaction.')
        sys.exit(1)
    print('PASS: COMMIT outside transaction is rejected.')

    # Error: ROLLBACK without BEGIN
    rc, stdout = run_commands(executable, db_file, "ROLLBACK;\n.exit\n")
    if 'Error: No active transaction.' not in stdout:
        print('FAIL: Expected error for ROLLBACK outside transaction.')
        sys.exit(1)
    print('PASS: ROLLBACK outside transaction is rejected.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
