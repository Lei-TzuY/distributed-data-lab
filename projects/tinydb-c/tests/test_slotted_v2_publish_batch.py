import os
import subprocess
import sys
import tempfile


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe_source = os.path.join(repo_root, "tests", "slotted_v2_publish_batch_probe.c")
    if not os.path.exists(probe_source):
        raise AssertionError("missing slotted_v2_publish_batch_probe.c")

    with tempfile.TemporaryDirectory(prefix="tinydb-v2-publish-") as temp_dir:
        cmake_lists = os.path.join(temp_dir, "CMakeLists.txt")
        src_dir = os.path.join(repo_root, "src").replace("\\", "/")
        probe_cmake = probe_source.replace("\\", "/")
        with open(cmake_lists, "w", encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(tinydb_slotted_v2_publish_batch_probe C)\n"
                "set(CMAKE_C_STANDARD 99)\n"
                "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
                "if(MSVC)\n"
                "  add_compile_options(/W4 /WX /utf-8)\n"
                "else()\n"
                "  add_compile_options(-Wall -Wextra -Werror)\n"
                "endif()\n"
                f'add_executable(v2_publish_probe "{probe_cmake}")\n'
                f'target_include_directories(v2_publish_probe PRIVATE "{src_dir}")\n'
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
            os.path.join(build_dir, "Debug", "v2_publish_probe.exe"),
            os.path.join(build_dir, "Release", "v2_publish_probe.exe"),
            os.path.join(build_dir, "v2_publish_probe.exe"),
            os.path.join(build_dir, "v2_publish_probe"),
        ]
        executable = next((path for path in candidates if os.path.exists(path)), None)
        if executable is None:
            raise AssertionError("slotted V2 publish probe executable was not produced")

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
            "SLOTTED_V2_PUBLISH_BATCH_OK",
            "atomic=yes",
            "rollback=yes",
            "checksum_isolated=yes",
            "duplicate_guard=yes",
            "root0=yes",
            "deep_batch=yes",
            "beyond_legacy_limit=yes",
            "beyond_previous_limit=yes",
            "capacity_guard=yes",
            "budget_guard=yes",
        ):
            if marker not in output:
                raise AssertionError(f"missing {marker}\n{output}")

    print(
        "PASS: staged slotted V2 page batches prevalidate topology identities, "
        "restore every caller page after deterministic partial publication, "
        "support page-zero roots and 64-page recursive publication batches, "
        "roll back after crossing the former 32-page ceiling, enforce a "
        "configurable rollback-byte budget, and leave checksum trailers untouched"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
