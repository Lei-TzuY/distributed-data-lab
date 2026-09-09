import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "schema_catalog_v3_store.h"


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
        "TINYDB_SCHEMA_CATALOG_V3_WAL_COMMIT_MAGIC",
        "tinydb_schema_catalog_v3_store_read(",
        "tinydb_schema_catalog_v3_store_write_file(",
        "tinydb_schema_catalog_v3_store_publish(",
        "tinydb_schema_catalog_v3_store_recover(",
        "require_commit_marker",
        "tinydb_schema_catalog_v3_envelope_decode(",
        "*recovered_out = true",
    ]:
        assert token in text


def test_v3_store_runtime_contract():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for schema-catalog V3 store regression")

    source = r'''#include "schema_catalog_v3_store.h"
#include <stdio.h>
#include <string.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

static int build_envelope(uint32_t root, uint64_t generation,
                          unsigned char* output, size_t* output_size) {
    Catalog catalog;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    unsigned char identity[TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE];
    size_t identity_size = 0u;
    const unsigned char shape[] = {0x01u, 0x00u, 0x00u, 0x00u,
                                   0x74u, 0x00u, 0x00u, 0x00u};
    memset(&catalog, 0, sizeof(catalog));
    catalog.num_tables = 1u;
    strcpy(catalog.schemas[0].name, "t");
    catalog.schemas[0].root_page_num = root;
    catalog.schemas[0].num_columns = 1u;
    catalog.schemas[0].row_size = 4u;
    if (!tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &snapshot)) return 0;
    snapshot.entries[0].schema_generation = generation;
    if (!tinydb_schema_catalog_v3_encode(&catalog, &snapshot,
                                          identity, sizeof(identity),
                                          &identity_size)) return 0;
    return tinydb_schema_catalog_v3_envelope_encode(
        shape, sizeof(shape), identity, identity_size,
        output, TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE, output_size);
}

int main(int argc, char** argv) {
    if (argc != 2) return 100;
    char main_path[768];
    char wal_path[768];
    unsigned char old_env[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    unsigned char new_env[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    unsigned char loaded[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    unsigned char workspace[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    size_t old_size = 0u, new_size = 0u, loaded_size = 0u;
    bool recovered = false;

    if (snprintf(main_path, sizeof(main_path), "%s.schema", argv[1]) < 0 ||
        snprintf(wal_path, sizeof(wal_path), "%s.schema.wal", argv[1]) < 0) return 101;
    (void)remove(main_path);
    (void)remove(wal_path);

    if (!expect(build_envelope(4u, UINT64_C(1), old_env, &old_size), "old envelope build failed")) return 1;
    if (!expect(build_envelope(9u, UINT64_C(2), new_env, &new_size), "new envelope build failed")) return 2;
    if (!expect(tinydb_schema_catalog_v3_store_write_file(main_path, old_env, old_size, false),
                "initial main write failed")) return 3;

    if (!expect(tinydb_schema_catalog_v3_store_write_file(wal_path, new_env, new_size, true),
                "committed WAL write failed")) return 4;
    if (!expect(tinydb_schema_catalog_v3_store_recover(main_path, wal_path,
                                                        workspace, sizeof(workspace),
                                                        &recovered),
                "recovery failed")) return 5;
    if (!expect(recovered, "committed WAL was not reported recovered")) return 6;
    if (!expect(tinydb_schema_catalog_v3_store_read(main_path, false,
                                                     loaded, sizeof(loaded),
                                                     &loaded_size) ==
                TINYDB_SCHEMA_CATALOG_V3_STORE_READ_OK,
                "recovered main cannot be read")) return 7;
    if (!expect(loaded_size == new_size && memcmp(loaded, new_env, new_size) == 0,
                "recovered main did not match committed WAL")) return 8;

    /* An envelope without the commit marker models a crash before WAL commit.
     * Recovery must discard it and leave the durable main publication alone. */
    if (!expect(tinydb_schema_catalog_v3_store_write_file(wal_path, old_env, old_size, false),
                "uncommitted WAL fixture failed")) return 9;
    recovered = true;
    if (!expect(tinydb_schema_catalog_v3_store_recover(main_path, wal_path,
                                                        workspace, sizeof(workspace),
                                                        &recovered),
                "uncommitted WAL cleanup failed")) return 10;
    if (!expect(!recovered, "uncommitted WAL was incorrectly published")) return 11;
    if (!expect(tinydb_schema_catalog_v3_store_read(main_path, false,
                                                     loaded, sizeof(loaded),
                                                     &loaded_size) ==
                TINYDB_SCHEMA_CATALOG_V3_STORE_READ_OK &&
                loaded_size == new_size && memcmp(loaded, new_env, new_size) == 0,
                "uncommitted WAL changed main publication")) return 12;

    /* A corrupt committed-looking WAL must also fail closed. */
    memcpy(workspace, old_env, old_size);
    workspace[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE + 1u] ^= 0x80u;
    if (!expect(tinydb_schema_catalog_v3_store_write_file(wal_path, old_env, old_size, true),
                "corrupt WAL base write failed")) return 13;
    FILE* corrupt = fopen(wal_path, "r+b");
    if (corrupt == NULL) return 14;
    if (fseek(corrupt, (long)(TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE + 1u), SEEK_SET) != 0 ||
        fwrite(workspace + TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE + 1u, 1, 1, corrupt) != 1) return 15;
    fclose(corrupt);
    recovered = true;
    if (!expect(tinydb_schema_catalog_v3_store_recover(main_path, wal_path,
                                                        workspace, sizeof(workspace),
                                                        &recovered),
                "corrupt WAL cleanup failed")) return 16;
    if (!expect(!recovered, "corrupt WAL was incorrectly published")) return 17;

    (void)remove(main_path);
    (void)remove(wal_path);
    puts("committed_wal_recovery=yes");
    puts("uncommitted_wal_fail_closed=yes");
    puts("corrupt_wal_fail_closed=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-schema-v3-store-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBSchemaV3StoreProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(schema_v3_store_probe probe.c)\n"
            f'target_include_directories(schema_v3_store_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        )
        build = configure_and_build(tmp_path, cmake)
        executable = build / ("Debug/schema_v3_store_probe.exe" if shutil.which("cl") else "schema_v3_store_probe")
        db_prefix = tmp_path / "catalog_fixture"
        run = subprocess.run(
            [str(executable), str(db_prefix)], capture_output=True, text=True,
            encoding="utf-8", errors="ignore", timeout=30,
        )
        assert run.returncode == 0, run.stdout + run.stderr
        for token in [
            "committed_wal_recovery=yes",
            "uncommitted_wal_fail_closed=yes",
            "corrupt_wal_fail_closed=yes",
        ]:
            assert token in run.stdout
