import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_full_page_preflight.h"
LIVE_GUARD = ROOT / "src" / "compact_v2_migration_live_page_guard.h"
ENGINE_GUARD = ROOT / "src" / "engine_open_guard.c"


def test_source_contract():
    preflight = HEADER.read_text(encoding="utf-8")
    live_guard = LIVE_GUARD.read_text(encoding="utf-8")
    engine = ENGINE_GUARD.read_text(encoding="utf-8")
    for token in [
        "tinydb_compact_v2_migration_collect_tree_pages(",
        "tinydb_compact_v2_migration_build_catalog_live_page_map(",
        "tinydb_compact_v2_migration_detached_tree_disjoint_from_live(",
        "tinydb_fixed_v1_page_zero_is_retired(",
        "pager_try_pin_existing_page_handle(",
    ]:
        assert token in live_guard, f"missing complete-tree page ownership guard: {token}"
    for token in [
        "tinydb_compact_v2_migration_claims_are_disjoint_from_live(",
        "tinydb_compact_v2_migration_open_adapter_manifest_pages_are_safe(",
    ]:
        assert token in preflight, f"missing full-page preflight helper: {token}"
    assert "tinydb_compact_v2_migration_open_adapter_manifest_pages_are_safe" in engine


def configure_and_build(tmp_path: Path):
    cmake = (
        "cmake_minimum_required(VERSION 3.10)\n"
        "project(TinyDBCompactV2FullPagePreflight C)\n"
        "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
        "if(MSVC)\n"
        "  add_compile_options(/W4 /WX /utf-8)\n"
        "  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n"
        "else()\n"
        "  add_compile_options(-Wall -Wextra -Werror)\n"
        "  add_compile_definitions(_XOPEN_SOURCE=700)\n"
        "  find_package(Threads REQUIRED)\n"
        "endif()\n"
        "add_executable(full_page_preflight_probe probe.c)\n"
        f'target_include_directories(full_page_preflight_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        "if(NOT MSVC)\n  target_link_libraries(full_page_preflight_probe PRIVATE Threads::Threads)\nendif()\n"
    )
    (tmp_path / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    build = tmp_path / "build"
    configure = subprocess.run(
        ["cmake", "-S", str(tmp_path), "-B", str(build)],
        capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=60,
    )
    if configure.returncode != 0:
        raise AssertionError(configure.stdout + configure.stderr)
    compiled = subprocess.run(
        ["cmake", "--build", str(build), "--config", "Debug"],
        capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=120,
    )
    if compiled.returncode != 0:
        raise AssertionError(compiled.stdout + compiled.stderr)
    return build


def run_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for full-page recovery regression")

    source = r'''#include "compact_v2_migration_full_page_preflight.h"
#include <stdio.h>
#include <string.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

int main(void) {
    uint8_t live[32];
    uint32_t claims[2] = {9u, 13u};
    TinyDBCompactV2MigrationManifest manifest;

    memset(live, 0, sizeof(live));
    memset(&manifest, 0, sizeof(manifest));
    manifest.table_id = 1u;
    manifest.old_root_page_num = 4u;
    manifest.staged_root_page_num = 9u;
    manifest.old_schema_generation = UINT64_C(7);
    manifest.new_schema_generation = UINT64_C(8);
    manifest.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED;
    manifest.claimed_page_count = 2u;
    manifest.claimed_pages = claims;

    /* page 13 models another table's live non-root leaf/internal page. */
    live[4] = 1u;
    live[13] = 1u;
    if (!expect(!tinydb_compact_v2_migration_claims_are_disjoint_from_live(
                    &manifest, live, (uint32_t)sizeof(live)),
                "recovery allowed a staging claim that overlaps a live non-root page")) return 1;

    live[13] = 0u;
    if (!expect(tinydb_compact_v2_migration_claims_are_disjoint_from_live(
                    &manifest, live, (uint32_t)sizeof(live)),
                "safe staging claims were rejected")) return 2;

    claims[1] = 32u;
    if (!expect(!tinydb_compact_v2_migration_claims_are_disjoint_from_live(
                    &manifest, live, (uint32_t)sizeof(live)),
                "out-of-file staging claim was accepted")) return 3;

    puts("non_root_live_overlap_rejected=yes");
    puts("safe_claims_allowed=yes");
    puts("out_of_file_claim_rejected=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-full-page-preflight-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        build = configure_and_build(tmp_path)
        exe = build / (
            "Debug/full_page_preflight_probe.exe"
            if sys.platform.startswith("win")
            else "full_page_preflight_probe"
        )
        run = subprocess.run(
            [str(exe)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in [
            "non_root_live_overlap_rejected=yes",
            "safe_claims_allowed=yes",
            "out_of_file_claim_rejected=yes",
        ]:
            assert marker in run.stdout, f"missing {marker}: {run.stdout}"


def main():
    test_source_contract()
    run_probe()
    print("PASS: compact V2 reopen preflight rejects reclaim of live non-root table pages")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
