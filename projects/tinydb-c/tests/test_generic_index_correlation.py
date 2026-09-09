import glob
import os
import re
import subprocess
import sys


CORR_MAGIC = b"1CIG"  # little-endian 0x47494331
CORR_VERSION = 1


def find_tinydb(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find tinydb executable")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_path, commands):
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
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


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def require_corr_v1(path):
    if not os.path.exists(path):
        raise AssertionError("pairwise correlation statistics were not materialized: " + path)
    raw = read_bytes(path)
    if len(raw) < 8:
        raise AssertionError("pairwise correlation statistics are truncated")
    if raw[:4] != CORR_MAGIC:
        raise AssertionError(f"unexpected pairwise correlation magic: {raw[:4]!r}")
    version = int.from_bytes(raw[4:8], "little")
    if version != CORR_VERSION:
        raise AssertionError(f"unexpected pairwise correlation version: {version}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_generic_index_correlation.db")
    cleanup(db_path)

    country_index = "idx_accounts_country"
    currency_index = "idx_accounts_currency"
    corr_path = db_path + f".{country_index}.{currency_index}.corr.stats"

    try:
        setup = [
            "CREATE TABLE accounts (id INT, country VARCHAR(2), currency VARCHAR(3), tier INT);",
        ]
        for row_id in range(1, 101):
            if row_id <= 60:
                country, currency, tier = "TW", "TWD", 1
            elif row_id <= 80:
                country, currency, tier = "US", "USD", 2
            else:
                country, currency, tier = "JP", "JPY", 3
            setup.append(
                f"INSERT INTO accounts VALUES ({row_id}, '{country}', '{currency}', {tier});"
            )
        setup.extend(
            [
                f"CREATE INDEX {country_index} ON accounts(country);",
                f"CREATE INDEX {currency_index} ON accounts(currency);",
                ".exit",
            ]
        )
        created = run_session(executable, db_path, setup)
        if "Error:" in created or "Syntax error" in created:
            raise AssertionError(created)
        if os.path.exists(corr_path):
            raise AssertionError("CREATE INDEX unexpectedly materialized pairwise statistics")

        planned = run_session(
            executable,
            db_path,
            [
                "EXPLAIN SELECT id FROM accounts WHERE country = 'TW' AND currency = 'TWD';",
                "SELECT COUNT(*) FROM accounts WHERE country = 'TW' AND currency = 'TWD';",
                "EXPLAIN SELECT id FROM accounts WHERE country = 'TW' AND currency = 'USD';",
                "SELECT COUNT(*) FROM accounts WHERE country = 'TW' AND currency = 'USD';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if "Error:" in planned or "Syntax error" in planned:
            raise AssertionError(planned)

        # Unary selectivities are 60/100 each. Independence would predict 36 rows and
        # make intersection look cheaper (264) than either a 300-cost anchor or scan.
        # The exact pair synopsis observes 60 joint rows, making intersection cost 360,
        # so the planner must downgrade to one indexed anchor plus residual validation.
        require(planned, "PLAN: GENERIC SECONDARY INDEX + RESIDUAL FILTER")
        require(planned, "CORRELATION ESTIMATE: 60 rows (pairwise equality synopsis)")

        # TW/USD has no overlap. Correlation therefore makes the true zero-row
        # intersection especially cheap, so the multi-index path should remain selected.
        require(planned, "PLAN: GENERIC SECONDARY INDEX INTERSECTION")
        require(planned, "CORRELATION ESTIMATE: 0 rows (pairwise equality synopsis)")
        require(planned, "ESTIMATED ROWS: 0 / 100")
        if scalar_results(planned) != [60, 0]:
            raise AssertionError("correlation-aware planning changed SELECT semantics\n" + planned)
        require(planned, "ok")
        require_corr_v1(corr_path)

        persisted_before = read_bytes(corr_path)
        reopened = run_session(
            executable,
            db_path,
            [
                "EXPLAIN SELECT id FROM accounts WHERE currency = 'TWD' AND country = 'TW';",
                "SELECT COUNT(*) FROM accounts WHERE currency = 'TWD' AND country = 'TW';",
                ".exit",
            ],
        )
        require(reopened, "CORRELATION ESTIMATE: 60 rows (pairwise equality synopsis)")
        if scalar_results(reopened) != [60]:
            raise AssertionError("persisted pair statistics changed semantics after reopen\n" + reopened)
        if read_bytes(corr_path) != persisted_before:
            raise AssertionError("clean reopen unexpectedly rewrote valid pair statistics")

        # Any indexed-table mutation advances the durable generic-index epoch. The stale
        # pair sidecar must not be trusted; the next correlated plan rebuilds it from rows.
        mutated = run_session(
            executable,
            db_path,
            [
                "UPDATE accounts SET country = 'US' WHERE id = 1;",
                "EXPLAIN SELECT id FROM accounts WHERE country = 'TW' AND currency = 'TWD';",
                "SELECT COUNT(*) FROM accounts WHERE country = 'TW' AND currency = 'TWD';",
                ".exit",
            ],
        )
        require(mutated, "CORRELATION ESTIMATE: 59 rows (pairwise equality synopsis)")
        if scalar_results(mutated) != [59]:
            raise AssertionError("epoch-stale correlation statistics were trusted after mutation\n" + mutated)
        require_corr_v1(corr_path)
        rebuilt = read_bytes(corr_path)
        if rebuilt == persisted_before:
            raise AssertionError("mutation did not rebuild epoch-stale pair statistics")

        # Corrupt the persisted checksum/payload. Planning must fail closed to a fresh
        # table scan of pair values, regenerate a valid sidecar, and preserve results.
        with open(corr_path, "r+b") as handle:
            handle.seek(0)
            first = handle.read(1)
            handle.seek(0)
            handle.write(bytes([first[0] ^ 0x5A]))

        corrupted = run_session(
            executable,
            db_path,
            [
                "EXPLAIN SELECT id FROM accounts WHERE country = 'TW' AND currency = 'TWD';",
                "SELECT COUNT(*) FROM accounts WHERE country = 'TW' AND currency = 'TWD';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(corrupted, "CORRELATION ESTIMATE: 59 rows (pairwise equality synopsis)")
        if scalar_results(corrupted) != [59]:
            raise AssertionError("corrupt correlation sidecar affected query semantics\n" + corrupted)
        require(corrupted, "ok")
        require_corr_v1(corr_path)

        # DROP INDEX must remove correlation sidecars involving that index.
        dropped = run_session(
            executable,
            db_path,
            [f"DROP INDEX {currency_index};", ".exit"],
        )
        if "Error:" in dropped or "Syntax error" in dropped:
            raise AssertionError(dropped)
        if os.path.exists(corr_path):
            raise AssertionError("DROP INDEX left a pairwise correlation sidecar behind")

        print(
            "PASS: pairwise equality correlation statistics correct independence-error "
            "costing, persist across reopen, rebuild on epoch changes/corruption, preserve "
            "query semantics, and are removed with their indexes."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
