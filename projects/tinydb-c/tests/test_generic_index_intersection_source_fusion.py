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


def require_metrics(output):
    pattern = re.compile(
        r"ANALYZE: execution_time_ms=[0-9.]+ "
        r"cache_hits=\d+ cache_misses=\d+ evictions=\d+ page_accesses=\d+"
    )
    if pattern.search(output) is None:
        raise AssertionError("missing EXPLAIN ANALYZE metrics\n" + output)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    if executable is None:
        print("FAIL: Could not find tinydb executable.")
        sys.exit(1)

    db_file = os.path.join(
        os.path.dirname(__file__),
        "test_generic_index_intersection_source_fusion.db",
    )
    price_range = db_file + ".idx_products_price.idx.range"
    stock_range = db_file + ".idx_products_stock.idx.range"
    cleanup(db_file)

    try:
        setup = run_session(
            executable,
            db_file,
            [
                "CREATE TABLE products (id INT, name VARCHAR, price INT, stock INT);",
                "INSERT INTO products VALUES (1, 'cheap', 100, 0);",
                "INSERT INTO products VALUES (2, 'mid-a', 1500, 0);",
                "INSERT INTO products VALUES (3, 'mid-b', 2500, 5);",
                "INSERT INTO products VALUES (4, 'high', 5000, 0);",
                "INSERT INTO products VALUES (5, 'mid-c', 1800, 0);",
                "CREATE INDEX idx_products_price ON products(price);",
                "CREATE INDEX idx_products_stock ON products(stock);",
                ".exit",
            ],
        )
        if "Error:" in setup or "Syntax error" in setup:
            raise AssertionError(setup)

        if os.path.exists(price_range) or os.path.exists(stock_range):
            raise AssertionError("CREATE INDEX unexpectedly materialized ordered range snapshots")

        contradictory = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price >= 9000 AND price < 1000 AND stock = 0;",
                ".exit",
            ],
        )
        if scalar_results(contradictory) != [0]:
            raise AssertionError("contradictory fused source did not short-circuit\n" + contradictory)
        if not os.path.exists(price_range):
            raise AssertionError("contradictory price source did not create its typed snapshot")
        if os.path.exists(stock_range):
            raise AssertionError("empty fused price source failed to short-circuit before stock source")

        one_fused_source = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM products WHERE price >= 1000 AND price <= 3000 AND stock = 0;",
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND price <= 3000 AND stock = 0;",
                ".exit",
            ],
        )
        require(one_fused_source, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(one_fused_source, "INDEXES: 2")
        require(one_fused_source, "INDEX TERMS: 3")
        require(one_fused_source, "FUSED SOURCES: 1")
        require(
            one_fused_source,
            "FILTER: price >= 1000 AND price <= 3000 AND stock = 0",
        )
        if scalar_results(one_fused_source) != [2]:
            raise AssertionError("one-source fusion returned wrong intersection count\n" + one_fused_source)
        if not os.path.exists(stock_range):
            raise AssertionError("non-empty intersection did not materialize stock source snapshot")

        two_fused_sources = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM products WHERE price >= 1000 AND price <= 3000 AND stock >= 0 AND stock <= 0;",
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND price <= 3000 AND stock >= 0 AND stock <= 0;",
                ".exit",
            ],
        )
        require(two_fused_sources, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(two_fused_sources, "INDEXES: 2")
        require(two_fused_sources, "INDEX TERMS: 4")
        require(two_fused_sources, "FUSED SOURCES: 2")
        if scalar_results(two_fused_sources) != [2]:
            raise AssertionError("two-source fusion returned wrong intersection count\n" + two_fused_sources)

        tighter = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT id FROM products WHERE price >= 1000 AND price <= 3000 AND price > 1700 AND stock = 0;",
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND price <= 3000 AND price > 1700 AND stock = 0;",
                ".exit",
            ],
        )
        require(tighter, "INDEX TERMS: 4")
        require(tighter, "FUSED SOURCES: 1")
        if scalar_results(tighter) != [1]:
            raise AssertionError("three-term source fusion returned wrong count\n" + tighter)

        like_residual = run_session(
            executable,
            db_file,
            [
                "EXPLAIN SELECT name FROM products WHERE price >= 1000 AND price <= 3000 AND stock = 0 AND name LIKE 'mid-%';",
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND price <= 3000 AND stock = 0 AND name LIKE 'mid-%';",
                ".exit",
            ],
        )
        require(like_residual, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(like_residual, "INDEX TERMS: 3")
        require(like_residual, "FUSED SOURCES: 1")
        require(like_residual, "name LIKE 'mid-%'")
        if scalar_results(like_residual) != [2]:
            raise AssertionError("LIKE residual changed fused intersection result\n" + like_residual)

        mutation = run_session(
            executable,
            db_file,
            [
                "UPDATE products SET stock = 7 WHERE id = 5;",
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND price <= 3000 AND stock = 0;",
                "EXPLAIN ANALYZE SELECT id FROM products WHERE price >= 1000 AND price <= 3000 AND stock = 0;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if scalar_results(mutation) != [1]:
            raise AssertionError("epoch rebuild did not refresh fused intersection sources\n" + mutation)
        require(mutation, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(mutation, "INDEX TERMS: 3")
        require(mutation, "FUSED SOURCES: 1")
        require(mutation, "ACTUAL RESULT")
        require(mutation, "ok")
        require_metrics(mutation)

        reopened = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND price <= 3000 AND stock = 0;",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if scalar_results(reopened) != [1]:
            raise AssertionError("fused intersection result did not survive reopen\n" + reopened)
        require(reopened, "ok")

        dropped = run_session(
            executable,
            db_file,
            [
                "DROP INDEX idx_products_stock;",
                "EXPLAIN SELECT id FROM products WHERE price >= 1000 AND price <= 3000 AND stock = 0;",
                "SELECT COUNT(*) FROM products WHERE price >= 1000 AND price <= 3000 AND stock = 0;",
                ".exit",
            ],
        )
        require(dropped, "PLAN: GENERIC SECONDARY INDEX FUSED RANGE")
        require(dropped, "INDEX: idx_products_price")
        require(dropped, "RANGE TERMS: 2 on price")
        if "PLAN: GENERIC SECONDARY INDEX INTERSECTION" in dropped:
            raise AssertionError("intersection remained active after dropping one source index\n" + dropped)
        if scalar_results(dropped) != [1]:
            raise AssertionError("DROP INDEX fallback changed query result\n" + dropped)
        if os.path.exists(stock_range):
            raise AssertionError("DROP INDEX left stock typed range snapshot behind")

        print(
            "PASS: multi-index AND intersection fuses all ordered bounds per source, "
            "uses fused cardinalities for the smallest-base intersection, short-circuits "
            "empty source intervals, revalidates LIKE residuals, rebuilds after mutation, "
            "survives reopen, and falls back safely after DROP INDEX."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
