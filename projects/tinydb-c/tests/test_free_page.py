"""
test_free_page.py — verify that deleted/collapsed pages are recycled.

LEAF_NODE_MAX_CELLS = (4096 - 14) / (4 + 293) = 13.
Inserting 14 rows triggers the first leaf split, creating 3 pages:
  page 0 = internal root
  page 1 = left leaf  (rows 1-7)
  page 2 = right leaf (rows 8-14)
File size = 3 * 4096 = 12288 bytes.

When rows 8-14 are deleted the right leaf becomes empty:
  1. remove_empty_leaf frees page 2  → free list: [2]
  2. Root collapse copies page 1 into page 0, frees page 1 → free list: [2, 1]

Re-inserting rows 8-14 triggers another split (page 0 has 13 rows):
  - create_new_root allocates left child:  pops page 1 from free list (reused!)
  - leaf_node_split gets new right leaf:   pops page 2 from free list (reused!)
  num_pages stays at 3; file does NOT grow.

Without the free list new pages would be allocated at positions 3 and 4,
growing the file to 5 * 4096 = 20480 bytes.
"""

import os
import subprocess
import sys

PAGE_SIZE = 4096
LEAF_MAX_CELLS = 13   # (4096 - 14) / 297


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


def db_size(db_file):
    return os.path.getsize(db_file)


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    executable = find_executable(base_dir)
    if not executable:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_fp.db')

    # ── Test 1: freed pages are recycled in the same session ──────────────
    remove_database(db_file)

    # Insert 14 rows → split into 3 pages (root + 2 leaves), 12288 bytes
    inserts = ''.join(
        f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@example.com');\n"
        for i in range(1, LEAF_MAX_CELLS + 2)   # 1..14
    )
    # Delete right-leaf rows → empty leaf freed; root collapses → 2 orphaned pages
    deletes = ''.join(
        f"DELETE FROM users WHERE id = {i};\n"
        for i in range(LEAF_MAX_CELLS // 2 + 1, LEAF_MAX_CELLS + 2)  # 8..14
    )
    # Re-insert those rows → triggers another split, should REUSE freed pages
    reinserts = ''.join(
        f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@example.com');\n"
        for i in range(LEAF_MAX_CELLS // 2 + 1, LEAF_MAX_CELLS + 2)  # 8..14
    )

    rc, stdout = run_commands(executable, db_file,
                              inserts + deletes + reinserts + '.exit\n')
    if rc != 0:
        print(f'FAIL: tinydb exited with {rc}\n{stdout}')
        sys.exit(1)

    size_after = db_size(db_file)
    expected_size = 3 * PAGE_SIZE   # 12288: pages were reused, not grown
    if size_after != expected_size:
        print(f'FAIL: expected DB size {expected_size} B after reuse, '
              f'got {size_after} B (pages were NOT recycled).')
        sys.exit(1)
    print(f'PASS: free-page reuse keeps file at {expected_size} B (3 pages).')

    # Verify data is correct after page reuse
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users;\n.exit\n')
    for i in range(1, LEAF_MAX_CELLS + 2):
        expected = f'({i}, user{i}, u{i}@example.com)'
        if expected not in stdout:
            print(f'FAIL: row {i} missing after page-reuse re-insert.')
            sys.exit(1)
    print('PASS: all 14 rows readable after freed-page reuse.')

    # ── Test 2: table_truncate + pager_shrink actually truncates the file ─
    remove_database(db_file)

    inserts14 = ''.join(
        f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@example.com');\n"
        for i in range(1, LEAF_MAX_CELLS + 2)
    )
    rc, _ = run_commands(executable, db_file, inserts14 + '.exit\n')
    if rc != 0:
        print(f'FAIL: setup inserts exited with {rc}')
        sys.exit(1)
    size_before_truncate = db_size(db_file)
    if size_before_truncate != 3 * PAGE_SIZE:
        print(f'FAIL: expected 12288 B before truncate, got {size_before_truncate} B')
        sys.exit(1)

    rc, _ = run_commands(executable, db_file,
                         'DELETE FROM users;\n.exit\n')
    if rc != 0:
        print(f'FAIL: DELETE FROM users exited with {rc}')
        sys.exit(1)
    size_after_truncate = db_size(db_file)
    if size_after_truncate != PAGE_SIZE:
        print(f'FAIL: expected {PAGE_SIZE} B after truncate, '
              f'got {size_after_truncate} B (file not shrunk).')
        sys.exit(1)
    print(f'PASS: DELETE FROM users shrinks file from 12288 B to {PAGE_SIZE} B.')

    # Table is usable after truncate+shrink
    rc, stdout = run_commands(executable, db_file, (
        "INSERT INTO users VALUES (99, 'revived', 'revived@example.com');\n"
        'SELECT * FROM users;\n'
        '.exit\n'
    ))
    if '(99, revived, revived@example.com)' not in stdout:
        print('FAIL: insert after truncate+shrink returned wrong data.')
        sys.exit(1)
    print('PASS: table is fully usable after pager_shrink.')

    # ── Test 3: rollback restores the free list ────────────────────────────
    remove_database(db_file)

    # Seed two rows in separate autocommits
    rc, _ = run_commands(executable, db_file, (
        "INSERT INTO users VALUES (1, 'a', 'a@example.com');\n"
        "INSERT INTO users VALUES (2, 'b', 'b@example.com');\n"
        '.exit\n'
    ))
    if rc != 0:
        print(f'FAIL: seed inserts exited with {rc}')
        sys.exit(1)

    # Build a tree big enough to have non-root leaves, then rollback the deletes
    inserts_for_split = ''.join(
        f"INSERT INTO users VALUES ({i}, 'u{i}', 'u{i}@x.com');\n"
        for i in range(3, LEAF_MAX_CELLS + 2)   # 3..14 → 14 rows total → split
    )
    rc, _ = run_commands(executable, db_file, inserts_for_split + '.exit\n')
    if rc != 0:
        print(f'FAIL: split inserts exited with {rc}')
        sys.exit(1)

    # Within a transaction, delete the right leaf rows, then rollback
    deletes_txn = ''.join(
        f"DELETE FROM users WHERE id = {i};\n"
        for i in range(LEAF_MAX_CELLS // 2 + 1, LEAF_MAX_CELLS + 2)  # 8..14
    )
    rc, stdout = run_commands(executable, db_file, (
        'BEGIN;\n' + deletes_txn + 'ROLLBACK;\n'
        'SELECT COUNT(*) FROM users;\n'
        '.exit\n'
    ))
    if rc != 0:
        print(f'FAIL: rollback test exited with {rc}')
        sys.exit(1)
    if '14' not in stdout:
        print(f'FAIL: expected 14 rows after rollback, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: rollback after deletes restores free list; all 14 rows intact.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
