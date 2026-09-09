import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time


def find_executable(repo_root):
    candidates = [
        os.path.join(repo_root, 'build', 'Debug', 'tinydb.exe'),
        os.path.join(repo_root, 'build', 'Release', 'tinydb.exe'),
        os.path.join(repo_root, 'build', 'tinydb.exe'),
        os.path.join(repo_root, 'build', 'tinydb'),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def run_commands(executable, db_file, commands, timeout=60):
    result = subprocess.run(
        [executable, db_file],
        input=commands,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    return result.returncode, result.stdout, result.stderr


def cleanup(db_file):
    for path in glob.glob(db_file + '*'):
        try:
            os.remove(path)
        except OSError:
            pass


def run_test():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    if executable is None:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    temp_dir = tempfile.mkdtemp(prefix='tinydb-no-steal-')
    db_file = os.path.join(temp_dir, 'no_steal.db')

    try:
        seed = ''.join(
            f"INSERT INTO users VALUES ({i}, 'stable{i}', 'stable{i}@example.com');\n"
            for i in range(1, 21)
        ) + '.exit\n'
        rc, stdout, stderr = run_commands(executable, db_file, seed)
        if rc != 0:
            print('FAIL: unable to seed committed rows.')
            print(stdout)
            print(stderr)
            sys.exit(1)

        # A single transaction large enough to dirty far more than the
        # 16-frame buffer pool. Old steal-on-eviction behavior could write
        # these uncommitted pages into the main DB before COMMIT.
        commands = ['BEGIN;\n']
        for row_id in range(1000, 1700):
            commands.append(
                f"INSERT INTO users VALUES ({row_id}, 'ghost{row_id}', "
                f"'ghost{row_id}@example.com');\n"
            )

        process = subprocess.Popen(
            [executable, db_file],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        assert process.stdin is not None
        process.stdin.write(''.join(commands))
        process.stdin.flush()

        # 700 inserts are tiny for the engine but dirty dozens of B+ tree
        # pages, guaranteeing LRU eviction on the legacy implementation.
        time.sleep(2.0)
        process.kill()
        process.wait(timeout=10)

        rc, stdout, stderr = run_commands(
            executable,
            db_file,
            "SELECT COUNT(*) FROM users;\n"
            "SELECT * FROM users WHERE id >= 1000 LIMIT 1;\n"
            "PRAGMA integrity_check;\n"
            ".exit\n",
        )
        if rc != 0:
            print('FAIL: database could not be reopened after crash.')
            print(stdout)
            print(stderr)
            sys.exit(1)

        if 'db > 20\nExecuted.' not in stdout:
            print('FAIL: uncommitted large transaction changed the durable row count.')
            print(stdout)
            sys.exit(1)
        if 'ghost1000' in stdout or 'ghost1699' in stdout:
            print('FAIL: an uncommitted ghost row leaked after crash.')
            print(stdout)
            sys.exit(1)
        if 'ok' not in stdout.lower():
            print('FAIL: integrity check did not pass after large transaction crash.')
            print(stdout)
            sys.exit(1)

        print('PASS: no-steal pager keeps a large uncommitted transaction out of the main DB.')
    finally:
        cleanup(db_file)
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == '__main__':
    run_test()
