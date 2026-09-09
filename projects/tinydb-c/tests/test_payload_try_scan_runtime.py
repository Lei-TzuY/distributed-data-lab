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
    source = os.path.join(repo_root, "tests", "payload_try_scan_probe.c")
    core_library = find_core_library(repo_root)
    if core_library is None:
        raise AssertionError("could not find built tinydb_core static library")

    with tempfile.TemporaryDirectory(prefix="tinydb-payload-try-scan-") as temp_dir:
        source_cmake = source.replace("\\", "/")
        include_cmake = os.path.join(repo_root, "src").replace("\\", "/")
        library_cmake = core_library.replace("\\", "/")
        with open(os.path.join(temp_dir, "CMakeLists.txt"), "w", encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(tinydb_payload_try_scan_probe C)\n"
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
                f'add_executable(payload_try_scan_probe "{source_cmake}")\n'
                f'target_include_directories(payload_try_scan_probe PRIVATE "{include_cmake}")\n'
                "target_link_libraries(payload_try_scan_probe PRIVATE tinydb_core)\n"
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
            os.path.join(build_dir, "Debug", "payload_try_scan_probe.exe"),
            os.path.join(build_dir, "Release", "payload_try_scan_probe.exe"),
            os.path.join(build_dir, "payload_try_scan_probe.exe"),
            os.path.join(build_dir, "payload_try_scan_probe"),
        ]
        executable = next((path for path in candidates if os.path.exists(path)), None)
        if executable is None:
            raise AssertionError("payload try-scan probe executable was not produced")

        db_path = os.path.join(temp_dir, "payload-try-scan.db")
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

        required = [
            "PAYLOAD_TRY_SCAN_OK",
            "rows=40",
            "multileaf=yes",
            "range=20",
            "early_stop=yes",
            "busy_nonfatal=yes",
            "busy_zero_callback=yes",
            "one_free_frame_full=yes",
            "one_free_frame_range=yes",
            "optional_message=yes",
        ]
        missing = [marker for marker in required if marker not in output]
        if missing:
            raise AssertionError(f"missing markers {missing}:\n{output}")

    print(
        "PASS: non-fatal payload full/range scans traverse a real multi-leaf "
        "V2 tree, fail closed under 16/16 pin pressure, and complete with one "
        "replaceable frame"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
