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


def stat_mtime(path):
    return os.stat(path).st_mtime_ns


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_index_snapshot.db")
    index_file = db_file + ".idx_products_price.idx"
    epoch_file = db_file + ".gidx.epoch"
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                "INSERT INTO products VALUES (1, 'p1', 100);",
                "INSERT INTO products VALUES (2, 'p2', 100);",
                "INSERT INTO products VALUES (3, 'p3', 200);",
                "INSERT INTO products VALUES (4, 'p4', 200);",
                "INSERT INTO products VALUES (5, 'p5', 200);",
                "CREATE INDEX idx_products_price ON products(price);",
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                ".exit",
            ],
        )
        require(first, "db > 3\nExecuted.")
        if not os.path.exists(index_file):
            raise AssertionError("persistent generic index snapshot was not created")
        if not os.path.exists(epoch_file):
            raise AssertionError("generic index epoch file was not created")

        initial_snapshot_mtime = stat_mtime(index_file)
        initial_epoch = open(epoch_file, "rb").read()

        # A clean reopen should load the durable snapshot rather than rebuild it.
        time.sleep(1.1)
        second = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                ".exit",
            ],
        )
        require(second, "db > 3\nExecuted.")
        if stat_mtime(index_file) != initial_snapshot_mtime:
            raise AssertionError("valid generic index snapshot was rewritten on clean reopen")

        # Corruption must never be trusted. The query should rebuild and rewrite.
        with open(index_file, "r+b") as snapshot:
            first_byte = snapshot.read(1)
            if not first_byte:
                raise AssertionError("generic index snapshot is unexpectedly empty")
            snapshot.seek(0)
            snapshot.write(bytes([first_byte[0] ^ 0xFF]))
            snapshot.flush()
            os.fsync(snapshot.fileno())
        corrupted_mtime = stat_mtime(index_file)
        time.sleep(1.1)
        third = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                ".exit",
            ],
        )
        require(third, "db > 3\nExecuted.")
        rebuilt_mtime = stat_mtime(index_file)
        if rebuilt_mtime <= corrupted_mtime:
            raise AssertionError("corrupt generic index snapshot was not rebuilt")

        # Mutate without reading the index again. Epoch must advance before data
        # changes, leaving the old snapshot safely stale across close/reopen.
        before_mutation_snapshot = stat_mtime(index_file)
        before_mutation_epoch = open(epoch_file, "rb").read()
        mutation = run_session(
            executable,
            db_file,
            [
                "UPDATE products SET price = 200 WHERE id = 1;",
                ".exit",
            ],
        )
        require(mutation, "Executed.")
        if stat_mtime(index_file) != before_mutation_snapshot:
            raise AssertionError("mutation unexpectedly rewrote the lazy index snapshot")
        after_mutation_epoch = open(epoch_file, "rb").read()
        if after_mutation_epoch == before_mutation_epoch:
            raise AssertionError("generic index epoch did not advance before mutation")
        if after_mutation_epoch == initial_epoch:
            raise AssertionError("generic index epoch remained at its initial value")

        time.sleep(1.1)
        after_mutation = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                ".exit",
            ],
        )
        require(after_mutation, "db > 4\nExecuted.")
        mutation_rebuild_mtime = stat_mtime(index_file)
        if mutation_rebuild_mtime <= before_mutation_snapshot:
            raise AssertionError("epoch-stale snapshot was not rebuilt after mutation")

        # Rollback still advances the epoch before the attempted mutation. That
        # intentionally causes a harmless rebuild rather than risking stale data.
        before_rollback_snapshot = mutation_rebuild_mtime
        before_rollback_epoch = open(epoch_file, "rb").read()
        rolled_back = run_session(
            executable,
            db_file,
            [
                "BEGIN;",
                "UPDATE products SET price = 200 WHERE id = 2;",
                "ROLLBACK;",
                ".exit",
            ],
        )
        require(rolled_back, "Executed.")
        after_rollback_epoch = open(epoch_file, "rb").read()
        if after_rollback_epoch == before_rollback_epoch:
            raise AssertionError("rollback-path mutation did not invalidate the epoch")
        if stat_mtime(index_file) != before_rollback_snapshot:
            raise AssertionError("rollback path unexpectedly rewrote snapshot before query")

        time.sleep(1.1)
        after_rollback = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price = 200;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(after_rollback, "db > 4\nExecuted.")
        require(after_rollback, "ok")
        if stat_mtime(index_file) <= before_rollback_snapshot:
            raise AssertionError("rollback-invalidated snapshot was not rebuilt")

        print(
            "PASS: durable generic-index snapshot load, checksum fallback, mutation epoch, "
            "rollback invalidation, and reopen correctness verified."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
