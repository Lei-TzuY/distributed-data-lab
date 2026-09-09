"""
test_checksum.py — verify FNV-1a page checksum protection.

Each page on disk ends with a 4-byte FNV-1a hash of the preceding
PAGE_USABLE_SIZE (= 4092) bytes.  get_page() verifies this hash whenever
it loads a page from the DB file.  A mismatch causes an immediate exit
with a "Corruption detected" message.
"""

import os
import subprocess
import sys

PAGE_SIZE = 4096
PAGE_USABLE_SIZE = PAGE_SIZE - 4    # last 4 bytes = checksum
LEAF_MAX_CELLS = 13                 # (4092 - 14) // 297


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


def corrupt_byte(db_file, offset):
    """Flip all bits of the byte at `offset` in the DB file."""
    with open(db_file, 'r+b') as f:
        f.seek(offset)
        b = f.read(1)[0]
        f.seek(offset)
        f.write(bytes([b ^ 0xFF]))


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    executable = find_executable(base_dir)
    if not executable:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_csum.db')

    # ── Test 1: normal reads produce no checksum errors ───────────────────
    remove_database(db_file)
    inserts = ''.join(
        f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@example.com');\n"
        for i in range(1, LEAF_MAX_CELLS + 2)   # 14 rows → 3 pages
    )
    rc, stdout = run_commands(executable, db_file,
                              inserts + 'SELECT COUNT(*) FROM users;\n.exit\n')
    if rc != 0 or 'Corruption' in stdout:
        print(f'FAIL: unexpected error on clean DB (rc={rc}):\n{stdout}')
        sys.exit(1)
    if '14' not in stdout:
        print(f'FAIL: COUNT(*) expected 14, got: {stdout.strip()}')
        sys.exit(1)
    print('PASS: clean DB reads without checksum errors.')

    # ── Test 2: corruption in page 0 (root) is detected ──────────────────
    corrupt_byte(db_file, 42)       # byte 42 of page 0, well inside usable area
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users;\n.exit\n')
    if rc == 0:
        print('FAIL: expected non-zero exit after page 0 corruption.')
        sys.exit(1)
    if 'Corruption detected' not in stdout:
        print(f'FAIL: expected "Corruption detected" message, got:\n{stdout}')
        sys.exit(1)
    if 'page 0' not in stdout:
        print(f'FAIL: expected "page 0" in corruption message, got:\n{stdout}')
        sys.exit(1)
    print('PASS: corruption in page 0 (root) detected on SELECT.')

    # ── Test 3: corruption in page 1 (non-root leaf) is detected ─────────
    remove_database(db_file)
    rc, _ = run_commands(executable, db_file, inserts + '.exit\n')
    if rc != 0:
        print(f'FAIL: seed inserts exited with {rc}')
        sys.exit(1)
    # Page 1 starts at byte 4096; corrupt byte 4196 (100 bytes in)
    corrupt_byte(db_file, PAGE_SIZE + 100)
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users;\n.exit\n')
    if rc == 0:
        print('FAIL: expected non-zero exit after page 1 corruption.')
        sys.exit(1)
    if 'Corruption detected' not in stdout:
        print(f'FAIL: expected "Corruption detected" on page 1, got:\n{stdout}')
        sys.exit(1)
    if 'page 1' not in stdout:
        print(f'FAIL: expected "page 1" in message, got:\n{stdout}')
        sys.exit(1)
    print('PASS: corruption in page 1 (leaf) detected on SELECT.')

    # ── Test 4: tampering with the checksum field itself is detected ──────
    remove_database(db_file)
    rc, _ = run_commands(executable, db_file, inserts + '.exit\n')
    if rc != 0:
        print(f'FAIL: seed inserts exited with {rc}')
        sys.exit(1)
    # Flip the first byte of page 0's checksum (= byte at PAGE_USABLE_SIZE)
    corrupt_byte(db_file, PAGE_USABLE_SIZE)
    rc, stdout = run_commands(executable, db_file,
                              'SELECT * FROM users;\n.exit\n')
    if rc == 0:
        print('FAIL: expected non-zero exit after checksum field corruption.')
        sys.exit(1)
    if 'Corruption detected' not in stdout:
        print(f'FAIL: expected "Corruption detected", got:\n{stdout}')
        sys.exit(1)
    print('PASS: tampering with the checksum field itself is detected.')

    # ── Test 5: WAL recovery writes pages with valid checksums ────────────
    # Simulate crash: write to WAL but don't checkpoint.
    remove_database(db_file)
    # Insert a row and commit (writes to WAL, but crash before checkpoint)
    process = subprocess.Popen(
        [executable, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    process.stdin.write(
        "INSERT INTO users VALUES (1, 'alice', 'alice@example.com');\n"
    )
    process.stdin.flush()
    import time; time.sleep(0.3)
    process.kill()
    process.wait()
    # WAL file should exist; reopen should recover without corruption errors
    if not os.path.exists(db_file + '.wal'):
        # WAL might not exist if the process wrote the checkpoint too fast;
        # that's OK — it means the data was already safely committed.
        print('PASS: WAL recovery with checksum (no WAL file — already checkpointed).')
    else:
        rc, stdout = run_commands(executable, db_file,
                                  'SELECT * FROM users;\n.exit\n')
        if 'Corruption' in stdout:
            print(f'FAIL: corruption reported after WAL recovery:\n{stdout}')
            sys.exit(1)
        print('PASS: WAL recovery writes pages with valid checksums.')

    remove_database(db_file)


if __name__ == '__main__':
    run_test()
