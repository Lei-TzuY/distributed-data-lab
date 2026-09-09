import os
import re
import subprocess
import sys


def find_executable(base_dir):
    candidates = [
        os.path.join(base_dir, 'build', 'Debug', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'Release', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb'),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def remove_database(db_file):
    suffixes = ('', '.wal', '.catalog', '.catalog.wal')
    for suffix in suffixes:
        path = db_file + suffix
        if os.path.exists(path):
            os.remove(path)
    base_dir = os.path.dirname(db_file)
    prefix = os.path.basename(db_file) + '.'
    for name in os.listdir(base_dir):
        if name.startswith(prefix) and (name.endswith('.idx') or name.endswith('.idx.wal')):
            os.remove(os.path.join(base_dir, name))


def run_commands(executable, db_file, commands):
    process = subprocess.run(
        [executable, db_file],
        input=commands,
        capture_output=True,
        text=True,
    )
    return process.returncode, process.stdout, process.stderr


def assert_metrics(output):
    match = re.search(
        r'ANALYZE: execution_time_ms=([0-9]+(?:\.[0-9]+)?) '
        r'cache_hits=(\d+) cache_misses=(\d+) evictions=(\d+) page_accesses=(\d+)',
        output,
    )
    if not match:
        raise AssertionError(f'EXPLAIN ANALYZE metrics missing or malformed:\n{output}')

    hits = int(match.group(2))
    misses = int(match.group(3))
    accesses = int(match.group(5))
    if accesses != hits + misses:
        raise AssertionError(
            f'page_accesses mismatch: expected {hits + misses}, got {accesses}'
        )


def run_test():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    if not executable:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_explain_analyze.db')
    remove_database(db_file)

    inserts = ''.join(
        f"INSERT INTO users VALUES ({i}, 'user{i}', 'u{i}@example.com');\n"
        for i in range(1, 31)
    )

    try:
        rc, stdout, stderr = run_commands(
            executable,
            db_file,
            inserts
            + "CREATE INDEX idx_users_email ON users(email);\n"
            + "EXPLAIN ANALYZE SELECT * FROM users WHERE id = 17;\n"
            + "EXPLAIN ANALYZE SELECT * FROM users WHERE email = 'u10@example.com';\n"
            + "EXPLAIN SELECT * FROM users WHERE id = 17;\n"
            + ".exit\n",
        )
        if rc != 0:
            print(f'FAIL: tinydb exited with {rc}')
            print(stdout)
            print(stderr)
            sys.exit(1)

        if 'PLAN: PRIMARY KEY LOOKUP (id = 17)' not in stdout:
            print('FAIL: primary-key EXPLAIN ANALYZE did not show the expected plan.')
            print(stdout)
            sys.exit(1)
        if '(17, user17, u17@example.com)' not in stdout:
            print('FAIL: EXPLAIN ANALYZE did not execute the primary-key query.')
            print(stdout)
            sys.exit(1)

        if "PLAN: SECONDARY INDEX LOOKUP (email = 'u10@example.com')" not in stdout:
            print('FAIL: indexed EXPLAIN ANALYZE did not show the expected index plan.')
            print(stdout)
            sys.exit(1)
        if '(10, user10, u10@example.com)' not in stdout:
            print('FAIL: indexed EXPLAIN ANALYZE did not execute the query.')
            print(stdout)
            sys.exit(1)

        analyze_sections = stdout.split('ANALYZE:')[1:]
        if len(analyze_sections) != 2:
            print(f'FAIL: expected exactly 2 ANALYZE metric records, got {len(analyze_sections)}.')
            print(stdout)
            sys.exit(1)
        assert_metrics('ANALYZE:' + analyze_sections[0])
        assert_metrics('ANALYZE:' + analyze_sections[1])

        tail_after_second = 'ANALYZE:' + analyze_sections[1]
        explain_only_plan_count = stdout.count('PLAN: PRIMARY KEY LOOKUP (id = 17)')
        if explain_only_plan_count != 2:
            print('FAIL: legacy EXPLAIN behavior changed unexpectedly.')
            print(stdout)
            sys.exit(1)
        if stdout.count('ACTUAL RESULT') != 2:
            print('FAIL: plain EXPLAIN unexpectedly executed its query.')
            print(stdout)
            sys.exit(1)

        print('PASS: EXPLAIN ANALYZE reports plan, executes query, and exposes pager metrics.')
    except AssertionError as exc:
        print(f'FAIL: {exc}')
        sys.exit(1)
    finally:
        remove_database(db_file)


if __name__ == '__main__':
    run_test()
