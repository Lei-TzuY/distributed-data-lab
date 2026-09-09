import glob
import json
import os
import subprocess
import sys


def find_benchmark(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb_generic_bench.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb_generic_bench.exe"),
        os.path.join(base_dir, "build", "tinydb_generic_bench.exe"),
        os.path.join(base_dir, "build", "tinydb_generic_bench"),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def cleanup(path):
    for candidate in glob.glob(path + "*"):
        try:
            os.remove(candidate)
        except OSError:
            pass


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), "..")
    executable = find_benchmark(base_dir)
    if executable is None:
        print("FAIL: Could not find tinydb_generic_bench executable.")
        sys.exit(1)

    text_db = os.path.join(os.path.dirname(__file__), "test_generic_benchmark_text.db")
    json_db = os.path.join(os.path.dirname(__file__), "test_generic_benchmark_json.db")
    cleanup(text_db)
    cleanup(json_db)

    try:
        text = subprocess.run(
            [executable, text_db, "128", "256", "3"],
            capture_output=True,
            text=True,
            timeout=120,
        )
        if text.returncode != 0:
            raise AssertionError(text.stdout + "\n" + text.stderr)
        for marker in (
            "TinyDB generic record benchmark",
            "insert: inserted=128",
            "lookup: requested=256 hits=256",
            "scan: rounds=3 records=384 matches=6",
            "layout: schema_row_size=264 slot_size=293",
            "GENERIC_BENCHMARK_OK",
        ):
            if marker not in text.stdout:
                raise AssertionError(f"missing text marker {marker!r}\n{text.stdout}")

        machine = subprocess.run(
            [executable, json_db, "128", "256", "3", "--json"],
            capture_output=True,
            text=True,
            timeout=120,
        )
        if machine.returncode != 0:
            raise AssertionError(machine.stdout + "\n" + machine.stderr)
        payload = json.loads(machine.stdout.strip())

        expected = {
            "rows": 128,
            "lookups": 256,
            "scan_rounds": 3,
            "schema_row_size": 264,
            "leaf_value_slot_size": 293,
            "lookup_hits": 256,
            "scan_records": 384,
            "scan_matches": 6,
            "ok": True,
        }
        for key, value in expected.items():
            if payload.get(key) != value:
                raise AssertionError(f"unexpected {key}: {payload.get(key)!r}, expected {value!r}")

        if not (90.0 < payload["slot_utilization_pct"] < 91.0):
            raise AssertionError("unexpected fixed-slot utilization")
        if payload["root_page"] == 0:
            raise AssertionError("generic benchmark unexpectedly used the users root")
        if payload["leaf_pages"] < 2 or payload["internal_pages"] < 1:
            raise AssertionError("benchmark did not force a generic B+ tree split")
        if payload["metadata_capacity"] < payload["root_page"] + 1:
            raise AssertionError("pager metadata capacity is smaller than the generic root")
        for rate in ("rows_per_sec", "lookups_per_sec", "scan_records_per_sec"):
            if payload[rate] <= 0:
                raise AssertionError(f"non-positive benchmark rate: {rate}")

        refusal = subprocess.run(
            [executable, json_db, "16", "16", "1", "--json"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        if refusal.returncode != 2 or "already exists" not in refusal.stderr:
            raise AssertionError("generic benchmark did not refuse an existing DB path")

        print(
            "PASS: generic benchmark validates schema layout, transactional inserts, "
            "PK lookups, schema-aware scans, JSON metrics and existing-path safety."
        )
    finally:
        cleanup(text_db)
        cleanup(json_db)


if __name__ == "__main__":
    try:
        run_test()
    except (AssertionError, json.JSONDecodeError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
