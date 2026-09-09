import glob
import os
import subprocess
import sys


def find_probe(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb_generic_record_probe.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb_generic_record_probe.exe"),
        os.path.join(base_dir, "build", "tinydb_generic_record_probe.exe"),
        os.path.join(base_dir, "build", "tinydb_generic_record_probe"),
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


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), "..")
    probe = find_probe(base_dir)
    if probe is None:
        print("FAIL: Could not find tinydb_generic_record_probe.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_generic_record.db")
    cleanup(db_file)

    result = subprocess.run(
        [probe, db_file],
        capture_output=True,
        text=True,
        timeout=120,
    )

    if result.returncode != 0:
        print("FAIL: generic record probe returned", result.returncode)
        print(result.stdout)
        print(result.stderr)
        cleanup(db_file)
        sys.exit(1)

    output = result.stdout + result.stderr
    required = [
        "GENERIC_LAYOUT products_row_size=264 price_offset=260 orders_row_size=12 quantity_offset=8",
        "GENERIC_RECORD_OK products=30 orders=20 reopen=yes",
        "ok",
    ]
    for marker in required:
        if marker not in output:
            print(f"FAIL: missing generic record marker: {marker}")
            print(output)
            cleanup(db_file)
            sys.exit(1)

    cleanup(db_file)
    print("PASS: schema-aware serialized record layouts persist across B+ tree splits and reopen.")


if __name__ == "__main__":
    run_test()
