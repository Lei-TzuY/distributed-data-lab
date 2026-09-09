import glob
import json
import os
import shutil
import subprocess
import tempfile


def benchmark_executable(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "tinydb_bench"),
        os.path.join(repo_root, "build", "Debug", "tinydb_bench.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_bench.exe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("tinydb_bench executable was not built")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = benchmark_executable(repo_root)
    temp_dir = tempfile.mkdtemp(prefix="tinydb-bench-test-")
    db_path = os.path.join(temp_dir, "smoke.db")
    json_db_path = os.path.join(temp_dir, "json.db")

    try:
        result = subprocess.run(
            [exe, db_path, "200", "500"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        assert result.returncode == 0, result.stdout + "\n" + result.stderr
        assert "inserted=200" in result.stdout, result.stdout
        assert "lookup_hits=500" in result.stdout, result.stdout
        assert "rows=200" in result.stdout, result.stdout
        assert "metadata_capacity=" in result.stdout, result.stdout
        assert "BENCHMARK_OK" in result.stdout, result.stdout

        json_result = subprocess.run(
            [exe, json_db_path, "50", "100", "--json"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        assert json_result.returncode == 0, json_result.stdout + "\n" + json_result.stderr
        payload = json.loads(json_result.stdout.strip())
        assert payload["rows"] == 50, payload
        assert payload["lookups"] == 100, payload
        assert payload["lookup_hits"] == 100, payload
        assert payload["ok"] is True, payload
        assert payload["dynamic_page_table"] is True, payload
        assert payload["initial_metadata_capacity"] == 64, payload
        assert payload["metadata_capacity"] >= payload["pages"], payload
        assert payload["legacy_page_ceiling"] == 4096, payload
        assert payload["pages"] >= 1, payload
        assert payload["leaf_pages"] >= 1, payload
        assert payload["cache_hits"] + payload["cache_misses"] >= 100, payload
        assert payload["rows_per_sec"] >= 0.0, payload
        assert payload["lookups_per_sec"] >= 0.0, payload

        # The benchmark intentionally refuses to overwrite an existing DB.
        second = subprocess.run(
            [exe, db_path, "10", "10"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert second.returncode == 2, second.stdout + "\n" + second.stderr
        assert "already exists" in second.stderr, second.stderr
    finally:
        for path in glob.glob(db_path + "*"):
            try:
                os.remove(path)
            except OSError:
                pass
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
