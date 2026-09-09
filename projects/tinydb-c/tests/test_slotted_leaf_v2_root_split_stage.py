import os
import subprocess
import sys
import tempfile


def find_core_library(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_core.lib"),
        os.path.join(repo_root, "build", "Release", "tinydb_core.lib"),
        os.path.join(repo_root, "build", "tinydb_core.lib"),
        os.path.join(repo_root, "build", "libtinydb_core.a"),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    source = os.path.join(repo_root, "tests", "slotted_leaf_v2_root_split_stage_probe.c")
    core_library = find_core_library(repo_root)
    if not os.path.exists(source):
        raise AssertionError("missing slotted_leaf_v2_root_split_stage_probe.c")
    if core_library is None:
        raise AssertionError("could not find built tinydb_core static library")

    with tempfile.TemporaryDirectory(prefix="tinydb-v2-root-stage-") as temp_dir:
        source_cmake = source.replace("\\", "/")
        include_cmake = os.path.join(repo_root, "src").replace("\\", "/")
        library_cmake = core_library.replace("\\", "/")
        with open(os.path.join(temp_dir, "CMakeLists.txt"), "w", encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(tinydb_v2_root_split_stage_probe C)\n"
                "set(CMAKE_C_STANDARD 99)\n"
                "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
                "if(MSVC)\n"
                "  add_compile_options(/W4 /WX /utf-8)\n"
                "  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n"
                "else()\n"
                "  add_compile_options(-Wall -Wextra -Werror)\n"
                "endif()\n"
                "add_library(tinydb_core STATIC IMPORTED GLOBAL)\n"
                f'set_target_properties(tinydb_core PROPERTIES IMPORTED_LOCATION "{library_cmake}")\n'
                f'add_executable(root_stage_probe "{source_cmake}")\n'
                f'target_include_directories(root_stage_probe PRIVATE "{include_cmake}")\n'
                "target_link_libraries(root_stage_probe PRIVATE tinydb_core)\n"
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
            timeout=90,
        )
        if build.returncode != 0:
            raise AssertionError(build.stdout + build.stderr)

        candidates = [
            os.path.join(build_dir, "Debug", "root_stage_probe.exe"),
            os.path.join(build_dir, "Release", "root_stage_probe.exe"),
            os.path.join(build_dir, "root_stage_probe.exe"),
            os.path.join(build_dir, "root_stage_probe"),
        ]
        executable = next((path for path in candidates if os.path.exists(path)), None)
        if executable is None:
            raise AssertionError("V2 root split stage probe executable was not produced")

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
            "SLOTTED_V2_ROOT_SPLIT_STAGE_OK",
            "root0=yes",
            "topology=yes",
            "atomic_failure=yes",
            "checksum_reserved=yes",
        ):
            if marker not in output:
                raise AssertionError(f"missing {marker}\n{output}")

    print(
        "PASS: a V2 root leaf can atomically become an internal root with two "
        "linked non-root V2 children while preserving the stable root page and "
        "Pager-owned checksum trailers"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
