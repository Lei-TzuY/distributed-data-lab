import os
import subprocess
import sys
import random


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

    db_file = os.path.join(os.path.dirname(__file__), 'test_merge.db')
    remove_database(db_file)

    # ── Test 1: delete all rows one by one; SELECT must always be correct ──
    n = 20
    inserts = ''.join(
        f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@example.com');\n"
        for i in range(1, n + 1)
    )
    # Delete in reverse order (empties right leaves first)
    deletes = ''.join(
        f"DELETE FROM users WHERE id = {i};\n"
        for i in range(n, 0, -1)
    )
    rc, stdout = run_commands(executable, db_file, inserts + deletes + "SELECT * FROM users;\n.exit\n")
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}')
        sys.exit(1)
    for i in range(1, n + 1):
        row = f'(user{i}, u{i}@example.com)'
        if row in stdout:
            print(f'FAIL: row {i} still visible after being deleted.')
            sys.exit(1)
    print('PASS: All rows deleted one by one; SELECT returns empty.')

    remove_database(db_file)

    # ── Test 2: insert 20, delete all but a few, scan is still correct ──
    ids = list(range(1, n + 1))
    random.Random(7).shuffle(ids)
    inserts2 = ''.join(
        f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@example.com');\n"
        for i in ids
    )
    keep = {3, 7, 15}
    deletes2 = ''.join(
        f"DELETE FROM users WHERE id = {i};\n"
        for i in range(1, n + 1) if i not in keep
    )
    rc, stdout = run_commands(
        executable, db_file,
        inserts2 + deletes2 + "SELECT * FROM users;\n.exit\n"
    )
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}')
        sys.exit(1)
    for i in keep:
        if f'({i}, user{i}, u{i}@example.com)' not in stdout:
            print(f'FAIL: row {i} is missing after selective deletes.')
            sys.exit(1)
    for i in range(1, n + 1):
        if i not in keep and f'({i}, user{i}, u{i}@example.com)' in stdout:
            print(f'FAIL: deleted row {i} still appears.')
            sys.exit(1)
    # Verify scan order for remaining rows
    positions = [stdout.find(f'({i}, user{i}, u{i}@example.com)') for i in sorted(keep)]
    if positions != sorted(positions):
        print('FAIL: Remaining rows are not in sorted order after leaf merges.')
        sys.exit(1)
    print('PASS: Selective deletes leave only the correct rows in sorted order.')

    remove_database(db_file)

    # ── Test 3: tree shrinks back to a single leaf after enough deletes ──
    inserts3 = ''.join(
        f"INSERT INTO users VALUES ({i}, 'u{i}', 'u{i}@x.com');\n"
        for i in range(1, 16)
    )
    # After 15 inserts the tree must have internal nodes
    rc, stdout_before = run_commands(executable, db_file, inserts3 + ".btree\n.exit\n")
    if 'internal' not in stdout_before:
        print('FAIL: Expected internal nodes after 15 inserts.')
        sys.exit(1)

    # Delete all but id=1; the tree should collapse back to a single leaf
    deletes3 = ''.join(
        f"DELETE FROM users WHERE id = {i};\n"
        for i in range(2, 16)
    )
    rc, stdout_after = run_commands(executable, db_file, deletes3 + ".btree\n.exit\n")
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}')
        sys.exit(1)
    if 'internal' in stdout_after:
        print('FAIL: Internal nodes still present after tree should have collapsed.')
        sys.exit(1)
    if '(u1, u1@x.com)' not in stdout_after and '(1, u1, u1@x.com)' not in stdout_after:
        # check via select too
        rc2, sel = run_commands(executable, db_file, "SELECT * FROM users;\n.exit\n")
        if '(1, u1, u1@x.com)' not in sel:
            print('FAIL: Sole remaining row (id=1) not found after collapse.')
            sys.exit(1)
    print('PASS: B+ tree collapses back to a single leaf after mass deletes.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
