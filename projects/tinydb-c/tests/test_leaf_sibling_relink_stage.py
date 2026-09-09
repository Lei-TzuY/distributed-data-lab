import os
import subprocess
import sys
import tempfile


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = os.path.join(repo_root, "tests", "leaf_sibling_relink_stage_probe.c")
    sources = [
        probe,
        os.path.join(repo_root, "src", "leaf_format.c"),
        os.path.join(repo_root, "src", "leaf_page_access.c"),
        os.path.join(repo_root, "src", "slotted_leaf_v2.c"),
    ]
    for source in sources:
        if not os.path.exists(source):
            raise AssertionError(f"missing source: {source}")

    with tempfile.TemporaryDirectory(prefix="tinydb-leaf-relink-") as temp_dir:
        include_dir = os.path.join(repo_root, "src").replace("\\", "/")
        source_args = " ".join(
            f'"{source.replace(chr(92), "/")}"' for source in sources
        )
        with open(os.path.join(temp_dir, "CMakeLists.txt"), "w", encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(tinydb_leaf_sibling_relink_stage_probe C)\n"
                "set(CMAKE_C_STANDARD 99)\n"
                "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
                "if(MSVC)\n"
                "  add_compile_options(/W4 /WX /utf-8)\n"
                "else()\n"
                "  add_compile_options(-Wall -Wextra -Werror)\n"
                "endif()\n"
                f"add_executable(leaf_relink_probe {source_args})\n"
                f'target_include_directories(leaf_relink_probe PRIVATE "{include_dir}")\n'
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
            os.path.join(build_dir, "Debug", "leaf_relink_probe.exe"),
            os.path.join(build_dir, "Release", "leaf_relink_probe.exe"),
            os.path.join(build_dir, "leaf_relink_probe.exe"),
            os.path.join(build_dir, "leaf_relink_probe"),
        ]
        executable = next((path for path in candidates if os.path.exists(path)), None)
        if executable is None:
            raise AssertionError("leaf sibling relink probe executable was not produced")

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
            "LEAF_SIBLING_RELINK_STAGE_OK",
            "v2=yes",
            "v1=yes",
            "boundary=yes",
            "atomic_failure=yes",
            "checksum_reserved=yes",
        ):
            if marker not in output:
                raise AssertionError(f"missing {marker}\n{output}")

    print(
        "PASS: V1/V2 leaf sibling pointers can be staged with stale-link guards, "
        "boundary-zero support, atomic failure, and checksum-trailer isolation"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
