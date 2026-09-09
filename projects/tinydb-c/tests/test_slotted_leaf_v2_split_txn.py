import os
import shutil
import subprocess
import sys
import tempfile


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe_source = os.path.join(repo_root, "tests", "slotted_leaf_v2_split_txn_probe.c")
    codec_source = os.path.join(repo_root, "src", "slotted_leaf_v2.c")
    src_dir = os.path.join(repo_root, "src")

    with tempfile.TemporaryDirectory(prefix="tinydb-v2-split-txn-") as temp_dir:
        cmake_file = os.path.join(temp_dir, "CMakeLists.txt")
        with open(cmake_file, "w", encoding="utf-8") as handle:
            handle.write(
                "cmake_minimum_required(VERSION 3.16)\n"
                "project(tinydb_v2_split_txn C)\n"
                "set(CMAKE_C_STANDARD 11)\n"
                "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
                "add_executable(split_txn_probe\n"
                f"  \"{probe_source.replace(os.sep, '/')}\"\n"
                f"  \"{codec_source.replace(os.sep, '/')}\"\n"
                ")\n"
                f"target_include_directories(split_txn_probe PRIVATE \"{src_dir.replace(os.sep, '/')}\")\n"
                "if(MSVC)\n"
                "  target_compile_options(split_txn_probe PRIVATE /W4 /WX)\n"
                "else()\n"
                "  target_compile_options(split_txn_probe PRIVATE -Wall -Wextra -Werror)\n"
                "endif()\n"
            )

        build_dir = os.path.join(temp_dir, "build")
        configure = subprocess.run(
            ["cmake", "-S", temp_dir, "-B", build_dir],
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
            os.path.join(build_dir, "Debug", "split_txn_probe.exe"),
            os.path.join(build_dir, "Release", "split_txn_probe.exe"),
            os.path.join(build_dir, "split_txn_probe.exe"),
            os.path.join(build_dir, "split_txn_probe"),
        ]
        executable = next((path for path in candidates if os.path.exists(path)), None)
        if executable is None:
            raise AssertionError("split_txn_probe executable was not produced")

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
            "SLOTTED_SPLIT_TXN_OK",
            "backlink_repair=yes",
            "atomic_failure=yes",
            "checksum_reserved=yes",
        ):
            if marker not in output:
                raise AssertionError(f"missing {marker}\n{output}")

    print("PASS: V2 split repairs sibling backlinks atomically")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
