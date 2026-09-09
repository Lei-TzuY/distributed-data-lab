import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_schema_repack_catalog_v3_publishes_shape_root_and_generation_together():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required")

    source = r'''#include "multitable.h"
#include "schema_repack_catalog_v3_publish.h"
#include <stdio.h>
#include <string.h>

bool multitable_catalog_load_v1_base(Table* table, const char* database_filename) {
    (void)table; (void)database_filename; return false;
}

static void init_schema(TableSchema* schema, uint32_t root, uint32_t varchar_size) {
    memset(schema, 0, sizeof(*schema));
    strcpy(schema->name, "items");
    schema->root_page_num = root;
    schema->num_columns = 2u;
    strcpy(schema->columns[0].name, "id");
    schema->columns[0].type = COL_TYPE_INT;
    schema->columns[0].size = 4u;
    schema->columns[0].offset = 0u;
    strcpy(schema->columns[1].name, "payload");
    schema->columns[1].type = COL_TYPE_VARCHAR;
    schema->columns[1].size = varchar_size;
    schema->columns[1].offset = 4u;
    schema->row_size = 4u + varchar_size;
}

static int read_state(const char* path,
                      Catalog* catalog,
                      TinyDBSchemaCatalogGenerationSnapshot* snapshot) {
    unsigned char envelope[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    size_t envelope_size = 0u;
    TinyDBSchemaCatalogV3EnvelopeView view;
    if (tinydb_schema_catalog_v3_store_read(path, false, envelope, sizeof(envelope),
                                             &envelope_size) != TINYDB_SCHEMA_CATALOG_V3_STORE_READ_OK) return 0;
    if (tinydb_schema_catalog_v3_envelope_decode(envelope, envelope_size, &view) !=
        TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK) return 0;
    if (!tinydb_schema_catalog_shape_decode(view.shape, view.shape_size, catalog)) return 0;
    return tinydb_schema_catalog_v3_decode(catalog, view.identity, view.identity_size, snapshot) ==
           TINYDB_SCHEMA_CATALOG_V3_DECODE_OK;
}

int main(int argc, char** argv) {
    Table table;
    Pager pager;
    TableSchema destination;
    TinyDBSchemaRepackCatalogV3PublishContext context;
    TinyDBSchemaRepackCatalogPublishOps ops;
    Catalog decoded;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    char main_path[768];
    if (argc != 2) return 100;
    memset(&table, 0, sizeof(table));
    memset(&pager, 0, sizeof(pager));
    memset(&context, 0, sizeof(context));
    memset(&ops, 0, sizeof(ops));
    pager.num_pages = 100u;
    table.pager = &pager;
    table.catalog.num_tables = 1u;
    init_schema(&table.catalog.schemas[0], 4u, 32u);
    if (!multitable_catalog_save(&table, argv[1])) return 1;

    destination = table.catalog.schemas[0];
    destination.columns[1].size = 128u;
    destination.row_size = 132u;
    context.table = &table;
    context.database_filename = argv[1];
    context.destination_schema = &destination;
    if (!tinydb_schema_repack_catalog_v3_publish_ops_init(&context, &ops)) return 2;
    if (!ops.publish_catalog_durable(ops.context, 1u, 4u, UINT64_C(1), 9u, UINT64_C(2))) return 3;
    if (table.catalog.schemas[0].root_page_num != 9u ||
        table.catalog.schemas[0].columns[1].size != 128u ||
        table.catalog.schemas[0].row_size != 132u) return 4;

    if (snprintf(main_path, sizeof(main_path), "%s.schema", argv[1]) < 0) return 5;
    if (!read_state(main_path, &decoded, &snapshot)) return 6;
    if (decoded.schemas[0].root_page_num != 9u ||
        decoded.schemas[0].columns[1].size != 128u ||
        decoded.schemas[0].row_size != 132u ||
        snapshot.entries[0].table_id != 1u ||
        snapshot.entries[0].root_page_num != 9u ||
        snapshot.entries[0].schema_generation != UINT64_C(2)) return 7;

    /* Both stale root and stale generation must fail before another envelope
     * can be made authoritative. */
    if (ops.publish_catalog_durable(ops.context, 1u, 4u, UINT64_C(2), 10u, UINT64_C(3))) return 8;
    if (ops.publish_catalog_durable(ops.context, 1u, 9u, UINT64_C(1), 10u, UINT64_C(2))) return 9;
    if (!read_state(main_path, &decoded, &snapshot)) return 10;
    if (decoded.schemas[0].root_page_num != 9u ||
        decoded.schemas[0].columns[1].size != 128u ||
        snapshot.entries[0].schema_generation != UINT64_C(2)) return 11;

    (void)remove(main_path);
    puts("schema_shape_and_root_atomic=yes");
    puts("schema_generation_advanced=yes");
    puts("stale_publication_rejected=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-schema-repack-v3-publish-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\nproject(TinyDBSchemaRepackV3Publish C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS)\n"
            "else()\n  add_compile_options(-Wall -Wextra -Werror -D_XOPEN_SOURCE=700)\nendif()\n"
            "add_executable(schema_repack_v3_publish probe.c \"" + (ROOT / "src" / "schema_catalog_v2.c").as_posix() + "\")\n"
            f'target_include_directories(schema_repack_v3_publish PRIVATE "{(ROOT / "src").as_posix()}")\n'
            "if(NOT MSVC)\n  target_link_libraries(schema_repack_v3_publish PRIVATE pthread)\nendif()\n",
            encoding="utf-8",
        )
        build = tmp_path / "build"
        cfg = subprocess.run(["cmake", "-S", str(tmp_path), "-B", str(build)], capture_output=True, text=True, timeout=60)
        assert cfg.returncode == 0, cfg.stdout + cfg.stderr
        comp = subprocess.run(["cmake", "--build", str(build), "--config", "Debug"], capture_output=True, text=True, timeout=120)
        assert comp.returncode == 0, comp.stdout + comp.stderr
        exe = build / ("Debug/schema_repack_v3_publish.exe" if shutil.which("cl") else "schema_repack_v3_publish")
        db_prefix = tmp_path / "catalog"
        run = subprocess.run([str(exe), str(db_prefix)], capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, run.stdout + run.stderr
        assert "schema_shape_and_root_atomic=yes" in run.stdout
        assert "schema_generation_advanced=yes" in run.stdout
        assert "stale_publication_rejected=yes" in run.stdout
