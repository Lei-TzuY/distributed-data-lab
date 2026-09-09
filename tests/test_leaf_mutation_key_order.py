import glob
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


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe_source = os.path.join(repo_root, "tests", "leaf_mutation_key_order_probe.c")
    core_library = find_core_library(repo_root)
    if not os.path.exists(probe_source):
        raise AssertionError("missing leaf_mutation_key_order_probe.c")
    if core_library is None:
        raise AssertionError("tinydb_core static library was not produced by the main build")

    with tempfile.TemporaryDirectory(prefix="tinydb-leaf-key-order-") as temp_dir:
        cmake_lists = os.path.join(temp_dir, "CMakeLists.txt")
        src_dir = os.path.join(repo_root, "src").replace("\\", "/")
        probe_cmake = probe_source.replace("\\", "/")
        core_cmake = core_library.replace("\\", "/")
        with open(cmake_lists, "w", encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(tinydb_leaf_mutation_key_order_probe C)\n"
                "set(CMAKE_C_STANDARD 99)\n"
                "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
                "if(MSVC)\n"
                "  add_compile_options(/W4 /WX /utf-8)\n"
                "else()\n"
                "  add_compile_options(-Wall -Wextra -Werror)\n"
                "endif()\n"
                "add_library(tinydb_core STATIC IMPORTED)\n"
                f'set_target_properties(tinydb_core PROPERTIES IMPORTED_LOCATION "{core_cmake}")\n'
                f'add_executable(leaf_key_order_probe "{probe_cmake}")\n'
                f'target_include_directories(leaf_key_order_probe PRIVATE "{src_dir}")\n'
                "target_link_libraries(leaf_key_order_probe PRIVATE tinydb_core)\n"
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
            os.path.join(build_dir, "Debug", "leaf_key_order_probe.exe"),
            os.path.join(build_dir, "Release", "leaf_key_order_probe.exe"),
            os.path.join(build_dir, "leaf_key_order_probe.exe"),
            os.path.join(build_dir, "leaf_key_order_probe"),
        ]
        executable = next((path for path in candidates if os.path.exists(path)), None)
        if executable is None:
            raise AssertionError("leaf mutation key-order probe executable was not produced")

        db_path = os.path.join(temp_dir, "leaf_key_order.db")
        cleanup(db_path)
        result = subprocess.run(
            [executable, db_path],
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
            "LEAF_MUTATION_KEY_ORDER_OK",
            "sorted=yes",
            "interior_disorder=yes",
            "duplicate=yes",
            "unchanged=yes",
        ):
            if marker not in output:
                raise AssertionError(f"missing {marker}\n{output}")

    print(
        "PASS: fixed-V1 mutation eligibility validates every key inside each leaf, "
        "rejecting interior disorder and duplicate keys without modifying pages"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
