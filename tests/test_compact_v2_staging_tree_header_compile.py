import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_compact_v2_staging_tree_header_compiles_with_ci_toolchain(tmp_path):
    probe = tmp_path / "probe.c"
    probe.write_text(
        '#include "compact_v2_staging_tree.h"\n'
        "void tinydb_staging_tree_header_compile_probe(void) {}\n",
        encoding="utf-8",
    )
    cmake = tmp_path / "CMakeLists.txt"
    src = (ROOT / "src").as_posix()
    cmake.write_text(
        "cmake_minimum_required(VERSION 3.10)\n"
        "project(tinydb_staging_header_probe C)\n"
        "set(CMAKE_C_STANDARD 99)\n"
        "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
        "add_library(staging_header_probe OBJECT probe.c)\n"
        f'target_include_directories(staging_header_probe PRIVATE "{src}")\n'
        "if(MSVC)\n"
        "  target_compile_options(staging_header_probe PRIVATE /W4 /WX /utf-8)\n"
        "else()\n"
        "  target_compile_options(staging_header_probe PRIVATE -Wall -Wextra -Werror)\n"
        "endif()\n",
        encoding="utf-8",
    )
    build = tmp_path / "build"
    subprocess.run(
        ["cmake", "-S", str(tmp_path), "-B", str(build)],
        check=True,
        cwd=ROOT,
    )
    subprocess.run(
        ["cmake", "--build", str(build), "--config", "Release"],
        check=True,
        cwd=ROOT,
    )
