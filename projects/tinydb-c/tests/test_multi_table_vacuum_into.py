import glob
import os
import re
import subprocess
import sys


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


def cleanup(prefix):
    for path in glob.glob(prefix + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_file, commands):
    result = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def scalar_results(output):
    return [int(value) for value in re.findall(r"db > (\d+)\nExecuted\.", output)]


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    test_dir = os.path.dirname(__file__)
    source = os.path.join(test_dir, "test_multi_table_vacuum_source.db")
    destination = os.path.join(test_dir, "test_multi_table_vacuum_compact.db")
    cleanup(source)
    cleanup(destination)

    try:
        setup_commands = [
            "INSERT INTO users VALUES (1, 'main1', 'm1@test.com');",
            "INSERT INTO users VALUES (2, 'main2', 'm2@test.com');",
            "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
            "INSERT INTO archive VALUES (1, 'archive1', 'a1@test.com');",
            "INSERT INTO archive VALUES (2, 'archive2', 'a2@test.com');",
            "INSERT INTO archive VALUES (3, 'archive3', 'a3@test.com');",
            "CREATE TABLE products (id INT, name VARCHAR, price INT);",
        ]
        for i in range(1, 41):
            setup_commands.append(
                f"INSERT INTO products VALUES ({i}, 'p{i}', {i * 10});"
            )
        setup_commands.extend(
            [
                "CREATE INDEX idx_products_price ON products(price);",
                "CREATE TABLE wide_archive (id INT, left_text VARCHAR(255), right_text VARCHAR(255));",
                "INSERT INTO wide_archive VALUES (10, 'left-a', 'right-c');",
                "INSERT INTO wide_archive VALUES (20, 'left-b', 'right-a');",
                "INSERT INTO wide_archive VALUES (30, 'left-c', 'right-b');",
                "CREATE INDEX idx_wide_right ON wide_archive(right_text);",
                "CREATE VIEW user_copy AS SELECT * FROM users;",
                "PRAGMA user_version = 77;",
                "DELETE FROM products WHERE price < 260;",
                "PRAGMA integrity_check;",
                ".exit",
            ]
        )
        setup = run_session(executable, source, setup_commands)
        require(setup, "ok")

        if not os.path.exists(source):
            raise AssertionError("source database was not created")
        source_size = os.path.getsize(source)

        vacuum = run_session(
            executable,
            source,
            [
                f"VACUUM INTO '{destination}';",
                "SELECT COUNT(*) FROM products;",
                "SELECT COUNT(*) FROM wide_archive;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(vacuum, f"Database backed up to '{destination}'.")
        require(vacuum, "ok")
        if scalar_results(vacuum) != [15, 3]:
            raise AssertionError("VACUUM INTO changed the source database\n" + vacuum)

        if not os.path.exists(destination):
            raise AssertionError("logical VACUUM INTO did not create destination")
        destination_size = os.path.getsize(destination)
        if destination_size >= source_size:
            raise AssertionError(
                f"logical VACUUM INTO did not compact pages: source={source_size}, "
                f"destination={destination_size}"
            )

        reopened = run_session(
            executable,
            destination,
            [
                "SELECT * FROM users WHERE id = 1;",
                "SELECT * FROM archive WHERE id = 1;",
                "SELECT COUNT(*) FROM products;",
                "SELECT name FROM products WHERE id = 26;",
                "SELECT right_text FROM wide_archive WHERE id = 20;",
                "SELECT left_text FROM wide_archive WHERE right_text = 'right-b';",
                "EXPLAIN SELECT left_text FROM wide_archive WHERE right_text >= 'right-b';",
                "INSERT INTO wide_archive VALUES (40, 'left-d', 'right-d');",
                "SELECT COUNT(*) FROM wide_archive;",
                "PRAGMA user_version;",
                "PRAGMA index_list(products);",
                "PRAGMA index_list(wide_archive);",
                "EXPLAIN SELECT name FROM products WHERE price >= 350;",
                "SELECT * FROM user_copy;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(reopened, "(1, main1, m1@test.com)")
        require(reopened, "(1, archive1, a1@test.com)")
        require(reopened, "p26")
        require(reopened, "right-a")
        require(reopened, "left-c")
        require(reopened, "77")
        require(reopened, "idx_products_price")
        require(reopened, "idx_wide_right")
        require(reopened, "PLAN: GENERIC SECONDARY INDEX RANGE SCAN")
        require(reopened, "INDEX: idx_products_price")
        require(reopened, "INDEX: idx_wide_right")
        require(reopened, "(2, main2, m2@test.com)")
        require(reopened, "ok")
        if scalar_results(reopened)[:2] != [15, 4]:
            raise AssertionError("destination row counts are wrong\n" + reopened)

        collision = run_session(
            executable,
            source,
            [
                f"VACUUM INTO '{destination}';",
                ".exit",
            ],
        )
        require(
            collision,
            "VACUUM INTO destination or one of its sidecars already exists",
        )

        print(
            "PASS: multi-table VACUUM INTO logically rebuilds compact independent roots, "
            "preserves users/legacy/generic and 516-byte payload-native rows, user_version, "
            "views and generic indexes, keeps the destination wide root writable after "
            "reopen, passes integrity, keeps the source unchanged, and refuses an existing "
            "destination."
        )
    finally:
        cleanup(source)
        cleanup(destination)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
