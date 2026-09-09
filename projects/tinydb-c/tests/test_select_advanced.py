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
    proc = subprocess.Popen(
        [executable, db_file],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
    )
    stdout, _ = proc.communicate(input=commands)
    return proc.returncode, stdout


def rows_in_output(stdout, ids):
    """Return the set of ids that appear in stdout."""
    found = set()
    for i in ids:
        if f'({i}, user{i},' in stdout:
            found.add(i)
    return found


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    exe = find_executable(base_dir)
    if not exe:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_adv.db')
    remove_database(db_file)

    # Seed the database with rows 1-10
    inserts = ''.join(
        f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@x.com');\n"
        for i in range(1, 11)
    )

    # ── COUNT(*) ──────────────────────────────────────────────────
    rc, out = run_commands(exe, db_file, inserts + "SELECT COUNT(*) FROM users;\n.exit\n")
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}')
        sys.exit(1)
    if '10\n' not in out:
        print(f'FAIL: COUNT(*) expected 10, got:\n{out}')
        sys.exit(1)
    print('PASS: SELECT COUNT(*) FROM users -> 10')

    rc, out = run_commands(exe, db_file,
        "SELECT COUNT(*) FROM users WHERE id = 5;\n.exit\n")
    if '1\n' not in out:
        print(f'FAIL: COUNT(*) WHERE id=5 expected 1, got:\n{out}')
        sys.exit(1)
    print('PASS: SELECT COUNT(*) WHERE id = 5 -> 1')

    rc, out = run_commands(exe, db_file,
        "SELECT COUNT(*) FROM users WHERE id > 7;\n.exit\n")
    if '3\n' not in out:
        print(f'FAIL: COUNT(*) WHERE id>7 expected 3, got:\n{out}')
        sys.exit(1)
    print('PASS: SELECT COUNT(*) WHERE id > 7 -> 3')

    rc, out = run_commands(exe, db_file,
        "SELECT COUNT(*) FROM users WHERE id <= 4;\n.exit\n")
    if '4\n' not in out:
        print(f'FAIL: COUNT(*) WHERE id<=4 expected 4, got:\n{out}')
        sys.exit(1)
    print('PASS: SELECT COUNT(*) WHERE id <= 4 -> 4')

    # ── LIMIT ─────────────────────────────────────────────────────
    rc, out = run_commands(exe, db_file,
        "SELECT * FROM users LIMIT 3;\n.exit\n")
    found = rows_in_output(out, range(1, 11))
    if found != {1, 2, 3}:
        print(f'FAIL: LIMIT 3 returned {sorted(found)}, expected [1,2,3]')
        sys.exit(1)
    print('PASS: SELECT * LIMIT 3 -> rows 1, 2, 3')

    rc, out = run_commands(exe, db_file,
        "SELECT * FROM users LIMIT 0;\n.exit\n")
    found = rows_in_output(out, range(1, 11))
    if found:
        print(f'FAIL: LIMIT 0 should return no rows, got {sorted(found)}')
        sys.exit(1)
    print('PASS: SELECT * LIMIT 0 -> no rows')

    # ── Range scans ───────────────────────────────────────────────
    rc, out = run_commands(exe, db_file,
        "SELECT * FROM users WHERE id > 7;\n.exit\n")
    found = rows_in_output(out, range(1, 11))
    if found != {8, 9, 10}:
        print(f'FAIL: WHERE id>7 returned {sorted(found)}, expected [8,9,10]')
        sys.exit(1)
    print('PASS: SELECT * WHERE id > 7 -> rows 8, 9, 10')

    rc, out = run_commands(exe, db_file,
        "SELECT * FROM users WHERE id >= 8;\n.exit\n")
    found = rows_in_output(out, range(1, 11))
    if found != {8, 9, 10}:
        print(f'FAIL: WHERE id>=8 returned {sorted(found)}, expected [8,9,10]')
        sys.exit(1)
    print('PASS: SELECT * WHERE id >= 8 -> rows 8, 9, 10')

    rc, out = run_commands(exe, db_file,
        "SELECT * FROM users WHERE id < 4;\n.exit\n")
    found = rows_in_output(out, range(1, 11))
    if found != {1, 2, 3}:
        print(f'FAIL: WHERE id<4 returned {sorted(found)}, expected [1,2,3]')
        sys.exit(1)
    print('PASS: SELECT * WHERE id < 4 -> rows 1, 2, 3')

    rc, out = run_commands(exe, db_file,
        "SELECT * FROM users WHERE id <= 3;\n.exit\n")
    found = rows_in_output(out, range(1, 11))
    if found != {1, 2, 3}:
        print(f'FAIL: WHERE id<=3 returned {sorted(found)}, expected [1,2,3]')
        sys.exit(1)
    print('PASS: SELECT * WHERE id <= 3 -> rows 1, 2, 3')

    # ── Range + LIMIT combined ────────────────────────────────────
    rc, out = run_commands(exe, db_file,
        "SELECT * FROM users WHERE id >= 5 LIMIT 3;\n.exit\n")
    found = rows_in_output(out, range(1, 11))
    if found != {5, 6, 7}:
        print(f'FAIL: WHERE id>=5 LIMIT 3 returned {sorted(found)}, expected [5,6,7]')
        sys.exit(1)
    print('PASS: SELECT * WHERE id >= 5 LIMIT 3 -> rows 5, 6, 7')

    # ── EXPLAIN for range scan ────────────────────────────────────
    rc, out = run_commands(exe, db_file,
        "EXPLAIN SELECT * FROM users WHERE id > 5;\n.exit\n")
    if 'PLAN: PRIMARY KEY RANGE SCAN (id > 5)' not in out:
        print(f'FAIL: EXPLAIN range scan output wrong:\n{out}')
        sys.exit(1)
    print('PASS: EXPLAIN SELECT WHERE id > 5 -> PRIMARY KEY RANGE SCAN')

    rc, out = run_commands(exe, db_file,
        "EXPLAIN SELECT COUNT(*) FROM users LIMIT 5;\n.exit\n")
    if 'AGGREGATE: COUNT(*)' not in out or 'LIMIT 5' not in out:
        print(f'FAIL: EXPLAIN COUNT/LIMIT output wrong:\n{out}')
        sys.exit(1)
    print('PASS: EXPLAIN COUNT(*) LIMIT shows AGGREGATE and LIMIT modifiers')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
