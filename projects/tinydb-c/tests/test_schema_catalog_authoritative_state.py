import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "schema_catalog_authoritative_state.h"
ENGINE_GUARD = ROOT / "src" / "engine_open_guard.c"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_schema_catalog_generation_bootstrap_legacy(",
        "tinydb_schema_catalog_v3_store_read(",
        "tinydb_schema_catalog_v3_envelope_decode(",
        "tinydb_schema_catalog_v3_decode(",
        "tinydb_schema_catalog_authoritative_shapes_equal(",
        "root_page_num >= table->pager->num_pages",
        "*snapshot_out = decoded_snapshot",
    ]:
        assert token in text, f"missing authoritative-state invariant: {token}"

    guard = ENGINE_GUARD.read_text(encoding="utf-8")
    assert "multitable_catalog_load(database->table" in guard
    assert "tinydb_schema_catalog_load_authoritative_generation(" in guard
    assert guard.index("multitable_catalog_load(database->table") < guard.index(
        "tinydb_schema_catalog_load_authoritative_generation("
    )


def configure_and_build(tmp_path: Path):
    cmake = (
        "cmake_minimum_required(VERSION 3.10)\n"
        "project(TinyDBAuthoritativeCatalogProbe C)\n"
        "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
        "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\n"
        "  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n"
        "else()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
        "add_executable(authoritative_catalog_probe probe.c)\n"
        f'target_include_directories(authoritative_catalog_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
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
        raise AssertionError("cmake is required for authoritative catalog regression")

    source = r'''#include "schema_catalog_authoritative_state.h"
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

static int write_v3(const char* database, const Catalog* catalog) {
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    unsigned char shape[TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE];
    unsigned char identity[TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE];
    unsigned char envelope[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    size_t shape_size = 0u, identity_size = 0u, envelope_size = 0u;
    char path[TINYDB_SCHEMA_CATALOG_AUTHORITATIVE_PATH_MAX];
    FILE* file;

    if (!tinydb_schema_catalog_generation_bootstrap_legacy(catalog, &snapshot)) return 0;
    snapshot.entries[0].schema_generation = UINT64_C(7);
    snapshot.entries[1].schema_generation = UINT64_C(19);
    if (!tinydb_schema_catalog_shape_encode(catalog, shape, sizeof(shape), &shape_size) ||
        !tinydb_schema_catalog_v3_encode(catalog, &snapshot, identity, sizeof(identity), &identity_size) ||
        !tinydb_schema_catalog_v3_envelope_encode(shape, shape_size, identity, identity_size,
                                                   envelope, sizeof(envelope), &envelope_size) ||
        !tinydb_schema_catalog_authoritative_build_path(database, path, sizeof(path))) {
        return 0;
    }
    file = fopen(path, "wb");
    if (file == NULL) return 0;
    if (fwrite(envelope, 1u, envelope_size, file) != envelope_size) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

int main(int argc, char** argv) {
    Table table;
    Pager pager;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    char legacy_db[700];
    char v3_db[700];

    if (argc != 2) return 100;
    memset(&table, 0, sizeof(table));
    memset(&pager, 0, sizeof(pager));
    pager.num_pages = 32u;
    table.pager = &pager;
    table.catalog.num_tables = 2u;
    init_schema(&table.catalog.schemas[0], "narrow", 4u);
    init_schema(&table.catalog.schemas[1], "wide", 17u);

    if (snprintf(legacy_db, sizeof(legacy_db), "%s/legacy.db", argv[1]) < 0 ||
        snprintf(v3_db, sizeof(v3_db), "%s/v3.db", argv[1]) < 0) return 101;

    if (!expect(tinydb_schema_catalog_load_authoritative_generation(
                    &table, legacy_db, &snapshot),
                "legacy bootstrap failed")) return 1;
    if (!expect(snapshot.num_tables == 2u &&
                snapshot.entries[0].table_id == 1u &&
                snapshot.entries[0].schema_generation == 1u &&
                snapshot.entries[1].table_id == 2u &&
                snapshot.entries[1].schema_generation == 1u,
                "legacy bootstrap identity drift")) return 2;

    if (!expect(write_v3(v3_db, &table.catalog), "unable to write V3 fixture")) return 3;
    if (!expect(tinydb_schema_catalog_load_authoritative_generation(
                    &table, v3_db, &snapshot),
                "V3 authoritative state failed")) return 4;
    if (!expect(snapshot.entries[0].schema_generation == 7u &&
                snapshot.entries[1].schema_generation == 19u,
                "persisted generations were not restored")) return 5;

    table.catalog.schemas[1].root_page_num = 18u;
    if (!expect(!tinydb_schema_catalog_load_authoritative_generation(
                    &table, v3_db, &snapshot),
                "catalog/envelope root drift did not fail closed")) return 6;
    if (!expect(snapshot.num_tables == 0u,
                "failed authoritative load published partial state")) return 7;

    table.catalog.schemas[1].root_page_num = 17u;
    pager.num_pages = 17u;
    if (!expect(!tinydb_schema_catalog_load_authoritative_generation(
                    &table, v3_db, &snapshot),
                "out-of-file catalog root did not fail closed")) return 8;
    if (!expect(snapshot.num_tables == 0u,
                "out-of-file failure published state")) return 9;

    puts("legacy_bootstrap=yes");
    puts("v3_generation_restore=yes");
    puts("shape_drift_fail_closed=yes");
    puts("root_bounds_fail_closed=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-authoritative-catalog-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        build = configure_and_build(tmp_path)
        exe = build / ("Debug/authoritative_catalog_probe.exe" if sys.platform.startswith("win") else "authoritative_catalog_probe")
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
            "legacy_bootstrap=yes",
            "v3_generation_restore=yes",
            "shape_drift_fail_closed=yes",
            "root_bounds_fail_closed=yes",
        ]:
            assert marker in run.stdout, f"missing {marker}: {run.stdout}"


def main():
    test_source_contract()
    run_probe()
    print("PASS: database open reconstructs authoritative catalog generations and rejects drift")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
