import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_open_adapter.h"
ENGINE_GUARD = ROOT / "src" / "engine_open_guard.c"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_compact_v2_migration_catalog_state_is_valid(",
        "tinydb_compact_v2_migration_catalog_state_read(",
        "tinydb_compact_v2_migration_open_adapter_remove_manifest(",
        "tinydb_compact_v2_migration_open_adapter_sync_parent(",
        "MOVEFILE_WRITE_THROUGH",
        "fsync(fd)",
    ]:
        assert token in text, f"missing production recovery adapter invariant: {token}"

    guard = ENGINE_GUARD.read_text(encoding="utf-8")
    required = [
        "tinydb_schema_catalog_load_authoritative_generation(",
        "tinydb_compact_v2_migration_open_adapter_init(",
        "tinydb_compact_v2_migration_open_adapter_build(",
        "tinydb_compact_v2_migration_recover_open_file_with_preflight(",
        "tinydb_compact_v2_migration_open_adapter_manifest_pages_are_safe",
        "TINYDB_COMPACT_V2_MIGRATION_OPEN_NO_MIGRATION",
        "TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERED",
    ]
    for token in required:
        assert token in guard, f"engine open is missing recovery lifecycle step: {token}"
    assert guard.index("tinydb_schema_catalog_load_authoritative_generation(") < guard.index(
        "tinydb_compact_v2_migration_recover_open_file_with_preflight("
    )


def configure_and_build(tmp_path: Path):
    cmake = (
        "cmake_minimum_required(VERSION 3.10)\n"
        "project(TinyDBCompactV2OpenAdapterProbe C)\n"
        "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
        "if(MSVC)\n"
        "  add_compile_options(/W4 /WX /utf-8)\n"
        "  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n"
        "else()\n"
        "  add_compile_options(-Wall -Wextra -Werror)\n"
        "  add_compile_definitions(_XOPEN_SOURCE=700)\n"
        "  find_package(Threads REQUIRED)\n"
        "endif()\n"
        "add_executable(compact_v2_open_adapter_probe probe.c)\n"
        f'target_include_directories(compact_v2_open_adapter_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        "if(NOT MSVC)\n  target_link_libraries(compact_v2_open_adapter_probe PRIVATE Threads::Threads)\nendif()\n"
    )
    (tmp_path / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    build = tmp_path / "build"
    configure = subprocess.run(
        ["cmake", "-S", str(tmp_path), "-B", str(build)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=60,
    )
    if configure.returncode != 0:
        raise AssertionError(configure.stdout + configure.stderr)
    compiled = subprocess.run(
        ["cmake", "--build", str(build), "--config", "Debug"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    if compiled.returncode != 0:
        raise AssertionError(compiled.stdout + compiled.stderr)
    return build


def run_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for compact V2 open-adapter regression")

    source = r'''#include "compact_v2_migration_open_adapter.h"
#include <stdio.h>
#include <string.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

int main(int argc, char** argv) {
    Catalog catalog;
    Pager pager;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    TinyDBCompactV2MigrationOpenAdapterContext context;
    TinyDBCompactV2MigrationPagerRecoveryAdapter adapter;
    char database[700];
    FILE* manifest;
    uint32_t root = 99u;
    uint64_t generation = 99u;

    if (argc != 2) return 100;
    memset(&catalog, 0, sizeof(catalog));
    memset(&pager, 0, sizeof(pager));
    catalog.num_tables = 1u;
    (void)snprintf(catalog.schemas[0].name, sizeof(catalog.schemas[0].name), "wide");
    catalog.schemas[0].root_page_num = 4u;
    catalog.schemas[0].num_columns = 1u;
    (void)snprintf(catalog.schemas[0].columns[0].name,
                   sizeof(catalog.schemas[0].columns[0].name), "id");
    catalog.schemas[0].columns[0].type = COL_TYPE_INT;
    catalog.schemas[0].columns[0].size = 4u;
    catalog.schemas[0].columns[0].offset = 0u;
    catalog.schemas[0].row_size = 4u;
    pager.num_pages = 8u;

    if (!tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &snapshot)) return 101;
    snapshot.entries[0].schema_generation = UINT64_C(7);
    if (snprintf(database, sizeof(database), "%s/open-adapter.db", argv[1]) < 0) return 102;

    if (!expect(tinydb_compact_v2_migration_open_adapter_init(
                    &context, database, &catalog, &snapshot, &pager),
                "adapter init failed")) return 1;
    if (!expect(tinydb_compact_v2_migration_open_adapter_build(&context, &adapter),
                "adapter build failed")) return 2;
    if (!expect(adapter.read_catalog(adapter.context, 1u, &root, &generation),
                "authoritative catalog lookup failed")) return 3;
    if (!expect(root == 4u && generation == 7u,
                "authoritative catalog values drifted")) return 4;

    root = 99u;
    generation = 99u;
    if (!expect(!adapter.read_catalog(adapter.context, 2u, &root, &generation),
                "unknown table id did not fail closed")) return 5;
    if (!expect(root == 0u && generation == 0u,
                "failed lookup leaked partial output")) return 6;

    manifest = fopen(context.manifest_path, "wb");
    if (manifest == NULL) return 103;
    if (fputs("fixture", manifest) < 0 || fclose(manifest) != 0) return 104;
    if (!expect(adapter.remove_manifest(adapter.context),
                "manifest removal failed")) return 7;
    if (!expect(adapter.sync_parent(adapter.context),
                "parent durability step failed")) return 8;
    manifest = fopen(context.manifest_path, "rb");
    if (!expect(manifest == NULL, "active manifest still exists after cleanup")) {
        if (manifest != NULL) fclose(manifest);
        return 9;
    }

    catalog.schemas[0].root_page_num = 8u;
    if (!expect(!tinydb_compact_v2_migration_open_adapter_init(
                    &context, database, &catalog, &snapshot, &pager),
                "out-of-file root did not fail adapter initialization")) return 10;

    puts("authoritative_lookup=yes");
    puts("unknown_id_fail_closed=yes");
    puts("durable_cleanup=yes");
    puts("root_bounds_fail_closed=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-compact-v2-open-adapter-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        build = configure_and_build(tmp_path)
        exe = build / (
            "Debug/compact_v2_open_adapter_probe.exe"
            if sys.platform.startswith("win")
            else "compact_v2_open_adapter_probe"
        )
        run = subprocess.run(
            [str(exe), str(tmp_path)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in [
            "authoritative_lookup=yes",
            "unknown_id_fail_closed=yes",
            "durable_cleanup=yes",
            "root_bounds_fail_closed=yes",
        ]:
            assert marker in run.stdout, f"missing {marker}: {run.stdout}"


def main():
    test_source_contract()
    run_probe()
    print("PASS: production compact V2 open adapter binds authoritative catalog state and durable cleanup")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
