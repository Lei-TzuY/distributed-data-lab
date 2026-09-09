import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_open_adapter.h"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    assert "return errno == ENOENT;" in text
    assert "ERROR_FILE_NOT_FOUND" in text
    assert "Manifest removal is deliberately idempotent" in text


def compile_and_run_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for migration cleanup regression")

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
    char database[700];
    FILE* file;

    if (argc != 2) return 100;
    memset(&catalog, 0, sizeof(catalog));
    memset(&pager, 0, sizeof(pager));
    catalog.num_tables = 1u;
    (void)snprintf(catalog.schemas[0].name, sizeof(catalog.schemas[0].name), "fixture");
    catalog.schemas[0].root_page_num = 1u;
    catalog.schemas[0].num_columns = 1u;
    (void)snprintf(catalog.schemas[0].columns[0].name,
                   sizeof(catalog.schemas[0].columns[0].name), "id");
    catalog.schemas[0].columns[0].type = COL_TYPE_INT;
    catalog.schemas[0].columns[0].size = 4u;
    catalog.schemas[0].columns[0].offset = 0u;
    catalog.schemas[0].row_size = 4u;
    pager.num_pages = 2u;
    if (!tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &snapshot)) return 101;
    if (snprintf(database, sizeof(database), "%s/idempotent-cleanup.db", argv[1]) < 0) return 102;
    if (!tinydb_compact_v2_migration_open_adapter_init(
            &context, database, &catalog, &snapshot, &pager)) return 103;

    file = fopen(context.manifest_path, "wb");
    if (file == NULL) return 104;
    if (fputs("fixture", file) < 0 || fclose(file) != 0) return 105;

    if (!expect(tinydb_compact_v2_migration_open_adapter_remove_manifest(&context),
                "first manifest removal failed")) return 1;
    if (!expect(tinydb_compact_v2_migration_open_adapter_remove_manifest(&context),
                "second removal of already-absent manifest was not idempotent")) return 2;
    if (!expect(tinydb_compact_v2_migration_open_adapter_sync_parent(&context),
                "parent durability step failed after idempotent removal")) return 3;

    file = fopen(context.manifest_path, "rb");
    if (!expect(file == NULL, "manifest unexpectedly reappeared")) {
        if (file != NULL) fclose(file);
        return 4;
    }

    puts("manifest_remove_retry=yes");
    puts("cleanup_parent_sync=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-migration-cleanup-idempotence-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBMigrationCleanupIdempotence C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n"
            "  add_compile_options(/W4 /WX /utf-8)\n"
            "  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n"
            "else()\n"
            "  add_compile_options(-Wall -Wextra -Werror)\n"
            "  add_compile_definitions(_XOPEN_SOURCE=700)\n"
            "  find_package(Threads REQUIRED)\n"
            "endif()\n"
            "add_executable(cleanup_probe probe.c)\n"
            f'target_include_directories(cleanup_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
            "if(NOT MSVC)\n  target_link_libraries(cleanup_probe PRIVATE Threads::Threads)\nendif()\n",
            encoding="utf-8",
        )
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
        exe = build / ("Debug/cleanup_probe.exe" if sys.platform.startswith("win") else "cleanup_probe")
        run = subprocess.run(
            [str(exe), str(tmp_path)], capture_output=True, text=True,
            encoding="utf-8", errors="ignore", timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        assert "manifest_remove_retry=yes" in run.stdout
        assert "cleanup_parent_sync=yes" in run.stdout


def main():
    test_source_contract()
    compile_and_run_probe()
    print("PASS: compact V2 migration manifest cleanup is retry-safe on POSIX and Windows")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
