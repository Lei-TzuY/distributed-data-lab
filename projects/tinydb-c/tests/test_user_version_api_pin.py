import glob
import os
import subprocess
import sys


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


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_binary(repo_root, "tinydb_user_version_api_pin_probe")
    db_path = os.path.join(os.path.dirname(__file__), "test_user_version_api_pin.db")
    cleanup(db_path)

    with open(os.path.join(repo_root, "src", "user_version.c"), encoding="utf-8") as handle:
        source = handle.read()
    get_body, set_body = source.split("bool db_try_set_user_version", maxsplit=1)
    assert "pager_page_handle_acquire_read(&root_handle)" in get_body
    assert "pager_page_handle_release_read(&root_handle)" in get_body
    release_error = set_body.split(
        "if (!pager_page_handle_release_write(&root_handle))", maxsplit=1
    )[1].split("}", maxsplit=1)[0]
    assert "pager_release_page_handle(&root_handle)" in release_error

    try:
        result = subprocess.run(
            [probe, db_path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        output = result.stdout + "\n" + result.stderr
        assert result.returncode == 0, output
        for marker in (
            "USER_VERSION_API_PIN_OK",
            "busy_nonfatal=yes",
            "zero_on_failure=yes",
            "one_free_frame_success=yes",
            "value_preserved=yes",
            "optional_message=yes",
            "set_busy_nonfatal=yes",
            "set_no_mutation_on_busy=yes",
            "set_one_free_frame_success=yes",
            "set_dirty=yes",
            "set_optional_message=yes",
            "set_persisted=yes",
            "checkpoint_busy_nonfatal=yes",
            "checkpoint_no_flush_on_busy=yes",
            "checkpoint_dirty_spill=yes",
            "checkpoint_one_free_frame_success=yes",
            "checkpoint_optional_message=yes",
            "checkpoint_persisted=yes",
        ):
            assert marker in result.stdout, output

        print(
            "PASS: linked user_version try-get/try-set and Pager try-checkpoint "
            "APIs return non-fatal buffer-pool backpressure, preserve a nonresident "
            "dirty-spill root without flushing on BUSY, recover with exactly one "
            "free frame, support omitted diagnostics, and persist successful values"
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
