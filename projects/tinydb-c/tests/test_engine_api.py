import glob
import os
import subprocess
import sys
import tempfile


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_engine_api_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_engine_api_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_engine_api_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_engine_api_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find tinydb_engine_api_probe")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)

    with tempfile.TemporaryDirectory(prefix="tinydb-engine-api-") as temp_dir:
        db_path = os.path.join(temp_dir, "engine.db")
        result = subprocess.run(
            [probe, db_path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        assert result.returncode == 0, result.stdout + "\n" + result.stderr
        assert "ENGINE_API_OK" in result.stdout, result.stdout
        assert "WIDE_SCHEMA_REOPEN_OK" in result.stdout, result.stdout
        assert "REAL_REPACK_SCAN_OK" in result.stdout, result.stdout
        assert "source_rows=120" in result.stdout, result.stdout
        assert "row_size=388" in result.stdout, result.stdout
        assert "row_size=516" in result.stdout, result.stdout
        assert "title_size=256" in result.stdout, result.stdout
        assert "body_size=256" in result.stdout, result.stdout
        assert "POST_RECOVERY_LIVE_PAGE_VALIDATION_OK" in result.stdout, result.stdout
        assert "archive_rows=80" in result.stdout, result.stdout
        cleanup(db_path)

    print("PASS: reusable tinydb_core engine facade, real authoritative schema-repack scan, multi-leaf wide-schema reopen, and post-recovery live-page validation verified")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
