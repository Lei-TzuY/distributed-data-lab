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


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
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

    db_file = os.path.join(
        os.path.dirname(__file__), "test_generic_same_index_range_fusion.db"
    )
    cleanup(db_file)

    try:
        first = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT);",
                "INSERT INTO products VALUES (1, 'p5', 5);",
                "INSERT INTO products VALUES (2, 'p10', 10);",
                "INSERT INTO products VALUES (3, 'p15', 15);",
                "INSERT INTO products VALUES (4, 'p20', 20);",
                "INSERT INTO products VALUES (5, 'p25', 25);",
                "INSERT INTO products VALUES (6, 'x30', 30);",
                "INSERT INTO products VALUES (7, 'p35', 35);",
                "INSERT INTO products VALUES (8, 'p40', 40);",
                "INSERT INTO products VALUES (9, 'p45', 45);",
                "INSERT INTO products VALUES (10, 'p50', 50);",
                "INSERT INTO products VALUES (11, 'p55', 55);",
                "INSERT INTO products VALUES (12, 'p60', 60);",
                "CREATE INDEX idx_products_price ON products(price);",
                "EXPLAIN SELECT id FROM products WHERE price >= 20 AND price <= 40;",
                "SELECT COUNT(*) FROM products WHERE price >= 20 AND price <= 40;",
                "EXPLAIN SELECT id FROM products WHERE price >= 10 AND price <= 50 AND price > 30;",
                "SELECT COUNT(*) FROM products WHERE price >= 10 AND price <= 50 AND price > 30;",
                "SELECT COUNT(*) FROM products WHERE price >= 50 AND price < 20;",
                "EXPLAIN SELECT name FROM products WHERE price >= 20 AND price <= 40 AND name LIKE 'p%';",
                "SELECT COUNT(*) FROM products WHERE price >= 20 AND price <= 40 AND name LIKE 'p%';",
                "EXPLAIN ANALYZE SELECT name FROM products WHERE price > 20 AND price < 40 LIMIT 2 OFFSET 1;",
                "CREATE TABLE labels (id INT, name VARCHAR, rank INT);",
                "INSERT INTO labels VALUES (1, 'alpha', 1);",
                "INSERT INTO labels VALUES (2, 'beta', 2);",
                "INSERT INTO labels VALUES (3, 'charlie', 3);",
                "INSERT INTO labels VALUES (4, 'delta', 4);",
                "INSERT INTO labels VALUES (5, 'echo', 5);",
                "CREATE INDEX idx_labels_name ON labels(name);",
                "EXPLAIN SELECT name FROM labels WHERE name >= 'beta' AND name < 'delta';",
                "SELECT COUNT(*) FROM labels WHERE name >= 'beta' AND name < 'delta';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )

        if first.count("PLAN: GENERIC SECONDARY INDEX FUSED RANGE") < 5:
            raise AssertionError("expected fused range plans were not selected\n" + first)
        require(first, "INDEX: idx_products_price")
        require(first, "RANGE TERMS: 2 on price")
        require(first, "RANGE TERMS: 3 on price")
        require(first, "FILTER: price >= 20 AND price <= 40")
        require(first, "FILTER: price >= 20 AND price <= 40 AND name LIKE 'p%'")
        require(first, "INDEX: idx_labels_name")
        require(first, "RANGE TERMS: 2 on name")
        require(first, "FILTER: name >= 'beta' AND name < 'delta'")
        require(first, "x30")
        require(first, "p35")
        require(first, "ok")
        if scalar_results(first) != [5, 4, 0, 4, 2]:
            raise AssertionError("same-index fused range counts are wrong\n" + first)

        reopened = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM products WHERE price >= 20 AND price <= 40;",
                "SELECT COUNT(*) FROM products WHERE price >= 20 AND price <= 40;",
                "SELECT COUNT(*) FROM labels WHERE name >= 'beta' AND name < 'delta';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(reopened, "PLAN: GENERIC SECONDARY INDEX FUSED RANGE")
        if scalar_results(reopened) != [5, 2]:
            raise AssertionError("fused ranges changed after clean reopen\n" + reopened)
        require(reopened, "ok")

        mutated = run_session(
            executable,
            db_file,
            [
                "UPDATE products SET price = 100 WHERE id = 5;",
                "EXPLAIN SELECT id FROM products WHERE price >= 20 AND price <= 40;",
                "SELECT COUNT(*) FROM products WHERE price >= 20 AND price <= 40;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(mutated, "PLAN: GENERIC SECONDARY INDEX FUSED RANGE")
        if scalar_results(mutated) != [4]:
            raise AssertionError("epoch-stale fused range was not rebuilt correctly\n" + mutated)
        require(mutated, "ok")

        dropped = run_session(
            executable,
            db_file,
            [
                "DROP INDEX idx_products_price;",
                "EXPLAIN SELECT id FROM products WHERE price >= 20 AND price <= 40;",
                "SELECT COUNT(*) FROM products WHERE price >= 20 AND price <= 40;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if "PLAN: GENERIC SECONDARY INDEX FUSED RANGE" in dropped:
            raise AssertionError("fused range remained active after DROP INDEX\n" + dropped)
        if scalar_results(dropped) != [4]:
            raise AssertionError("DROP INDEX fallback changed fused-range semantics\n" + dropped)
        require(dropped, "ok")

        print(
            "PASS: repeated ordered predicates on one secondary index fuse into one typed "
            "bounded snapshot interval for INT and VARCHAR keys, handle contradictory and "
            "three-term bounds, retain residual LIKE revalidation, rebuild after indexed "
            "mutation, survive reopen, and fall back safely after DROP INDEX."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
