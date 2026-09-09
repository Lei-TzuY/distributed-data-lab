import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "schema_catalog_generation.h"


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
        "TINYDB_SCHEMA_GENERATION_INITIAL",
        "tinydb_schema_catalog_generation_bootstrap_legacy(",
        "tinydb_schema_catalog_generation_is_valid(",
        "tinydb_schema_catalog_generation_authoritative(",
        "tinydb_schema_catalog_generation_publish_replacement(",
        "expected_old_generation == UINT64_MAX",
        "entry->schema_generation = expected_old_generation + UINT64_C(1)",
    ]:
        assert token in text


def test_schema_generation_runtime_contract():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for schema-generation regression")

    source = r'''#include "schema_catalog_generation.h"
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
    uint32_t root = 999u;
    uint64_t generation = UINT64_C(999);
    memset(&catalog, 0, sizeof(catalog));
    catalog.num_tables = 2u;
    strcpy(catalog.schemas[0].name, "alpha");
    catalog.schemas[0].root_page_num = 0u;
    catalog.schemas[0].num_columns = 1u;
    strcpy(catalog.schemas[1].name, "beta");
    catalog.schemas[1].root_page_num = 7u;
    catalog.schemas[1].num_columns = 1u;

    if (!expect(tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &snapshot),
                "legacy bootstrap failed")) return 1;
    if (!expect(snapshot.entries[0].table_id == 1u &&
                snapshot.entries[1].table_id == 2u &&
                snapshot.entries[0].schema_generation == UINT64_C(1) &&
                snapshot.entries[1].schema_generation == UINT64_C(1),
                "bootstrap identity/generation mismatch")) return 2;

    if (!expect(tinydb_schema_catalog_generation_authoritative(
                    &catalog, &snapshot, 2u, &root, &generation),
                "authoritative lookup failed")) return 3;
    if (!expect(root == 7u && generation == UINT64_C(1),
                "authoritative state mismatch")) return 4;

    if (!expect(tinydb_schema_catalog_generation_publish_replacement(
                    &catalog, &snapshot, 2u, 7u, UINT64_C(1), 11u),
                "replacement publication failed")) return 5;
    if (!expect(catalog.schemas[1].root_page_num == 11u &&
                snapshot.entries[1].root_page_num == 11u &&
                snapshot.entries[1].schema_generation == UINT64_C(2),
                "root/generation were not published together")) return 6;

    if (!expect(!tinydb_schema_catalog_generation_publish_replacement(
                    &catalog, &snapshot, 2u, 7u, UINT64_C(1), 12u),
                "stale expected state was accepted")) return 7;
    if (!expect(catalog.schemas[1].root_page_num == 11u &&
                snapshot.entries[1].schema_generation == UINT64_C(2),
                "failed publication mutated state")) return 8;

    snapshot.entries[1].table_id = snapshot.entries[0].table_id;
    root = 99u;
    generation = UINT64_C(99);
    if (!expect(!tinydb_schema_catalog_generation_authoritative(
                    &catalog, &snapshot, 2u, &root, &generation),
                "duplicate table identity was accepted")) return 9;
    if (!expect(root == 0u && generation == UINT64_C(0),
                "failed lookup exposed partial output")) return 10;

    puts("legacy_bootstrap=yes");
    puts("monotonic_publication=yes");
    puts("stale_publish_rejected=yes");
    puts("duplicate_identity_fail_closed=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-schema-generation-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBSchemaGenerationProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(schema_generation_probe probe.c)\n"
            f'target_include_directories(schema_generation_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        )
        build = configure_and_build(tmp_path, cmake)
        executable = build / ("Debug/schema_generation_probe.exe" if shutil.which("cl") else "schema_generation_probe")
        run = subprocess.run(
            [str(executable)], capture_output=True, text=True,
            encoding="utf-8", errors="ignore", timeout=30,
        )
        assert run.returncode == 0, run.stdout + run.stderr
        for token in [
            "legacy_bootstrap=yes",
            "monotonic_publication=yes",
            "stale_publish_rejected=yes",
            "duplicate_identity_fail_closed=yes",
        ]:
            assert token in run.stdout
