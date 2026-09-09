import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_open_adapter.h"
FULL_PAGE_PREFLIGHT = ROOT / "src" / "compact_v2_migration_full_page_preflight.h"
OPEN_RECOVERY = ROOT / "src" / "compact_v2_migration_open_recovery.h"
ENGINE_GUARD = ROOT / "src" / "engine_open_guard.c"


def test_source_contract():
    adapter = HEADER.read_text(encoding="utf-8")
    full_page_preflight = FULL_PAGE_PREFLIGHT.read_text(encoding="utf-8")
    open_recovery = OPEN_RECOVERY.read_text(encoding="utf-8")
    engine = ENGINE_GUARD.read_text(encoding="utf-8")
    for token in [
        "tinydb_compact_v2_migration_open_adapter_is_authoritative_root(",
        "tinydb_compact_v2_migration_open_adapter_manifest_is_safe(",
        "TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING",
        "TINYDB_COMPACT_V2_MIGRATION_STRICT_KEEP_NEW_RECLAIM_OLD",
    ]:
        assert token in adapter, f"missing compatibility cross-table recovery guard: {token}"
    for token in [
        "tinydb_compact_v2_migration_build_catalog_live_page_map(",
        "tinydb_compact_v2_migration_claims_are_disjoint_from_live(",
        "tinydb_compact_v2_migration_detached_tree_disjoint_from_live(",
        "tinydb_compact_v2_migration_open_adapter_manifest_pages_are_safe(",
    ]:
        assert token in full_page_preflight, f"missing full-page recovery guard: {token}"
    assert "tinydb_compact_v2_migration_recover_open_file_with_preflight(" in open_recovery
    assert "preflight(preflight_context, &workspace->manifest)" in open_recovery
    assert "tinydb_compact_v2_migration_recover_open_file_with_preflight(" in engine
    assert "tinydb_compact_v2_migration_open_adapter_manifest_pages_are_safe" in engine


def configure_and_build(tmp_path: Path):
    cmake = (
        "cmake_minimum_required(VERSION 3.10)\n"
        "project(TinyDBCompactV2CrossTableRootGuard C)\n"
        "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
        "if(MSVC)\n"
        "  add_compile_options(/W4 /WX /utf-8)\n"
        "  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n"
        "else()\n"
        "  add_compile_options(-Wall -Wextra -Werror)\n"
        "  add_compile_definitions(_XOPEN_SOURCE=700)\n"
        "  find_package(Threads REQUIRED)\n"
        "endif()\n"
        "add_executable(cross_table_root_guard_probe probe.c)\n"
        f'target_include_directories(cross_table_root_guard_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        "if(NOT MSVC)\n  target_link_libraries(cross_table_root_guard_probe PRIVATE Threads::Threads)\nendif()\n"
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
        raise AssertionError("cmake is required for cross-table migration recovery regression")

    source = r'''#include "compact_v2_migration_open_adapter.h"
#include <stdio.h>
#include <string.h>

static void init_schema(TableSchema* schema, const char* name, uint32_t root) {
    memset(schema, 0, sizeof(*schema));
    (void)snprintf(schema->name, sizeof(schema->name), "%s", name);
    schema->root_page_num = root;
    schema->num_columns = 1u;
    (void)snprintf(schema->columns[0].name, sizeof(schema->columns[0].name), "id");
    schema->columns[0].type = COL_TYPE_INT;
    schema->columns[0].size = 4u;
    schema->columns[0].offset = 0u;
    schema->row_size = 4u;
}

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

int main(void) {
    Catalog catalog;
    Pager pager;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    TinyDBCompactV2MigrationOpenAdapterContext context;
    uint32_t bad_claims[2] = {9u, 17u};
    uint32_t safe_claims[2] = {9u, 10u};
    TinyDBCompactV2MigrationManifest manifest;

    memset(&catalog, 0, sizeof(catalog));
    memset(&pager, 0, sizeof(pager));
    catalog.num_tables = 2u;
    init_schema(&catalog.schemas[0], "orders", 4u);
    init_schema(&catalog.schemas[1], "audit", 17u);
    pager.num_pages = 32u;
    if (!tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &snapshot)) return 100;
    snapshot.entries[0].schema_generation = UINT64_C(7);
    snapshot.entries[1].schema_generation = UINT64_C(3);
    if (!tinydb_compact_v2_migration_open_adapter_init(
            &context, "cross-table.db", &catalog, &snapshot, &pager)) return 101;

    memset(&manifest, 0, sizeof(manifest));
    manifest.table_id = 1u;
    manifest.old_root_page_num = 4u;
    manifest.staged_root_page_num = 9u;
    manifest.old_schema_generation = UINT64_C(7);
    manifest.new_schema_generation = UINT64_C(8);
    manifest.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED;
    manifest.claimed_page_count = 2u;
    manifest.claimed_pages = bad_claims;
    if (!expect(!tinydb_compact_v2_migration_open_adapter_manifest_is_safe(&context, &manifest),
                "staging manifest was allowed to claim another live table root")) return 1;
    manifest.claimed_pages = safe_claims;
    if (!expect(tinydb_compact_v2_migration_open_adapter_manifest_is_safe(&context, &manifest),
                "safe unpublished staging manifest was rejected")) return 2;

    catalog.schemas[0].root_page_num = 9u;
    snapshot.entries[0].root_page_num = 9u;
    snapshot.entries[0].schema_generation = UINT64_C(8);
    manifest.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED;
    manifest.old_root_page_num = 17u;
    if (!expect(!tinydb_compact_v2_migration_open_adapter_manifest_is_safe(&context, &manifest),
                "retired root was allowed to target another live table root")) return 3;
    manifest.old_root_page_num = 12u;
    if (!expect(tinydb_compact_v2_migration_open_adapter_manifest_is_safe(&context, &manifest),
                "safe published migration manifest was rejected")) return 4;

    puts("staging_cross_table_root_rejected=yes");
    puts("safe_staging_allowed=yes");
    puts("old_tree_cross_table_root_rejected=yes");
    puts("safe_old_tree_allowed=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-cross-table-root-guard-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        build = configure_and_build(tmp_path)
        exe = build / (
            "Debug/cross_table_root_guard_probe.exe"
            if sys.platform.startswith("win")
            else "cross_table_root_guard_probe"
        )
        run = subprocess.run(
            [str(exe)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in [
            "staging_cross_table_root_rejected=yes",
            "safe_staging_allowed=yes",
            "old_tree_cross_table_root_rejected=yes",
            "safe_old_tree_allowed=yes",
        ]:
            assert marker in run.stdout, f"missing {marker}: {run.stdout}"


def main():
    test_source_contract()
    run_probe()
    print("PASS: compact V2 reopen recovery keeps the legacy root guard and wires full-page preflight")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
