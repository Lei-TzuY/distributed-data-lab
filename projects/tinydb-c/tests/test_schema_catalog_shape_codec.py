import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "schema_catalog_shape_codec.h"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_schema_catalog_shape_valid(",
        "tinydb_schema_catalog_shape_encode(",
        "tinydb_schema_catalog_shape_decode(",
        "TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE",
    ]:
        assert token in text


def test_shape_codec_compiles_and_executes():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for schema shape regression")

    source = r'''#include "schema_catalog_shape_codec.h"
#include <stdio.h>
#include <string.h>

static int check(int condition, const char* msg) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", msg);
    return 0;
}

int main(void) {
    Catalog input;
    Catalog decoded;
    unsigned char payload[TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE];
    size_t payload_size = 0u;
    memset(&input, 0, sizeof(input));

    input.num_tables = 2u;
    strcpy(input.schemas[0].name, "narrow");
    input.schemas[0].root_page_num = 4u;
    input.schemas[0].num_columns = 1u;
    strcpy(input.schemas[0].columns[0].name, "id");
    input.schemas[0].columns[0].type = COL_TYPE_INT;
    input.schemas[0].columns[0].size = 4u;
    input.schemas[0].columns[0].offset = 0u;
    input.schemas[0].row_size = 4u;

    strcpy(input.schemas[1].name, "wide");
    input.schemas[1].root_page_num = 17u;
    input.schemas[1].num_columns = 3u;
    strcpy(input.schemas[1].columns[0].name, "id");
    input.schemas[1].columns[0].type = COL_TYPE_INT;
    input.schemas[1].columns[0].size = 4u;
    input.schemas[1].columns[0].offset = 0u;
    strcpy(input.schemas[1].columns[1].name, "title");
    input.schemas[1].columns[1].type = COL_TYPE_VARCHAR;
    input.schemas[1].columns[1].size = 128u;
    input.schemas[1].columns[1].offset = 4u;
    strcpy(input.schemas[1].columns[2].name, "body");
    input.schemas[1].columns[2].type = COL_TYPE_VARCHAR;
    input.schemas[1].columns[2].size = 384u;
    input.schemas[1].columns[2].offset = 132u;
    input.schemas[1].row_size = 516u;

    if (!check(tinydb_schema_catalog_shape_encode(&input, payload, sizeof(payload), &payload_size),
               "shape encode failed")) return 1;
    if (!check(payload_size > 0u && tinydb_schema_catalog_shape_decode(payload, payload_size, &decoded),
               "shape decode failed")) return 2;
    if (!check(decoded.num_tables == 2u && decoded.schemas[0].root_page_num == 4u &&
               decoded.schemas[0].row_size == 4u && decoded.schemas[1].root_page_num == 17u &&
               decoded.schemas[1].row_size == 516u && decoded.schemas[1].num_columns == 3u,
               "different physical schemas did not round-trip")) return 3;

    Catalog invalid = input;
    invalid.schemas[1].columns[2].offset = 500u;
    invalid.schemas[1].columns[2].size = 384u;
    payload_size = 99u;
    if (!check(!tinydb_schema_catalog_shape_encode(&invalid, payload, sizeof(payload), &payload_size) &&
               payload_size == 0u,
               "out-of-bounds column accepted")) return 4;

    memset(&decoded, 0x7f, sizeof(decoded));
    if (!check(!tinydb_schema_catalog_shape_decode(payload, 3u, &decoded) && decoded.num_tables == 0u,
               "truncated decode leaked output")) return 5;

    if (!check(tinydb_schema_catalog_shape_encode(&input, payload, sizeof(payload), &payload_size),
               "second encode failed")) return 6;
    payload[payload_size++] = 0xa5u;
    memset(&decoded, 0x7f, sizeof(decoded));
    if (!check(!tinydb_schema_catalog_shape_decode(payload, payload_size, &decoded) && decoded.num_tables == 0u,
               "trailing data accepted")) return 7;

    puts("different_schema_shapes=yes");
    puts("bounds_fail_closed=yes");
    puts("trailing_rejected=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-schema-shape-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBSchemaShapeProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(schema_shape_probe probe.c)\n"
            f'target_include_directories(schema_shape_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
            encoding="utf-8",
        )
        build = tmp_path / "build"
        configure = subprocess.run(["cmake", "-S", str(tmp_path), "-B", str(build)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=60)
        assert configure.returncode == 0, configure.stdout + configure.stderr
        compiled = subprocess.run(["cmake", "--build", str(build), "--config", "Debug"], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=120)
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        executable = build / ("Debug/schema_shape_probe.exe" if shutil.which("cl") else "schema_shape_probe")
        run = subprocess.run([str(executable)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30)
        assert run.returncode == 0, run.stdout + run.stderr
        for token in ["different_schema_shapes=yes", "bounds_fail_closed=yes", "trailing_rejected=yes"]:
            assert token in run.stdout
