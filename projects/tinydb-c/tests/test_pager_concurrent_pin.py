import glob
import os
import subprocess
import sys
import tempfile


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe_source = os.path.join(repo_root, "tests", "pager_concurrent_pin_probe.c")
    pager_source = os.path.join(repo_root, "src", "pager.c")
    checkpoint_source = os.path.join(repo_root, "src", "pager_checkpoint_order.c")
    src_dir = os.path.join(repo_root, "src")

    with tempfile.TemporaryDirectory(prefix="tinydb-concurrent-pin-") as temp_dir:
        cmake_lists = os.path.join(temp_dir, "CMakeLists.txt")
        paths = {
            "probe": probe_source.replace("\\", "/"),
            "pager": pager_source.replace("\\", "/"),
            "checkpoint": checkpoint_source.replace("\\", "/"),
            "src": src_dir.replace("\\", "/"),
        }
        with open(cmake_lists, "w", encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(tinydb_pager_concurrent_pin_probe C)\n"
                "set(CMAKE_C_STANDARD 99)\n"
                "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
                "if(MSVC)\n"
                "  add_compile_options(/W4 /WX /utf-8)\n"
                "  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n"
                "else()\n"
                "  add_compile_options(-Wall -Wextra -Werror)\n"
                "endif()\n"
                "find_package(Threads REQUIRED)\n"
                f'add_executable(pager_concurrent_pin_probe "{paths["probe"]}" '
                f'"{paths["pager"]}" "{paths["checkpoint"]}")\n'
                f'set_source_files_properties("{paths["pager"]}" PROPERTIES '
                'COMPILE_DEFINITIONS "pager_checkpoint=pager_checkpoint_legacy_base")\n'
                f'target_include_directories(pager_concurrent_pin_probe PRIVATE "{paths["src"]}")\n'
                "target_link_libraries(pager_concurrent_pin_probe PRIVATE Threads::Threads)\n"
            )

        build_dir = os.path.join(temp_dir, "build")
        configure = subprocess.run(
            ["cmake", "-S", temp_dir, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Debug"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=60,
        )
        if configure.returncode != 0:
            raise AssertionError(configure.stdout + configure.stderr)

        build = subprocess.run(
            ["cmake", "--build", build_dir, "--config", "Debug"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        if build.returncode != 0:
            raise AssertionError(build.stdout + build.stderr)

        candidates = [
            os.path.join(build_dir, "Debug", "pager_concurrent_pin_probe.exe"),
            os.path.join(build_dir, "Release", "pager_concurrent_pin_probe.exe"),
            os.path.join(build_dir, "pager_concurrent_pin_probe.exe"),
            os.path.join(build_dir, "pager_concurrent_pin_probe"),
        ]
        executable = next((path for path in candidates if os.path.exists(path)), None)
        if executable is None:
            raise AssertionError("concurrent pin probe executable was not produced")

        db_path = os.path.join(temp_dir, "concurrent-pin.db")
        result = subprocess.run(
            [executable, db_path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=90,
        )
        output = result.stdout + result.stderr
        if result.returncode != 0:
            raise AssertionError(output)
        for marker in (
            "CONCURRENT_PIN_LRU_OK",
            "handle_guard=yes",
            "legacy_lock_guard=yes",
            "worker_pins=yes",
            "post_release_evict=yes",
            "try_close=yes",
        ):
            if marker not in output:
                raise AssertionError(f"missing {marker}\n{output}")

        for path in glob.glob(db_path + "*"):
            try:
                os.remove(path)
            except OSError:
                pass

    print(
        "PASS: shared-Pager worker threads churn the LRU while explicit handles and "
        "legacy page locks retain frame identity, then release restores evictability"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
