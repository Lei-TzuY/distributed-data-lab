import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_production_catalog_uses_v3_store():
    text = (ROOT / "src" / "schema_catalog_v2.c").read_text(encoding="utf-8")
    for token in [
        'schema_catalog_v3_store.h',
        'recover_v3_wal_if_present(',
        'load_v3_main(',
        'derive_snapshot_for_save(',
        'tinydb_schema_catalog_v3_store_publish(',
    ]:
        assert token in text


def test_production_v3_save_generation_roundtrip():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required")
    source = r'''#include "multitable.h"
#include "schema_catalog_shape_codec.h"
#include "schema_catalog_v3_store.h"
#include <stdio.h>
#include <string.h>

bool multitable_catalog_load_v1_base(Table* table, const char* database_filename) {
    (void)table; (void)database_filename; return false;
}

static void init_table(TableSchema* s, const char* name, uint32_t root,
                       uint32_t varchar_size) {
    memset(s, 0, sizeof(*s));
    strcpy(s->name, name);
    s->root_page_num = root;
    s->num_columns = varchar_size ? 2u : 1u;
    strcpy(s->columns[0].name, "id");
    s->columns[0].type = COL_TYPE_INT;
    s->columns[0].size = 4u;
    s->columns[0].offset = 0u;
    s->row_size = 4u;
    if (varchar_size) {
        strcpy(s->columns[1].name, "payload");
        s->columns[1].type = COL_TYPE_VARCHAR;
        s->columns[1].size = varchar_size;
        s->columns[1].offset = 4u;
        s->row_size += varchar_size;
    }
}

static int read_snapshot(const char* main_path,
                         Catalog* catalog,
                         TinyDBSchemaCatalogGenerationSnapshot* snapshot) {
    unsigned char envelope[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    size_t envelope_size = 0u;
    TinyDBSchemaCatalogV3EnvelopeView view;
    if (tinydb_schema_catalog_v3_store_read(main_path, false, envelope,
            sizeof(envelope), &envelope_size) != TINYDB_SCHEMA_CATALOG_V3_STORE_READ_OK) return 0;
    if (tinydb_schema_catalog_v3_envelope_decode(envelope, envelope_size, &view) !=
            TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK) return 0;
    if (!tinydb_schema_catalog_shape_decode(view.shape, view.shape_size, catalog)) return 0;
    return tinydb_schema_catalog_v3_decode(catalog, view.identity, view.identity_size, snapshot) ==
           TINYDB_SCHEMA_CATALOG_V3_DECODE_OK;
}

int main(void) {
    const char* db = "prod-v3.db";
    const char* main_path = "prod-v3.db.schema";
    Table table;
    Catalog decoded;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    memset(&table, 0, sizeof(table));
    table.catalog.num_tables = 2u;
    init_table(&table.catalog.schemas[0], "narrow", 4u, 0u);
    init_table(&table.catalog.schemas[1], "wide", 17u, 512u);

    if (!multitable_catalog_save(&table, db)) return 1;
    if (!read_snapshot(main_path, &decoded, &snapshot)) return 2;
    if (snapshot.entries[0].table_id != 1u || snapshot.entries[0].schema_generation != 1u ||
        snapshot.entries[1].table_id != 2u || snapshot.entries[1].schema_generation != 1u) return 3;

    strcpy(table.catalog.schemas[0].name, "renamed_narrow");
    table.catalog.schemas[1].root_page_num = 23u;
    table.catalog.schemas[1].columns[1].size = 1024u;
    table.catalog.schemas[1].row_size = 1028u;
    table.catalog.num_tables = 3u;
    init_table(&table.catalog.schemas[2], "events", 31u, 64u);
    if (!multitable_catalog_save(&table, db)) return 4;
    if (!read_snapshot(main_path, &decoded, &snapshot)) return 5;
    if (strcmp(decoded.schemas[0].name, "renamed_narrow") != 0) return 6;
    if (snapshot.entries[0].schema_generation != 1u) return 7;
    if (snapshot.entries[1].root_page_num != 23u || snapshot.entries[1].schema_generation != 2u) return 8;
    if (snapshot.entries[2].table_id != 3u || snapshot.entries[2].schema_generation != 1u) return 9;

    FILE* wal = fopen("prod-v3.db.schema.wal", "rb");
    if (wal != NULL) { fclose(wal); return 10; }
    remove(main_path);
    puts("production_v3_upgrade=yes");
    puts("generation_diff=yes");
    puts("append_identity=yes");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-v3-production-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\nproject(TinyDBV3Production C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(v3_production_probe probe.c \"" + (ROOT / "src" / "schema_catalog_v2.c").as_posix() + "\")\n"
            f'target_include_directories(v3_production_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
            encoding="utf-8",
        )
        build = tmp_path / "build"
        cfg = subprocess.run(["cmake", "-S", str(tmp_path), "-B", str(build)], capture_output=True, text=True, timeout=60)
        assert cfg.returncode == 0, cfg.stdout + cfg.stderr
        comp = subprocess.run(["cmake", "--build", str(build), "--config", "Debug"], capture_output=True, text=True, timeout=120)
        assert comp.returncode == 0, comp.stdout + comp.stderr
        exe = build / ("Debug/v3_production_probe.exe" if shutil.which("cl") else "v3_production_probe")
        run = subprocess.run([str(exe)], cwd=tmp_path, capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, run.stdout + run.stderr
        for token in ["production_v3_upgrade=yes", "generation_diff=yes", "append_identity=yes"]:
            assert token in run.stdout
