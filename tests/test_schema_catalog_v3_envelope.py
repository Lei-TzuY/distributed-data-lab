import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "schema_catalog_v3_envelope.h"


def configure_and_build(tmp_path: Path, cmake_text: str):
    (tmp_path / "CMakeLists.txt").write_text(cmake_text, encoding="utf-8")
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


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_VERSION",
        "TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SHAPE_SIZE",
        "tinydb_schema_catalog_v3_envelope_encode(",
        "tinydb_schema_catalog_v3_envelope_decode(",
        "tinydb_schema_catalog_v3_envelope_decode_identity(",
        "tinydb_schema_catalog_v3_decode(decoded_shape_catalog",
        "input_size != expected_size",
    ]:
        assert token in text


def test_v3_envelope_runtime_contract():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for schema-catalog V3 envelope regression")

    source = r'''#include "schema_catalog_v3_envelope.h"
#include <stdio.h>
#include <string.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

int main(void) {
    Catalog catalog;
    Catalog mismatched;
    TinyDBSchemaCatalogGenerationSnapshot generation;
    TinyDBSchemaCatalogGenerationSnapshot decoded;
    TinyDBSchemaCatalogV3EnvelopeView view;
    unsigned char identity[TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE];
    unsigned char envelope[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    unsigned char corrupt[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    const unsigned char shape[] = {0x02u, 0x00u, 0x00u, 0x00u,
                                   0x6eu, 0x61u, 0x72u, 0x72u,
                                   0x6fu, 0x77u, 0x00u, 0x00u,
                                   0x77u, 0x69u, 0x64u, 0x65u};
    size_t identity_size = 0u;
    size_t envelope_size = 0u;

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

    if (!expect(tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &generation),
                "generation bootstrap failed")) return 1;
    generation.entries[0].schema_generation = UINT64_C(5);
    generation.entries[1].schema_generation = UINT64_C(19);
    if (!expect(tinydb_schema_catalog_v3_encode(&catalog, &generation,
                                                 identity, sizeof(identity),
                                                 &identity_size),
                "identity encode failed")) return 2;
    if (!expect(tinydb_schema_catalog_v3_envelope_encode(
                    shape, sizeof(shape), identity, identity_size,
                    envelope, sizeof(envelope), &envelope_size),
                "envelope encode failed")) return 3;

    if (!expect(tinydb_schema_catalog_v3_envelope_decode_identity(
                    &catalog, envelope, envelope_size, &decoded, &view) ==
                    TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK,
                "envelope round trip failed")) return 4;
    if (!expect(view.shape_size == sizeof(shape) &&
                memcmp(view.shape, shape, sizeof(shape)) == 0 &&
                view.identity_size == identity_size,
                "section boundaries changed")) return 5;
    if (!expect(decoded.entries[0].table_id == 1u &&
                decoded.entries[0].root_page_num == 4u &&
                decoded.entries[0].schema_generation == UINT64_C(5) &&
                decoded.entries[1].table_id == 2u &&
                decoded.entries[1].root_page_num == 17u &&
                decoded.entries[1].schema_generation == UINT64_C(19),
                "identity generations changed")) return 6;

    memcpy(corrupt, envelope, envelope_size);
    corrupt[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE + 3u] ^= 0x80u;
    if (!expect(tinydb_schema_catalog_v3_envelope_decode_identity(
                    &catalog, corrupt, envelope_size, &decoded, &view) ==
                    TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID,
                "outer checksum corruption accepted")) return 7;
    if (!expect(decoded.num_tables == 0u,
                "corrupt envelope leaked generation output")) return 8;

    if (!expect(tinydb_schema_catalog_v3_envelope_decode_identity(
                    &catalog, envelope, envelope_size - 1u, &decoded, &view) ==
                    TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_TRUNCATED,
                "truncated envelope accepted")) return 9;
    if (!expect(decoded.num_tables == 0u,
                "truncated envelope leaked generation output")) return 10;

    memcpy(corrupt, envelope, envelope_size);
    tinydb_schema_catalog_v3_put_u32(corrupt + 12u, (uint32_t)(sizeof(shape) + 1u));
    tinydb_schema_catalog_v3_put_u64(
        corrupt + envelope_size - TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE,
        tinydb_schema_catalog_v3_checksum(
            corrupt, envelope_size - TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE));
    if (!expect(tinydb_schema_catalog_v3_envelope_decode_identity(
                    &catalog, corrupt, envelope_size, &decoded, &view) ==
                    TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID,
                "section length drift accepted")) return 11;

    mismatched = catalog;
    mismatched.schemas[1].root_page_num = 18u;
    if (!expect(tinydb_schema_catalog_v3_envelope_decode_identity(
                    &mismatched, envelope, envelope_size, &decoded, &view) ==
                    TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID,
                "identity/root mismatch accepted")) return 12;
    if (!expect(decoded.num_tables == 0u,
                "root mismatch leaked generation output")) return 13;

    memcpy(corrupt, envelope, envelope_size);
    size_t identity_offset = TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE + sizeof(shape);
    corrupt[identity_offset + TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE + 8u] ^= 0x01u;
    tinydb_schema_catalog_v3_put_u64(
        corrupt + envelope_size - TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE,
        tinydb_schema_catalog_v3_checksum(
            corrupt, envelope_size - TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE));
    if (!expect(tinydb_schema_catalog_v3_envelope_decode_identity(
                    &catalog, corrupt, envelope_size, &decoded, &view) ==
                    TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID,
                "inner identity checksum corruption accepted")) return 14;

    puts("atomic_shape_identity=yes");
    puts("outer_checksum_fail_closed=yes");
    puts("inner_checksum_fail_closed=yes");
    puts("root_generation_crosscheck=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-schema-v3-envelope-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBSchemaV3EnvelopeProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(schema_v3_envelope_probe probe.c)\n"
            f'target_include_directories(schema_v3_envelope_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        )
        build = configure_and_build(tmp_path, cmake)
        executable = build / ("Debug/schema_v3_envelope_probe.exe" if shutil.which("cl") else "schema_v3_envelope_probe")
        run = subprocess.run(
            [str(executable)], capture_output=True, text=True,
            encoding="utf-8", errors="ignore", timeout=30,
        )
        assert run.returncode == 0, run.stdout + run.stderr
        for token in [
            "atomic_shape_identity=yes",
            "outer_checksum_fail_closed=yes",
            "inner_checksum_fail_closed=yes",
            "root_generation_crosscheck=yes",
        ]:
            assert token in run.stdout
