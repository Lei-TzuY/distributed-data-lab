import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "schema_catalog_v3.h"


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
        "TINYDB_SCHEMA_CATALOG_V3_VERSION",
        "TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE",
        "tinydb_schema_catalog_v3_checksum(",
        "tinydb_schema_catalog_v3_encode(",
        "tinydb_schema_catalog_v3_decode(",
        "tinydb_schema_catalog_generation_is_valid(catalog, &decoded)",
        "input_size != expected_size",
    ]:
        assert token in text


def test_v3_metadata_runtime_contract():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for schema-catalog V3 regression")

    source = r'''#include "schema_catalog_v3.h"
#include <stdio.h>
#include <string.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

int main(void) {
    Catalog catalog;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    TinyDBSchemaCatalogGenerationSnapshot decoded;
    unsigned char encoded[TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE];
    unsigned char corrupt[TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE];
    size_t encoded_size = 0u;
    memset(&catalog, 0, sizeof(catalog));
    catalog.num_tables = 2u;
    strcpy(catalog.schemas[0].name, "narrow");
    catalog.schemas[0].root_page_num = 3u;
    catalog.schemas[0].num_columns = 1u;
    catalog.schemas[0].row_size = 4u;
    strcpy(catalog.schemas[1].name, "wide");
    catalog.schemas[1].root_page_num = 9u;
    catalog.schemas[1].num_columns = 3u;
    catalog.schemas[1].row_size = 132u;

    if (!expect(tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &snapshot),
                "legacy metadata bootstrap failed")) return 1;
    snapshot.entries[0].schema_generation = UINT64_C(4);
    snapshot.entries[1].schema_generation = UINT64_C(12);

    if (!expect(tinydb_schema_catalog_v3_encode(&catalog, &snapshot, encoded,
                                                 sizeof(encoded), &encoded_size),
                "V3 encode failed")) return 2;
    if (!expect(encoded_size == TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE +
                               2u * TINYDB_SCHEMA_CATALOG_V3_ENTRY_SIZE +
                               TINYDB_SCHEMA_CATALOG_V3_CHECKSUM_SIZE,
                "V3 encoded size mismatch")) return 3;

    if (!expect(tinydb_schema_catalog_v3_decode(&catalog, encoded, encoded_size, &decoded) ==
                    TINYDB_SCHEMA_CATALOG_V3_DECODE_OK,
                "V3 round trip decode failed")) return 4;
    if (!expect(decoded.entries[0].table_id == 1u &&
                decoded.entries[0].root_page_num == 3u &&
                decoded.entries[0].schema_generation == UINT64_C(4) &&
                decoded.entries[1].table_id == 2u &&
                decoded.entries[1].root_page_num == 9u &&
                decoded.entries[1].schema_generation == UINT64_C(12),
                "V3 identity/generation round trip mismatch")) return 5;

    memcpy(corrupt, encoded, encoded_size);
    corrupt[TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE + 8u] ^= 0x40u;
    if (!expect(tinydb_schema_catalog_v3_decode(&catalog, corrupt, encoded_size, &decoded) ==
                    TINYDB_SCHEMA_CATALOG_V3_DECODE_INVALID,
                "checksum corruption accepted")) return 6;
    if (!expect(decoded.num_tables == 0u,
                "corrupt decode leaked partial output")) return 7;

    if (!expect(tinydb_schema_catalog_v3_decode(&catalog, encoded, encoded_size - 1u, &decoded) ==
                    TINYDB_SCHEMA_CATALOG_V3_DECODE_TRUNCATED,
                "truncated V3 block accepted")) return 8;

    memcpy(corrupt, encoded, encoded_size);
    tinydb_schema_catalog_v3_put_u32(corrupt + TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE +
                                     TINYDB_SCHEMA_CATALOG_V3_ENTRY_SIZE,
                                     1u);
    tinydb_schema_catalog_v3_put_u64(
        corrupt + encoded_size - TINYDB_SCHEMA_CATALOG_V3_CHECKSUM_SIZE,
        tinydb_schema_catalog_v3_checksum(
            corrupt, encoded_size - TINYDB_SCHEMA_CATALOG_V3_CHECKSUM_SIZE));
    if (!expect(tinydb_schema_catalog_v3_decode(&catalog, corrupt, encoded_size, &decoded) ==
                    TINYDB_SCHEMA_CATALOG_V3_DECODE_INVALID,
                "duplicate table identity accepted")) return 9;

    Catalog mismatched = catalog;
    mismatched.schemas[1].root_page_num = 10u;
    if (!expect(tinydb_schema_catalog_v3_decode(&mismatched, encoded, encoded_size, &decoded) ==
                    TINYDB_SCHEMA_CATALOG_V3_DECODE_INVALID,
                "root/catalog mismatch accepted")) return 10;

    puts("v3_roundtrip=yes");
    puts("checksum_fail_closed=yes");
    puts("duplicate_identity_rejected=yes");
    puts("root_crosscheck=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-schema-v3-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBSchemaV3Probe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(schema_v3_probe probe.c)\n"
            f'target_include_directories(schema_v3_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        )
        build = configure_and_build(tmp_path, cmake)
        executable = build / ("Debug/schema_v3_probe.exe" if shutil.which("cl") else "schema_v3_probe")
        run = subprocess.run(
            [str(executable)], capture_output=True, text=True,
            encoding="utf-8", errors="ignore", timeout=30,
        )
        assert run.returncode == 0, run.stdout + run.stderr
        for token in [
            "v3_roundtrip=yes",
            "checksum_fail_closed=yes",
            "duplicate_identity_rejected=yes",
            "root_crosscheck=yes",
        ]:
            assert token in run.stdout
