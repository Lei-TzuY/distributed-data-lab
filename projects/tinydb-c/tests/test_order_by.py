"""
test_order_by.py — verify ORDER BY id DESC reverse cursor.

Leaf layout has a prev_leaf pointer so cursor_retreat() can walk the
doubly-linked leaf chain right-to-left in O(1) per step.
"""

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


def rows_in_order(stdout):
    """Extract (id,) tuples from lines like '(3, user3, u3@example.com)'.

    The REPL prompt 'db > ' may appear on the same line as the first output
    row when running non-interactively, so we strip it before matching.
    """
    ids = []
    for line in stdout.splitlines():
        line = line.strip()
        if line.startswith('db > '):
            line = line[5:]
        if line.startswith('(') and line.endswith(')'):
            try:
                ids.append(int(line[1:line.index(',')]))
            except (ValueError, IndexError):
                pass
    return ids


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    executable = find_executable(base_dir)
    if not executable:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_ord.db')
    remove_database(db_file)

    # Seed rows 1-14 (spans two leaf pages after a split)
    inserts = ''.join(
        f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@example.com');\n"
        for i in range(1, 15)
    )
    rc, _ = run_commands(executable, db_file, inserts + '.exit\n')
    if rc != 0:
        print(f'FAIL: seed inserts exited with {rc}')
        sys.exit(1)

    # ── Test 1: full reverse scan ──────────────────────────────────────────
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users ORDER BY id DESC;\n.exit\n')
    if rc != 0:
        print(f'FAIL: full DESC exited with {rc}')
        sys.exit(1)
    ids = rows_in_order(stdout)
    expected = list(range(14, 0, -1))   # [14, 13, ..., 1]
    if ids != expected:
        print(f'FAIL: full DESC order wrong.\n  got:      {ids}\n  expected: {expected}')
        sys.exit(1)
    print('PASS: SELECT * ORDER BY id DESC returns rows 14..1.')

    # ── Test 2: reverse scan with lower bound (WHERE id > N) ──────────────
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users WHERE id > 5 ORDER BY id DESC;\n.exit\n')
    ids = rows_in_order(stdout)
    expected = list(range(14, 5, -1))   # [14, 13, ..., 6]
    if ids != expected:
        print(f'FAIL: DESC WHERE id>5 wrong.\n  got: {ids}\n  expected: {expected}')
        sys.exit(1)
    print('PASS: SELECT * WHERE id > 5 ORDER BY id DESC returns 14..6.')

    # ── Test 3: reverse scan with upper bound (WHERE id < N) ──────────────
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users WHERE id < 8 ORDER BY id DESC;\n.exit\n')
    ids = rows_in_order(stdout)
    expected = list(range(7, 0, -1))    # [7, 6, ..., 1]
    if ids != expected:
        print(f'FAIL: DESC WHERE id<8 wrong.\n  got: {ids}\n  expected: {expected}')
        sys.exit(1)
    print('PASS: SELECT * WHERE id < 8 ORDER BY id DESC returns 7..1.')

    # ── Test 4: DESC + LIMIT (top-N largest) ──────────────────────────────
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users ORDER BY id DESC LIMIT 3;\n.exit\n')
    ids = rows_in_order(stdout)
    if ids != [14, 13, 12]:
        print(f'FAIL: DESC LIMIT 3 wrong. got: {ids}')
        sys.exit(1)
    print('PASS: SELECT * ORDER BY id DESC LIMIT 3 returns [14, 13, 12].')

    # ── Test 5: WHERE id >= N ORDER BY id DESC ────────────────────────────
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users WHERE id >= 10 ORDER BY id DESC;\n.exit\n')
    ids = rows_in_order(stdout)
    expected = list(range(14, 9, -1))   # [14, 13, ..., 10]
    if ids != expected:
        print(f'FAIL: DESC WHERE id>=10 wrong. got: {ids}')
        sys.exit(1)
    print('PASS: SELECT * WHERE id >= 10 ORDER BY id DESC returns 14..10.')

    # ── Test 6: WHERE id <= N ORDER BY id DESC ────────────────────────────
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users WHERE id <= 5 ORDER BY id DESC;\n.exit\n')
    ids = rows_in_order(stdout)
    expected = list(range(5, 0, -1))    # [5, 4, ..., 1]
    if ids != expected:
        print(f'FAIL: DESC WHERE id<=5 wrong. got: {ids}')
        sys.exit(1)
    print('PASS: SELECT * WHERE id <= 5 ORDER BY id DESC returns 5..1.')

    # ── Test 7: WHERE id >= A AND use both bounds in DESC ─────────────────
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users WHERE id >= 4 ORDER BY id DESC LIMIT 4;\n.exit\n')
    ids = rows_in_order(stdout)
    if ids != [14, 13, 12, 11]:
        print(f'FAIL: DESC WHERE id>=4 LIMIT 4 wrong. got: {ids}')
        sys.exit(1)
    print('PASS: SELECT * WHERE id >= 4 ORDER BY id DESC LIMIT 4 returns [14,13,12,11].')

    # ── Test 8: EXPLAIN shows ORDER BY id DESC ────────────────────────────
    rc, stdout = run_commands(executable, db_file,
                              'EXPLAIN SELECT * FROM users ORDER BY id DESC LIMIT 5;\n.exit\n')
    if 'ORDER BY id DESC' not in stdout:
        print(f'FAIL: EXPLAIN missing ORDER BY id DESC. got:\n{stdout}')
        sys.exit(1)
    if 'LIMIT 5' not in stdout:
        print(f'FAIL: EXPLAIN missing LIMIT 5. got:\n{stdout}')
        sys.exit(1)
    print('PASS: EXPLAIN SELECT * ORDER BY id DESC LIMIT 5 shows both labels.')

    # ── Test 9: ORDER BY DESC on empty table returns no rows ──────────────
    empty_db = os.path.join(os.path.dirname(__file__), 'test_ord_empty.db')
    remove_database(empty_db)
    rc, stdout = run_commands(executable, empty_db,
                              'SELECT * FROM users ORDER BY id DESC;\n.exit\n')
    if rc != 0:
        print(f'FAIL: DESC on empty table exited with {rc}')
        sys.exit(1)
    if rows_in_order(stdout):
        print(f'FAIL: DESC on empty table returned rows: {stdout}')
        sys.exit(1)
    print('PASS: SELECT * ORDER BY id DESC on empty table returns no rows.')
    remove_database(empty_db)

    # ── Test 10: persist — DESC still works after DB reopen ───────────────
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users ORDER BY id DESC LIMIT 2;\n.exit\n')
    ids = rows_in_order(stdout)
    if ids != [14, 13]:
        print(f'FAIL: DESC after reopen wrong. got: {ids}')
        sys.exit(1)
    print('PASS: ORDER BY id DESC works correctly after DB reopen.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
