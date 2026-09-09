import glob
import os
import re
import subprocess
import sys


def find_executable(repo_root):
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
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    return result.stdout


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    db_path = os.path.join(os.path.dirname(__file__), "test_v2_diag_inspect.db")
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE wide_diag (id INT, left_text VARCHAR, right_text VARCHAR);",
                "INSERT INTO wide_diag VALUES (1, 'left-1', 'right-1');",
                "INSERT INTO wide_diag VALUES (2, 'left-2', 'right-2');",
                "INSERT INTO wide_diag VALUES (3, 'left-3', 'right-3');",
                ".schema wide_diag",
                ".btree wide_diag",
                ".stats wide_diag",
                ".check wide_diag",
                ".exit",
            ],
        )

        root_match = re.search(r"Table: wide_diag \(root page (\d+)\)", first)
        assert root_match is not None, first
        root_page = int(root_match.group(1))
        assert "- leaf (size 3)" in first, first
        assert "Rows: 3" in first, first
        assert "Leaf Pages: 1" in first, first
        assert "Internal Pages: 0" in first, first
        assert "wide_diag: ok: root=" in first, first
        for key in (1, 2, 3):
            assert f"- {key}" in first, first

        page_output = run_session(
            executable,
            db_path,
            [
                f".page {root_page}",
                ".btree wide_diag",
                ".exit",
            ],
        )
        assert f"--- Page {root_page} Details ---" in page_output, page_output
        assert "Type: LEAF" in page_output, page_output
        assert "Leaf Format: SLOTTED_V2" in page_output, page_output
        assert "Num Cells: 3" in page_output, page_output
        assert "Keys: 1, 2, 3" in page_output, page_output
        assert "invalid leaf" not in page_output.lower(), page_output

        # Compact V2 stores short VARCHAR values densely, so a fixed row-count
        # assumption is not a stable way to exercise diagnostics after a split.
        # Grow well beyond the observed one-leaf capacity and then assert the
        # topology itself rather than relying on a historical 40-row threshold.
        target_rows = 100
        grow_commands = [
            f"INSERT INTO wide_diag VALUES ({row_id}, 'left-{row_id}', 'right-{row_id}');"
            for row_id in range(4, target_rows + 1)
        ]
        grow_commands.extend([
            ".btree wide_diag",
            ".stats wide_diag",
            ".check wide_diag",
            "PRAGMA integrity_check;",
            ".exit",
        ])
        split_output = run_session(executable, db_path, grow_commands)

        assert "- internal (size " in split_output, split_output
        assert split_output.count("- leaf (size ") >= 2, split_output
        assert f"Rows: {target_rows}" in split_output, split_output
        leaf_match = re.search(r"Leaf Pages: (\d+)", split_output)
        internal_match = re.search(r"Internal Pages: (\d+)", split_output)
        assert leaf_match is not None and int(leaf_match.group(1)) >= 2, split_output
        assert internal_match is not None and int(internal_match.group(1)) >= 1, split_output
        assert "wide_diag: ok: root=" in split_output, split_output
        assert f"rows={target_rows}" in split_output, split_output
        assert "- 1" in split_output, split_output
        assert f"- {target_rows}" in split_output, split_output
        assert "invalid leaf" not in split_output.lower(), split_output
        assert "buffer pool busy" not in split_output.lower(), split_output
        assert "\nok\n" in split_output or "db > ok\n" in split_output, split_output

        reopen_output = run_session(
            executable,
            db_path,
            [
                ".stats wide_diag",
                ".check wide_diag",
                ".btree wide_diag",
                ".exit",
            ],
        )
        assert f"Rows: {target_rows}" in reopen_output, reopen_output
        assert "wide_diag: ok: root=" in reopen_output, reopen_output
        assert f"rows={target_rows}" in reopen_output, reopen_output
        assert reopen_output.count("- leaf (size ") >= 2, reopen_output

        print("PASS: V2 slotted leaf inspection, stats, and table checks survive splits and reopen")
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
