import glob
import os
import subprocess
import sys

PAGE_SIZE = 4096


def find_binary(repo_root, name):
    candidates = [
        os.path.join(repo_root, "build", "Debug", name + ".exe"),
        os.path.join(repo_root, "build", "Release", name + ".exe"),
        os.path.join(repo_root, "build", name + ".exe"),
        os.path.join(repo_root, "build", name),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError(f"Could not find {name}")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_repl(executable, db_path, commands):
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    repl = find_binary(repo_root, "tinydb")
    probe = find_binary(repo_root, "tinydb_page_ownership_probe")
    probe_db = os.path.join(os.path.dirname(__file__), "test_page_ownership_probe.db")
    orphan_db = os.path.join(os.path.dirname(__file__), "test_page_ownership_orphan.db")
    cleanup(probe_db)
    cleanup(orphan_db)

    try:
        direct = subprocess.run(
            [probe, probe_db],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        assert direct.returncode == 0, direct.stdout + "\n" + direct.stderr
        for marker in (
            "PAGE_OWNERSHIP_OK",
            "diagnostic_pin_pressure=yes",
            "direct_ownership_busy=yes",
            "tree_stats_busy=yes",
            "catalog_snapshot_busy=yes",
            "catalog_stats_busy=yes",
            "catalog_check_busy=yes",
            "table_check_busy=yes",
            "one_free_frame_success=yes",
            "tree_stats_one_free_frame_success=yes",
            "catalog_snapshot_one_free_frame_success=yes",
            "catalog_stats_one_free_frame_success=yes",
            "catalog_check_one_free_frame_success=yes",
            "catalog_pragmas_full_pool_success=yes",
            "page_inspect_busy=yes",
            "tree_inspect_busy=yes",
            "inspection_one_free_frame_success=yes",
            "integrity_pragma_busy=yes",
            "integrity_pragma_one_free_frame_success=yes",
            "user_version_busy=yes",
            "user_version_one_free_frame_success=yes",
        ):
            assert marker in direct.stdout, direct.stdout

        run_repl(
            repl,
            orphan_db,
            [
                "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                "INSERT INTO users VALUES (1, 'main', 'main@example.com');",
                "INSERT INTO archive VALUES (1, 'archive', 'archive@example.com');",
                ".exit",
            ],
        )

        # Add one physically allocated page that is absent from every catalog root.
        # The ownership pass should reject this page graph even though each tree on
        # its own remains structurally valid.
        with open(orphan_db, "ab") as database_file:
            database_file.write(b"\0" * PAGE_SIZE)

        output = run_repl(
            repl,
            orphan_db,
            [
                ".check all",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        needle = "allocated but unreachable from every catalog root"
        assert output.count(needle) >= 2, output
        assert "page ownership:" in output, output
        assert "all: ERROR:" in output, output

        print(
            "PASS: page ownership catches orphan/shared pages; the shared fail-closed "
            "catalog stats snapshot, aggregate stats/checks, .check all, public "
            "diagnostics, .page/.btree inspection, PRAGMA integrity_check, and "
            "PRAGMA user_version return non-fatal backpressure when fully pinned; "
            "catalog-only PRAGMA table_info and index_list remain available without "
            "a free frame; all page-backed read-only inspection paths make progress "
            "with exactly one free frame"
        )
    finally:
        cleanup(probe_db)
        cleanup(orphan_db)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
