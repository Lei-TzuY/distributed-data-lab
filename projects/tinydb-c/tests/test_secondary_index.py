import os
import struct
import subprocess
import sys

INDEX_CATALOG_MAGIC = 0x58444954
INDEX_CATALOG_VERSION = 2
INDEX_CATALOG_WAL_COMMIT_MAGIC = 0x49445843
USERNAME_INDEX_MAGIC = 0x55494458
USERNAME_INDEX_VERSION = 1
USERNAME_INDEX_WAL_COMMIT_MAGIC = 0x55494443


def find_executable(base_dir):
    possible_paths = [
        os.path.join(base_dir, 'build', 'Debug', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb'),
    ]
    return next((path for path in possible_paths if os.path.exists(path)), None)


def remove_database(db_file):
    for path in (
        db_file,
        db_file + '.wal',
        db_file + '.catalog',
        db_file + '.catalog.wal',
        db_file + '.username.idx',
        db_file + '.username.idx.wal',
    ):
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


def write_catalog_wal(db_file, include_commit):
    with open(db_file + '.catalog.wal', 'wb') as wal:
        header = struct.pack(
            '<IIIII',
            INDEX_CATALOG_MAGIC,
            INDEX_CATALOG_VERSION,
            1,
            0,
            0,
        )
        wal.write(header.ljust(1108, b'\x00'))
        if include_commit:
            wal.write(struct.pack('<I', INDEX_CATALOG_WAL_COMMIT_MAGIC))


def write_username_index_wal(db_file, entries, include_commit):
    with open(db_file + '.username.idx.wal', 'wb') as wal:
        wal.write(struct.pack('<III', USERNAME_INDEX_MAGIC, USERNAME_INDEX_VERSION, len(entries)))
        for username, row_id in entries:
            encoded = username.encode('ascii')
            if len(encoded) > 32:
                raise ValueError('username too long for test index record')
            wal.write(encoded + b'\0' * (33 - len(encoded)))
            wal.write(struct.pack('<I', row_id))
        if include_commit:
            wal.write(struct.pack('<I', USERNAME_INDEX_WAL_COMMIT_MAGIC))


def rows_in_order(stdout):
    ids = []
    for line in stdout.splitlines():
        line = line.strip()
        if line.startswith('db > '):
            line = line[5:]
        if line.startswith('(') and line.endswith(')'):
            ids.append(int(line[1:line.index(',')]))
    return ids


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    executable = find_executable(base_dir)
    if not executable:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_secondary.db')
    remove_database(db_file)

    seed = (
        "INSERT INTO users VALUES (1, 'alice', 'a1@example.com');\n"
        "INSERT INTO users VALUES (2, 'bob', 'b@example.com');\n"
        "INSERT INTO users VALUES (3, 'alice', 'a3@example.com');\n"
        "INSERT INTO users VALUES (4, 'carol', 'c@example.com');\n"
    )

    rc, stdout = run_commands(executable, db_file, (
        seed +
        "EXPLAIN SELECT * FROM users WHERE username = 'alice';\n"
        "SELECT * FROM users WHERE username = 'alice';\n"
        "CREATE INDEX idx_users_username ON users(username);\n"
        ".schema\n"
        "EXPLAIN SELECT * FROM users WHERE username = 'alice';\n"
        "SELECT * FROM users WHERE username = 'alice';\n"
        "SELECT COUNT(*) FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}')
        sys.exit(1)
    if "PLAN: FULL TABLE SCAN (username = 'alice')" not in stdout:
        print('FAIL: EXPLAIN before CREATE INDEX should show full scan.')
        sys.exit(1)
    if "PLAN: SECONDARY INDEX LOOKUP (username = 'alice')" not in stdout:
        print('FAIL: EXPLAIN after CREATE INDEX should show secondary index lookup.')
        sys.exit(1)
    if 'Index: idx_users_username ON users(username)' not in stdout:
        print('FAIL: .schema did not report the active username index.')
        sys.exit(1)
    if not os.path.exists(db_file + '.username.idx') or os.path.exists(db_file + '.username.idx.wal'):
        print('FAIL: CREATE INDEX did not persist clean username index entries.')
        sys.exit(1)
    if stdout.count('(1, alice, a1@example.com)') < 2:
        print('FAIL: alice row 1 missing from full scan/index lookup.')
        sys.exit(1)
    if stdout.count('(3, alice, a3@example.com)') < 2:
        print('FAIL: alice row 3 missing from full scan/index lookup.')
        sys.exit(1)
    if '2\n' not in stdout:
        print('FAIL: COUNT(*) over username index expected 2.')
        sys.exit(1)

    rc, desc_stdout = run_commands(executable, db_file, (
        "EXPLAIN SELECT * FROM users WHERE username = 'alice' ORDER BY id DESC;\n"
        "SELECT * FROM users WHERE username = 'alice' ORDER BY id DESC;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: DESC username query exited with {rc}')
        sys.exit(1)
    if "PLAN: SECONDARY INDEX LOOKUP (username = 'alice')" not in desc_stdout:
        print('FAIL: persisted username index should be used after reopen.')
        sys.exit(1)
    desc_ids = rows_in_order(desc_stdout)
    if desc_ids != [3, 1]:
        print(f'FAIL: username secondary index DESC order wrong: {desc_ids}')
        sys.exit(1)
    print('PASS: CREATE INDEX enables username lookup, count, schema, and DESC scans.')

    rc, stdout = run_commands(executable, db_file, (
        "INSERT INTO users VALUES (5, 'dave', 'd@example.com');\n"
        "SELECT * FROM users WHERE username = 'dave';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: indexed autocommit insert exited with {rc}')
        sys.exit(1)
    if rows_in_order(stdout) != [5]:
        print(f'FAIL: username index did not include autocommitted insert: {rows_in_order(stdout)}')
        sys.exit(1)
    rc, stdout = run_commands(executable, db_file, (
        "SELECT * FROM users WHERE username = 'dave';\n"
        ".exit\n"
    ))
    if rows_in_order(stdout) != [5]:
        print(f'FAIL: persisted username index missed autocommitted insert after reopen: {rows_in_order(stdout)}')
        sys.exit(1)
    print('PASS: persistent username index stays current after autocommit INSERT.')

    rc, stdout = run_commands(executable, db_file, (
        "BEGIN;\n"
        "INSERT INTO users VALUES (6, 'erin', 'e@example.com');\n"
        "COMMIT;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: indexed transaction commit exited with {rc}')
        sys.exit(1)
    rc, stdout = run_commands(executable, db_file, (
        "SELECT * FROM users WHERE username = 'erin';\n"
        ".exit\n"
    ))
    if rows_in_order(stdout) != [6]:
        print(f'FAIL: persisted username index missed transaction commit after reopen: {rows_in_order(stdout)}')
        sys.exit(1)
    print('PASS: persistent username index stays current after transaction COMMIT.')

    rc, stdout = run_commands(executable, db_file, (
        "DROP INDEX idx_users_username;\n"
        ".schema\n"
        "EXPLAIN SELECT * FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: DROP INDEX exited with {rc}')
        sys.exit(1)
    if 'Index: idx_users_username ON users(username)' in stdout:
        print('FAIL: .schema still reported dropped username index.')
        sys.exit(1)
    if "PLAN: FULL TABLE SCAN (username = 'alice')" not in stdout:
        print('FAIL: DROP INDEX should switch username queries back to full scan.')
        sys.exit(1)
    if os.path.exists(db_file + '.username.idx') or os.path.exists(db_file + '.username.idx.wal'):
        print('FAIL: DROP INDEX did not remove username index sidecar files.')
        sys.exit(1)
    rc, stdout = run_commands(executable, db_file, (
        ".schema\n"
        "EXPLAIN SELECT * FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if 'Index: idx_users_username ON users(username)' in stdout:
        print('FAIL: dropped username index returned after reopen.')
        sys.exit(1)
    if "PLAN: FULL TABLE SCAN (username = 'alice')" not in stdout:
        print('FAIL: reopened DB should keep dropped username index disabled.')
        sys.exit(1)
    print('PASS: DROP INDEX removes schema entry, sidecar files, and persists after reopen.')

    rc, stdout = run_commands(executable, db_file, (
        "CREATE INDEX idx_wrong ON users(username);\n"
        "DROP INDEX idx_wrong;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: generic index DDL syntax test exited with {rc}')
        sys.exit(1)
    print('PASS: generic index names are supported by the compiler.')

    rc, stdout = run_commands(executable, db_file, (
        "BEGIN;\n"
        "CREATE INDEX idx_users_username ON users(username);\n"
        "ROLLBACK;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: CREATE INDEX inside transaction test exited with {rc}')
        sys.exit(1)
    if 'Error: Index DDL is not allowed inside a transaction.' not in stdout:
        print('FAIL: CREATE INDEX inside transaction should be rejected.')
        sys.exit(1)
    rc, stdout = run_commands(executable, db_file, (
        "CREATE INDEX idx_users_username ON users(username);\n"
        "BEGIN;\n"
        "DROP INDEX idx_users_username;\n"
        "ROLLBACK;\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: DROP INDEX inside transaction test exited with {rc}')
        sys.exit(1)
    if 'Error: Index DDL is not allowed inside a transaction.' not in stdout:
        print('FAIL: DROP INDEX inside transaction should be rejected.')
        sys.exit(1)
    print('PASS: index DDL is rejected inside active transactions.')

    # Index is lazy-maintained after UPDATE and DELETE.
    rc, stdout = run_commands(executable, db_file, (
        "CREATE INDEX idx_users_username ON users(username);\n"
        "UPDATE users SET username = 'alice' WHERE id = 2;\n"
        "DELETE FROM users WHERE id = 1;\n"
        "SELECT * FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: update/delete index maintenance exited with {rc}')
        sys.exit(1)
    found = rows_in_order(stdout)
    if found != [2, 3]:
        print(f'FAIL: expected alice rows [2, 3] after update/delete, got {found}')
        sys.exit(1)
    print('PASS: username index rebuilds after UPDATE and DELETE.')

    # ROLLBACK marks the index dirty and rebuilds it against restored pages.
    rc, stdout = run_commands(executable, db_file, (
        "CREATE INDEX idx_users_username ON users(username);\n"
        "BEGIN;\n"
        "UPDATE users SET username = 'ghost' WHERE id = 2;\n"
        "ROLLBACK;\n"
        "SELECT * FROM users WHERE username = 'alice';\n"
        "SELECT * FROM users WHERE username = 'ghost';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: rollback index maintenance exited with {rc}')
        sys.exit(1)
    if '(2, alice, b@example.com)' not in stdout or '(3, alice, a3@example.com)' not in stdout:
        print('FAIL: rollback did not restore indexed alice rows.')
        sys.exit(1)
    if '(2, ghost, b@example.com)' in stdout:
        print('FAIL: rollback left a stale username index entry.')
        sys.exit(1)
    print('PASS: username index rebuilds correctly after ROLLBACK.')

    # Index metadata persists across reopen; entries are rebuilt lazily from table rows.
    rc, stdout = run_commands(executable, db_file, (
        ".schema\n"
        "EXPLAIN SELECT * FROM users WHERE username = 'alice';\n"
        "SELECT * FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if 'Index: idx_users_username ON users(username)' not in stdout:
        print('FAIL: persisted username index missing from reopened schema.')
        sys.exit(1)
    if "PLAN: SECONDARY INDEX LOOKUP (username = 'alice')" not in stdout:
        print('FAIL: reopened DB should use persisted username index metadata.')
        sys.exit(1)
    if '(2, alice, b@example.com)' not in stdout or '(3, alice, a3@example.com)' not in stdout:
        print('FAIL: lazily rebuilt username index after reopen returned wrong rows.')
        sys.exit(1)
    print('PASS: username index metadata persists and rebuilds after DB reopen.')

    os.remove(db_file + '.username.idx')
    rc, stdout = run_commands(executable, db_file, (
        "SELECT * FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: missing username index file fallback exited with {rc}')
        sys.exit(1)
    if '(2, alice, b@example.com)' not in stdout or '(3, alice, a3@example.com)' not in stdout:
        print('FAIL: missing username index file did not fall back to table rebuild.')
        sys.exit(1)
    if not os.path.exists(db_file + '.username.idx'):
        print('FAIL: missing username index file was not recreated after rebuild.')
        sys.exit(1)
    print('PASS: missing username index file falls back to rebuild and is recreated.')

    full_index_entries = [
        ('alice', 2),
        ('alice', 3),
        ('carol', 4),
        ('dave', 5),
        ('erin', 6),
    ]

    write_username_index_wal(db_file, full_index_entries, include_commit=True)
    rc, stdout = run_commands(executable, db_file, (
        "SELECT * FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: username index WAL recovery exited with {rc}')
        sys.exit(1)
    if 'Username index WAL found. Recovering...' not in stdout:
        print('FAIL: username index WAL recovery did not run.')
        sys.exit(1)
    if 'Username index recovery complete.' not in stdout:
        print('FAIL: complete username index WAL was not applied.')
        sys.exit(1)
    if rows_in_order(stdout) != [2, 3]:
        print(f'FAIL: recovered username index WAL returned wrong rows: {rows_in_order(stdout)}')
        sys.exit(1)
    if os.path.exists(db_file + '.username.idx.wal'):
        print('FAIL: recovered username index WAL was not removed.')
        sys.exit(1)
    print('PASS: committed username index WAL recovers entry file after crash.')

    write_username_index_wal(db_file, full_index_entries, include_commit=False)
    rc, stdout = run_commands(executable, db_file, (
        "SELECT * FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: incomplete username index WAL test exited with {rc}')
        sys.exit(1)
    if 'Ignoring incomplete username index WAL.' not in stdout:
        print('FAIL: incomplete username index WAL was not reported.')
        sys.exit(1)
    if rows_in_order(stdout) != [2, 3]:
        print(f'FAIL: incomplete username index WAL affected query results: {rows_in_order(stdout)}')
        sys.exit(1)
    if os.path.exists(db_file + '.username.idx.wal'):
        print('FAIL: incomplete username index WAL was not removed.')
        sys.exit(1)
    print('PASS: incomplete username index WAL is ignored atomically.')

    remove_database(db_file)
    rc, _ = run_commands(executable, db_file, seed + ".exit\n")
    if rc != 0:
        print(f'FAIL: seed before catalog WAL recovery exited with {rc}')
        sys.exit(1)
    write_catalog_wal(db_file, include_commit=True)
    rc, stdout = run_commands(executable, db_file, (
        "EXPLAIN SELECT * FROM users WHERE username = 'alice';\n"
        "SELECT * FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: catalog WAL recovery exited with {rc}')
        sys.exit(1)
    if 'Index catalog WAL found. Recovering...' not in stdout:
        print('FAIL: catalog WAL recovery did not run.')
        sys.exit(1)
    if 'Index catalog recovery complete.' not in stdout:
        print('FAIL: complete catalog WAL was not applied.')
        sys.exit(1)
    if "PLAN: SECONDARY INDEX LOOKUP (username = 'alice')" not in stdout:
        print('FAIL: recovered catalog WAL did not enable username index.')
        sys.exit(1)
    if not os.path.exists(db_file + '.catalog') or os.path.exists(db_file + '.catalog.wal'):
        print('FAIL: catalog WAL recovery did not leave expected files.')
        sys.exit(1)
    print('PASS: committed index catalog WAL recovers metadata after crash.')

    remove_database(db_file)
    rc, _ = run_commands(executable, db_file, seed + ".exit\n")
    if rc != 0:
        print(f'FAIL: seed before incomplete catalog WAL test exited with {rc}')
        sys.exit(1)
    write_catalog_wal(db_file, include_commit=False)
    rc, stdout = run_commands(executable, db_file, (
        "EXPLAIN SELECT * FROM users WHERE username = 'alice';\n"
        ".exit\n"
    ))
    if rc != 0:
        print(f'FAIL: incomplete catalog WAL test exited with {rc}')
        sys.exit(1)
    if 'Ignoring incomplete index catalog WAL.' not in stdout:
        print('FAIL: incomplete catalog WAL was not reported.')
        sys.exit(1)
    if "PLAN: FULL TABLE SCAN (username = 'alice')" not in stdout:
        print('FAIL: incomplete catalog WAL should not enable username index.')
        sys.exit(1)
    if os.path.exists(db_file + '.catalog') or os.path.exists(db_file + '.catalog.wal'):
        print('FAIL: incomplete catalog WAL left stale metadata files.')
        sys.exit(1)
    print('PASS: incomplete index catalog WAL is ignored atomically.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
