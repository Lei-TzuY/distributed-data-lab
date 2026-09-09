import os
import subprocess
import sys
import tempfile


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    source = os.path.join(repo_root, "tests", "slotted_v2_parent_max_stage_probe.c")
    if not os.path.exists(source):
        raise AssertionError("missing slotted_v2_parent_max_stage_probe.c")

    with tempfile.TemporaryDirectory(prefix="tinydb-v2-parent-max-") as temp_dir:
        source_cmake = source.replace("\\", "/")
        include_cmake = os.path.join(repo_root, "src").replace("\\", "/")
        with open(os.path.join(temp_dir, "CMakeLists.txt"), "w", encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(tinydb_v2_parent_max_stage_probe C)\n"
                "set(CMAKE_C_STANDARD 99)\n"
                "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
                "if(MSVC)\n"
                "  add_compile_options(/W4 /WX /utf-8)\n"
                "else()\n"
                "  add_compile_options(-Wall -Wextra -Werror)\n"
                "endif()\n"
                f'add_executable(parent_max_stage_probe "{source_cmake}")\n'
                f'target_include_directories(parent_max_stage_probe PRIVATE "{include_cmake}")\n'
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
            os.path.join(build_dir, "Debug", "parent_max_stage_probe.exe"),
            os.path.join(build_dir, "Release", "parent_max_stage_probe.exe"),
            os.path.join(build_dir, "parent_max_stage_probe.exe"),
            os.path.join(build_dir, "parent_max_stage_probe"),
        ]
        executable = next((path for path in candidates if os.path.exists(path)), None)
        if executable is None:
            raise AssertionError("parent-max stage probe executable was not produced")

        result = subprocess.run(
            [executable],
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
            "SLOTTED_V2_PARENT_MAX_STAGE_OK",
            "interior=yes",
            "rightmost_noop=yes",
            "atomic_failure=yes",
            "checksum_reserved=yes",
        ):
            if marker not in output:
                raise AssertionError(f"missing {marker}\n{output}")

    print(
        "PASS: lowering a non-rightmost V2 child maximum updates exactly one "
        "parent separator atomically, while a rightmost child remains a local "
        "no-op and checksum trailers stay Pager-owned"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
