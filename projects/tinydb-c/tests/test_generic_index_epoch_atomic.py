import os
import subprocess
import sys
import tempfile


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe_source = os.path.join(repo_root, "tests", "generic_index_epoch_atomic_probe.c")
    if not os.path.exists(probe_source):
        raise AssertionError("missing generic_index_epoch_atomic_probe.c")

    with tempfile.TemporaryDirectory(prefix="tinydb-index-epoch-") as temp_dir:
        cmake_lists = os.path.join(temp_dir, "CMakeLists.txt")
        src_dir = os.path.join(repo_root, "src").replace("\\", "/")
        probe_cmake = probe_source.replace("\\", "/")
        with open(cmake_lists, "w", encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(tinydb_generic_index_epoch_atomic_probe C)\n"
                "set(CMAKE_C_STANDARD 99)\n"
                "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
                "if(MSVC)\n"
                "  add_compile_options(/W4 /WX /utf-8)\n"
                "else()\n"
                "  add_compile_options(-Wall -Wextra -Werror)\n"
                "endif()\n"
                f'add_executable(index_epoch_probe "{probe_cmake}")\n'
                f'target_include_directories(index_epoch_probe PRIVATE "{src_dir}")\n'
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
            timeout=60,
        )
        if build.returncode != 0:
            raise AssertionError(build.stdout + build.stderr)

        candidates = [
            os.path.join(build_dir, "Debug", "index_epoch_probe.exe"),
            os.path.join(build_dir, "Release", "index_epoch_probe.exe"),
            os.path.join(build_dir, "index_epoch_probe.exe"),
            os.path.join(build_dir, "index_epoch_probe"),
        ]
        executable = next((path for path in candidates if os.path.exists(path)), None)
        if executable is None:
            raise AssertionError("generic index epoch atomic probe executable was not produced")

        result = subprocess.run(
            [executable],
            cwd=temp_dir,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=30,
        )
        output = result.stdout + result.stderr
        if result.returncode != 0:
            raise AssertionError(output)
        for marker in (
            "GENERIC_INDEX_EPOCH_ATOMIC_OK",
            "replace=yes",
            "failure_preserves_old=yes",
            "temp_cleanup=yes",
        ):
            if marker not in output:
                raise AssertionError(f"missing {marker}\n{output}")

    print(
        "PASS: generic secondary-index epochs publish through a synced temporary "
        "file, atomically replace existing state, and preserve the prior durable "
        "epoch when replacement preparation fails"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
