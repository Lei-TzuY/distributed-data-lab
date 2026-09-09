import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_v3_catalog_wal_is_the_durable_commit_boundary():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required")

    source = r'''#include "schema_catalog_shape_codec.h"
#include "schema_catalog_v3_store.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif

static int build_envelope(unsigned char* out, size_t* out_size) {
    Catalog catalog;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    unsigned char shape[TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE];
    unsigned char identity[TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE];
    size_t shape_size = 0u, identity_size = 0u;
    memset(&catalog, 0, sizeof(catalog));
    catalog.num_tables = 1u;
    strcpy(catalog.schemas[0].name, "items");
    catalog.schemas[0].root_page_num = 9u;
    catalog.schemas[0].num_columns = 1u;
    strcpy(catalog.schemas[0].columns[0].name, "id");
    catalog.schemas[0].columns[0].type = COL_TYPE_INT;
    catalog.schemas[0].columns[0].size = 4u;
    catalog.schemas[0].columns[0].offset = 0u;
    catalog.schemas[0].row_size = 4u;
    if (!tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &snapshot)) return 0;
    snapshot.entries[0].schema_generation = UINT64_C(2);
    if (!tinydb_schema_catalog_shape_encode(&catalog, shape, sizeof(shape), &shape_size) ||
        !tinydb_schema_catalog_v3_encode(&catalog, &snapshot, identity, sizeof(identity), &identity_size)) return 0;
    return tinydb_schema_catalog_v3_envelope_encode(shape, shape_size, identity, identity_size,
                                                     out, TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE, out_size);
}

int main(int argc, char** argv) {
    unsigned char envelope[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    unsigned char recovered[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    size_t envelope_size = 0u, recovered_size = 0u;
    TinyDBSchemaCatalogV3StorePublishResult result;
    bool did_recover = false;
    char main_path[768], wal_path[768];
    if (argc != 2) return 100;
    if (snprintf(main_path, sizeof(main_path), "%s/main.schema", argv[1]) < 0 ||
        snprintf(wal_path, sizeof(wal_path), "%s/catalog.wal", argv[1]) < 0) return 101;
    if (!build_envelope(envelope, &envelope_size)) return 1;

    /* main_path names an existing directory, forcing the post-commit main copy
     * to fail while the WAL path remains writable. */
    if (!tinydb_schema_catalog_v3_store_publish_detailed(
            main_path, wal_path, envelope, envelope_size, &result)) return 2;
    if (!result.wal_committed_durable || result.main_published_durable || result.cleanup_complete) return 3;
    if (remove(main_path) == 0) return 4;
#ifdef _WIN32
    if (_rmdir(main_path) != 0) return 5;
#else
    if (rmdir(main_path) != 0) return 5;
#endif
    if (!tinydb_schema_catalog_v3_store_recover(main_path, wal_path,
                                                 recovered, sizeof(recovered), &did_recover)) return 6;
    if (!did_recover) return 7;
    if (tinydb_schema_catalog_v3_store_read(main_path, false, recovered, sizeof(recovered),
                                             &recovered_size) != TINYDB_SCHEMA_CATALOG_V3_STORE_READ_OK) return 8;
    if (recovered_size != envelope_size || memcmp(recovered, envelope, envelope_size) != 0) return 9;
    (void)remove(main_path);
    puts("wal_commit_is_durable=yes");
    puts("main_copy_failure_is_recoverable=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-v3-commit-boundary-") as tmp:
        tmp_path = Path(tmp)
        fixture = tmp_path / "fixture"
        fixture.mkdir()
        (fixture / "main.schema").mkdir()
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\nproject(TinyDBV3CommitBoundary C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS)\n"
            "else()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(v3_commit_boundary probe.c)\n"
            f'target_include_directories(v3_commit_boundary PRIVATE "{(ROOT / "src").as_posix()}")\n',
            encoding="utf-8",
        )
        build = tmp_path / "build"
        cfg = subprocess.run(["cmake", "-S", str(tmp_path), "-B", str(build)], capture_output=True, text=True, timeout=60)
        assert cfg.returncode == 0, cfg.stdout + cfg.stderr
        comp = subprocess.run(["cmake", "--build", str(build), "--config", "Debug"], capture_output=True, text=True, timeout=120)
        assert comp.returncode == 0, comp.stdout + comp.stderr
        exe = build / ("Debug/v3_commit_boundary.exe" if shutil.which("cl") else "v3_commit_boundary")
        run = subprocess.run([str(exe), str(fixture)], capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, run.stdout + run.stderr
        assert "wal_commit_is_durable=yes" in run.stdout
        assert "main_copy_failure_is_recoverable=yes" in run.stdout
