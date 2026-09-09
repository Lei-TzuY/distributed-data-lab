import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRODUCTION = ROOT / "src" / "schema_catalog_v2.c"


def test_production_uses_shared_shape_codec():
    text = PRODUCTION.read_text(encoding="utf-8")
    assert '#include "schema_catalog_shape_codec.h"' in text
    assert "tinydb_schema_catalog_shape_encode(" in text
    assert "tinydb_schema_catalog_shape_decode(" in text
    assert "static bool encode_snapshot(" not in text
    assert "static bool decode_snapshot(" not in text
    assert "typedef struct {\n    unsigned char* data;" not in text


def test_v2_shape_bytes_remain_legacy_compatible():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for V2 shape compatibility regression")

    source = r'''#include "schema_catalog_shape_codec.h"
#include <stdio.h>
#include <string.h>

static int check(int condition, const char* msg) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", msg);
    return 0;
}

static void legacy_u32(unsigned char* out, size_t* pos, uint32_t value) {
    out[(*pos)++] = (unsigned char)(value & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 8) & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 16) & 0xffu);
    out[(*pos)++] = (unsigned char)((value >> 24) & 0xffu);
}

static void legacy_bytes(unsigned char* out, size_t* pos, const void* data, size_t size) {
    memcpy(out + *pos, data, size);
    *pos += size;
}

static size_t legacy_encode(const Catalog* catalog, unsigned char* out) {
    size_t pos = 0u;
    legacy_u32(out, &pos, catalog->num_tables);
    legacy_u32(out, &pos, catalog->num_views);
    for (uint32_t i = 0; i < catalog->num_tables; i++) {
        const TableSchema* schema = &catalog->schemas[i];
        legacy_bytes(out, &pos, schema->name, sizeof(schema->name));
        legacy_u32(out, &pos, schema->root_page_num);
        legacy_u32(out, &pos, schema->num_columns);
        for (uint32_t j = 0; j < schema->num_columns; j++) {
            const TableColumn* col = &schema->columns[j];
            legacy_bytes(out, &pos, col->name, sizeof(col->name));
            legacy_u32(out, &pos, (uint32_t)col->type);
            legacy_u32(out, &pos, col->size);
            legacy_u32(out, &pos, col->offset);
        }
        legacy_u32(out, &pos, schema->row_size);
        out[pos++] = schema->has_fk ? 1u : 0u;
        legacy_bytes(out, &pos, schema->fk_col, sizeof(schema->fk_col));
        legacy_bytes(out, &pos, schema->fk_parent_table, sizeof(schema->fk_parent_table));
        legacy_bytes(out, &pos, schema->fk_parent_col, sizeof(schema->fk_parent_col));
        out[pos++] = schema->fk_on_delete_cascade ? 1u : 0u;
    }
    for (uint32_t i = 0; i < catalog->num_views; i++) {
        legacy_bytes(out, &pos, catalog->views[i].name, sizeof(catalog->views[i].name));
        legacy_bytes(out, &pos, catalog->views[i].select_sql, sizeof(catalog->views[i].select_sql));
    }
    return pos;
}

int main(void) {
    Catalog catalog;
    Catalog decoded;
    unsigned char legacy[TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE];
    unsigned char shared[TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE];
    size_t legacy_size;
    size_t shared_size = 0u;
    memset(&catalog, 0, sizeof(catalog));
    memset(legacy, 0, sizeof(legacy));
    memset(shared, 0, sizeof(shared));

    catalog.num_tables = 2u;
    catalog.num_views = 1u;

    strcpy(catalog.schemas[0].name, "narrow");
    catalog.schemas[0].root_page_num = 4u;
    catalog.schemas[0].num_columns = 1u;
    strcpy(catalog.schemas[0].columns[0].name, "id");
    catalog.schemas[0].columns[0].type = COL_TYPE_INT;
    catalog.schemas[0].columns[0].size = 4u;
    catalog.schemas[0].columns[0].offset = 0u;
    catalog.schemas[0].row_size = 4u;

    strcpy(catalog.schemas[1].name, "wide");
    catalog.schemas[1].root_page_num = 17u;
    catalog.schemas[1].num_columns = 3u;
    strcpy(catalog.schemas[1].columns[0].name, "id");
    catalog.schemas[1].columns[0].type = COL_TYPE_INT;
    catalog.schemas[1].columns[0].size = 4u;
    catalog.schemas[1].columns[0].offset = 0u;
    strcpy(catalog.schemas[1].columns[1].name, "title");
    catalog.schemas[1].columns[1].type = COL_TYPE_VARCHAR;
    catalog.schemas[1].columns[1].size = 128u;
    catalog.schemas[1].columns[1].offset = 4u;
    strcpy(catalog.schemas[1].columns[2].name, "body");
    catalog.schemas[1].columns[2].type = COL_TYPE_VARCHAR;
    catalog.schemas[1].columns[2].size = 384u;
    catalog.schemas[1].columns[2].offset = 132u;
    catalog.schemas[1].row_size = 516u;
    catalog.schemas[1].has_fk = true;
    strcpy(catalog.schemas[1].fk_col, "id");
    strcpy(catalog.schemas[1].fk_parent_table, "narrow");
    strcpy(catalog.schemas[1].fk_parent_col, "id");
    catalog.schemas[1].fk_on_delete_cascade = true;

    strcpy(catalog.views[0].name, "wide_view");
    strcpy(catalog.views[0].select_sql, "select id,title from wide");

    legacy_size = legacy_encode(&catalog, legacy);
    if (!check(tinydb_schema_catalog_shape_encode(&catalog, shared, sizeof(shared), &shared_size),
               "shared shape encode failed")) return 1;
    if (!check(shared_size == legacy_size, "V2 shape size changed")) return 2;
    if (!check(memcmp(shared, legacy, shared_size) == 0, "V2 shape bytes changed")) return 3;
    if (!check(tinydb_schema_catalog_shape_decode(legacy, legacy_size, &decoded),
               "shared decoder rejected legacy V2 bytes")) return 4;
    if (!check(decoded.num_tables == 2u && decoded.num_views == 1u &&
               decoded.schemas[0].row_size == 4u && decoded.schemas[1].row_size == 516u &&
               decoded.schemas[1].has_fk && decoded.schemas[1].fk_on_delete_cascade,
               "legacy V2 bytes decoded incorrectly")) return 5;

    puts("v2_bytes_identical=yes");
    puts("legacy_decode_compatible=yes");
    puts("different_schema_shapes=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-v2-shape-compat-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBV2ShapeCompat C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(v2_shape_compat probe.c)\n"
            f'target_include_directories(v2_shape_compat PRIVATE "{(ROOT / "src").as_posix()}")\n',
            encoding="utf-8",
        )
        build = tmp_path / "build"
        configure = subprocess.run(["cmake", "-S", str(tmp_path), "-B", str(build)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=60)
        assert configure.returncode == 0, configure.stdout + configure.stderr
        compiled = subprocess.run(["cmake", "--build", str(build), "--config", "Debug"], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=120)
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        executable = build / ("Debug/v2_shape_compat.exe" if shutil.which("cl") else "v2_shape_compat")
        run = subprocess.run([str(executable)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30)
        assert run.returncode == 0, run.stdout + run.stderr
        for token in ["v2_bytes_identical=yes", "legacy_decode_compatible=yes", "different_schema_shapes=yes"]:
            assert token in run.stdout
