import glob
import os
import struct
import subprocess
import sys
import tempfile


FREE_MAGIC = 0x46524545
FREE_VERSION = 1
PAGE_SIZE = 4096


def find_tinydb(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb"),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def find_core_library(base_dir):
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb_core.lib"),
        os.path.join(base_dir, "build", "Release", "tinydb_core.lib"),
        os.path.join(base_dir, "build", "tinydb_core.lib"),
        os.path.join(base_dir, "build", "libtinydb_core.a"),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_file, commands):
    result = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + "\n" + result.stderr)
    return result.stdout + result.stderr


def build_crash_probe(repo_root, core_library, temp_dir):
    source = os.path.join(repo_root, "tests", "free_page_recovery_crash_probe.c")
    source_cmake = source.replace("\\", "/")
    include_cmake = os.path.join(repo_root, "src").replace("\\", "/")
    library_cmake = core_library.replace("\\", "/")
    with open(os.path.join(temp_dir, "CMakeLists.txt"), "w", encoding="utf-8") as handle:
        handle.write(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(tinydb_free_page_recovery_crash_probe C)\n"
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
            f'add_executable(free_page_recovery_crash_probe "{source_cmake}")\n'
            f'target_include_directories(free_page_recovery_crash_probe PRIVATE "{include_cmake}")\n'
            "target_link_libraries(free_page_recovery_crash_probe PRIVATE tinydb_core)\n"
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
        os.path.join(build_dir, "Debug", "free_page_recovery_crash_probe.exe"),
        os.path.join(build_dir, "Release", "free_page_recovery_crash_probe.exe"),
        os.path.join(build_dir, "free_page_recovery_crash_probe.exe"),
        os.path.join(build_dir, "free_page_recovery_crash_probe"),
    ]
    executable = next((path for path in candidates if os.path.exists(path)), None)
    if executable is None:
        raise AssertionError("free-page recovery crash probe was not produced")
    return executable


def read_free_sidecar(path):
    with open(path, "rb") as file:
        payload = file.read()
    if len(payload) < 20:
        raise AssertionError("free-page sidecar is truncated")

    magic, version, num_pages, count = struct.unpack_from("<IIII", payload, 0)
    expected_length = 20 + count * 4
    if len(payload) != expected_length:
        raise AssertionError(
            f"free-page sidecar length mismatch: expected {expected_length}, got {len(payload)}"
        )
    pages = list(struct.unpack_from(f"<{count}I", payload, 16)) if count else []
    checksum = struct.unpack_from("<I", payload, 16 + count * 4)[0]
    return magic, version, num_pages, pages, checksum


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    core_library = find_core_library(repo_root)
    if executable is None:
        raise AssertionError("could not find tinydb executable")
    if core_library is None:
        raise AssertionError("could not find built tinydb_core static library")

    with tempfile.TemporaryDirectory(prefix="tinydb-free-page-recovery-") as temp_dir:
        probe_dir = os.path.join(temp_dir, "probe")
        os.makedirs(probe_dir)
        crash_probe = build_crash_probe(repo_root, core_library, probe_dir)

        db_file = os.path.join(temp_dir, "test_free_page_recovery.db")
        free_file = db_file + ".free"
        free_wal_file = db_file + ".free.wal"
        wal_file = db_file + ".wal"
        cleanup(db_file)

        crashed = subprocess.run(
            [crash_probe, db_file],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        if crashed.returncode != 0:
            raise AssertionError(crashed.stdout + crashed.stderr)
        if not os.path.exists(wal_file) or os.path.getsize(wal_file) == 0:
            raise AssertionError("main WAL missing after deterministic hard exit")
        if not os.path.exists(free_file):
            raise AssertionError("free-page sidecar was not published before hard exit")

        # Simulate losing the allocator sidecar while retaining the durable
        # main WAL. WAL2 must contain enough committed allocator state to
        # reconstruct it during database recovery.
        os.remove(free_file)
        if os.path.exists(free_wal_file):
            os.remove(free_wal_file)

        recovered = run_session(
            executable,
            db_file,
            [
                "SELECT COUNT(*) FROM metrics;",
                "PRAGMA integrity_check;",
                ".stats metrics",
                ".exit",
            ],
        )
        if "WAL file found. Recovering..." not in recovered:
            raise AssertionError("database did not run WAL recovery")
        if "db > 5\nExecuted." not in recovered:
            raise AssertionError(recovered)
        if "page ownership:" in recovered or "db > ok\nExecuted." not in recovered:
            raise AssertionError(recovered)
        if not os.path.exists(free_file):
            raise AssertionError("WAL recovery did not recreate free-page sidecar")

        magic, version, num_pages, free_pages, _ = read_free_sidecar(free_file)
        physical_pages = os.path.getsize(db_file) // PAGE_SIZE
        if magic != FREE_MAGIC or version != FREE_VERSION:
            raise AssertionError("unexpected free-page sidecar format")
        if num_pages != physical_pages:
            raise AssertionError(
                f"sidecar page count {num_pages} does not match database {physical_pages}"
            )
        if not free_pages:
            raise AssertionError("expected recovered reusable pages after tree collapse")
        if len(free_pages) != len(set(free_pages)):
            raise AssertionError("recovered free-page list contains duplicates")
        if any(page == 0 or page >= num_pages for page in free_pages):
            raise AssertionError("recovered free-page list contains an invalid page")

        size_before_reuse = os.path.getsize(db_file)
        refill_commands = [
            f"INSERT INTO metrics VALUES ({i}, {i * 10}, 'refill-{i}');"
            for i in range(61, 81)
        ]
        refill_commands.extend([
            "SELECT COUNT(*) FROM metrics;",
            "PRAGMA integrity_check;",
            ".exit",
        ])
        refilled = run_session(executable, db_file, refill_commands)
        if "db > 25\nExecuted." not in refilled or "db > ok\nExecuted." not in refilled:
            raise AssertionError(refilled)
        if os.path.getsize(db_file) != size_before_reuse:
            raise AssertionError(
                "database grew even though recovered free pages should have been reusable"
            )

    print(
        "PASS: deterministic post-commit hard exit lets WAL2 reconstruct durable "
        "free-page state, and recovered pages are reused without file growth."
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
