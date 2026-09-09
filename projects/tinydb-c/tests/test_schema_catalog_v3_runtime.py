import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "schema_catalog_v3_runtime.h"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_schema_catalog_v3_runtime_bootstrap_legacy(",
        "tinydb_schema_catalog_v3_runtime_restore(",
        "tinydb_schema_catalog_v3_runtime_encode_envelope(",
        "tinydb_schema_catalog_v3_runtime_reconcile_appended_tables(",
        "tinydb_schema_catalog_v3_runtime_publish_schema_change(",
        "tinydb_schema_catalog_v3_runtime_authoritative(",
        "authoritative_v3",
    ]:
        assert token in text


def test_runtime_bridge_compiles_and_executes():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for schema-catalog V3 runtime regression")

    source = r'''#include "schema_catalog_v3_runtime.h"
#include <stdio.h>
#include <string.h>

static int check(int condition, const char* msg) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", msg);
    return 0;
}

int main(void) {
    Catalog catalog;
    TinyDBSchemaCatalogV3Runtime runtime;
    TinyDBSchemaCatalogV3Runtime restored;
    TinyDBSchemaCatalogV3EnvelopeView view;
    unsigned char identity[TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE];
    unsigned char envelope[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    const unsigned char shape[] = {1u,2u,3u,4u,5u};
    size_t envelope_size = 0u;
    uint32_t root = 0u;
    uint64_t generation = 0u;

    memset(&catalog, 0, sizeof(catalog));
    catalog.num_tables = 2u;
    strcpy(catalog.schemas[0].name, "narrow");
    catalog.schemas[0].root_page_num = 4u;
    catalog.schemas[0].num_columns = 1u;
    catalog.schemas[0].row_size = 4u;
    strcpy(catalog.schemas[1].name, "wide");
    catalog.schemas[1].root_page_num = 17u;
    catalog.schemas[1].num_columns = 3u;
    catalog.schemas[1].row_size = 516u;

    if (!check(tinydb_schema_catalog_v3_runtime_bootstrap_legacy(&catalog, &runtime),
               "legacy bootstrap failed")) return 1;
    if (!check(!runtime.authoritative_v3 &&
               runtime.generation.entries[0].table_id == 1u &&
               runtime.generation.entries[1].table_id == 2u,
               "legacy identities invalid")) return 2;

    if (!check(tinydb_schema_catalog_v3_runtime_publish_schema_change(
                   &catalog, &runtime, 2u, 17u, UINT64_C(1), 23u),
               "root replacement publication failed")) return 3;
    if (!check(tinydb_schema_catalog_v3_runtime_authoritative(
                   &catalog, &runtime, 2u, &root, &generation) &&
               root == 23u && generation == UINT64_C(2),
               "replacement state invalid")) return 4;

    if (!check(tinydb_schema_catalog_v3_runtime_publish_schema_change(
                   &catalog, &runtime, 2u, 23u, UINT64_C(2), 23u),
               "shape-only generation publication failed")) return 5;
    if (!check(tinydb_schema_catalog_v3_runtime_authoritative(
                   &catalog, &runtime, 2u, &root, &generation) &&
               root == 23u && generation == UINT64_C(3),
               "shape-only generation did not advance")) return 6;

    catalog.num_tables = 3u;
    strcpy(catalog.schemas[2].name, "events");
    catalog.schemas[2].root_page_num = 31u;
    catalog.schemas[2].num_columns = 2u;
    catalog.schemas[2].row_size = 68u;
    if (!check(tinydb_schema_catalog_v3_runtime_reconcile_appended_tables(
                   &catalog, &runtime),
               "append-only identity reconciliation failed")) return 7;
    if (!check(runtime.generation.entries[2].table_id == 3u &&
               runtime.generation.entries[2].schema_generation == UINT64_C(1),
               "new table identity invalid")) return 8;

    if (!check(tinydb_schema_catalog_v3_runtime_encode_envelope(
                   &catalog, &runtime, shape, sizeof(shape),
                   identity, sizeof(identity), envelope, sizeof(envelope),
                   &envelope_size),
               "runtime envelope encode failed")) return 9;
    if (!check(tinydb_schema_catalog_v3_runtime_restore(
                   &catalog, envelope, envelope_size, &restored, &view) ==
                   TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK,
               "runtime restore failed")) return 10;
    if (!check(restored.authoritative_v3 &&
               restored.generation.entries[1].schema_generation == UINT64_C(3) &&
               restored.generation.entries[2].table_id == 3u,
               "restored metadata invalid")) return 11;

    Catalog drift = catalog;
    drift.schemas[1].root_page_num = 99u;
    if (!check(tinydb_schema_catalog_v3_runtime_restore(
                   &drift, envelope, envelope_size, &restored, &view) ==
                   TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID,
               "root drift accepted")) return 12;
    if (!check(restored.generation.num_tables == 0u && !restored.authoritative_v3,
               "failed restore leaked runtime state")) return 13;

    puts("legacy_bootstrap=yes");
    puts("schema_generation_monotonic=yes");
    puts("append_identity=yes");
    puts("v3_restore_fail_closed=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-schema-v3-runtime-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBSchemaV3RuntimeProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(schema_v3_runtime_probe probe.c)\n"
            f'target_include_directories(schema_v3_runtime_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
            encoding="utf-8",
        )
        build = tmp_path / "build"
        configure = subprocess.run(
            ["cmake", "-S", str(tmp_path), "-B", str(build)],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=60,
        )
        assert configure.returncode == 0, configure.stdout + configure.stderr
        compiled = subprocess.run(
            ["cmake", "--build", str(build), "--config", "Debug"],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=120,
        )
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        executable = build / ("Debug/schema_v3_runtime_probe.exe" if shutil.which("cl") else "schema_v3_runtime_probe")
        run = subprocess.run(
            [str(executable)], capture_output=True, text=True,
            encoding="utf-8", errors="ignore", timeout=30,
        )
        assert run.returncode == 0, run.stdout + run.stderr
        for token in [
            "legacy_bootstrap=yes",
            "schema_generation_monotonic=yes",
            "append_identity=yes",
            "v3_restore_fail_closed=yes",
        ]:
            assert token in run.stdout
