import glob
import os
import subprocess
import sys
import time


def find_tinydb(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_file, commands):
    process = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if process.returncode != 0:
        raise AssertionError(process.stdout + "\n" + process.stderr)
    return process.stdout + process.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_snapshot_crash.db")
    index_file = db_file + ".idx_products_price.idx"
    epoch_file = db_file + ".gidx.epoch"
    cleanup(db_file)

    try:
        seeded = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                "INSERT INTO products VALUES (1, 'p1', 100);",
                "INSERT INTO products VALUES (2, 'p2', 200);",
                "INSERT INTO products VALUES (3, 'p3', 200);",
                "CREATE INDEX idx_products_price ON products(price);",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                ".exit",
            ],
        )
        require(seeded, "db > 2\nExecuted.")
        if not os.path.exists(index_file) or not os.path.exists(epoch_file):
            raise AssertionError("seeded persistent index files are missing")

        snapshot_mtime = os.stat(index_file).st_mtime_ns
        epoch_before = open(epoch_file, "rb").read()

        # Execute an autocommit indexed-table mutation and kill the process
        # instead of letting db_close() checkpoint normally. The epoch must have
        # been made durable before the data mutation itself begins.
        process = subprocess.Popen(
            [executable, db_file],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        assert process.stdin is not None
        process.stdin.write("UPDATE products SET price = 200 WHERE id = 1;\n")
        process.stdin.flush()
        time.sleep(1.0)
        process.kill()
        process.wait(timeout=30)

        epoch_after = open(epoch_file, "rb").read()
        if epoch_after == epoch_before:
            raise AssertionError("hard-crash mutation did not durably advance index epoch")
        if os.stat(index_file).st_mtime_ns != snapshot_mtime:
            raise AssertionError("hard-crash mutation unexpectedly rewrote old snapshot")

        # Reopen may recover the database WAL. The old index image must not be
        # accepted because its epoch predates the committed mutation.
        time.sleep(1.1)
        recovered = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(recovered, "db > 3\nExecuted.")
        require(recovered, "ok")
        if os.stat(index_file).st_mtime_ns <= snapshot_mtime:
            raise AssertionError("stale index snapshot was not rebuilt after crash recovery")

        print(
            "PASS: pre-mutation durable epoch prevents stale generic-index snapshot reuse "
            "after hard crash and WAL recovery."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
