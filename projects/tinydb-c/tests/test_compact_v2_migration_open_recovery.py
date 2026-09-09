import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_open_recovery.h"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_compact_v2_migration_manifest_load_file(",
        "TINYDB_COMPACT_V2_MIGRATION_OPEN_INVALID_MANIFEST",
        "TINYDB_COMPACT_V2_MIGRATION_OPEN_MANIFEST_IO_ERROR",
        "tinydb_compact_v2_migration_pager_recover_reopen(",
        "TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERY_FAILED",
        "memset(recovery_result_out, 0, sizeof(*recovery_result_out))",
    ]:
        assert token in text, f"missing open-recovery invariant: {token}"
    load = text.index("tinydb_compact_v2_migration_manifest_load_file(")
    recover = text.index("tinydb_compact_v2_migration_pager_recover_reopen(")
    assert load < recover


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


def compile_header_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for open-recovery regression")
    source = r'''#include "compact_v2_migration_open_recovery.h"

TinyDBCompactV2MigrationOpenRecoveryStatus open_recovery_header_probe(
    const char* filename,
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter,
    TinyDBCompactV2MigrationOpenRecoveryWorkspace* workspace,
    TinyDBCompactV2MigrationRecoveryResult* result) {
    return tinydb_compact_v2_migration_recover_open_file(
        filename, adapter, workspace, result, NULL, 0u);
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-open-recovery-compile-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBOpenRecoveryHeaderProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_library(open_recovery_header_probe OBJECT probe.c)\n"
            "if(MSVC)\n  target_compile_definitions(open_recovery_header_probe PRIVATE _CRT_SECURE_NO_WARNINGS)\nendif()\n"
            f'target_include_directories(open_recovery_header_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        )
        configure_and_build(tmp_path, cmake)


def run_real_open_recovery_probe_on_posix():
    if sys.platform.startswith("win"):
        print("open_recovery_runtime=covered_by_posix")
        return

    source = r'''#include "compact_v2_migration_open_recovery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ProbeContext {
    const char* database_filename;
    uint32_t root;
    uint64_t generation;
    int remove_calls;
    int sync_parent_calls;
} ProbeContext;

static bool read_catalog(void* ctx, uint32_t table_id, uint32_t* root, uint64_t* generation) {
    ProbeContext* p = (ProbeContext*)ctx;
    if (table_id != 9u) return false;
    *root = p->root;
    *generation = p->generation;
    return true;
}

static bool remove_manifest(void* ctx) {
    ProbeContext* p = (ProbeContext*)ctx;
    char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    p->remove_calls++;
    if (!tinydb_compact_v2_migration_manifest_file_path(
            p->database_filename, path, sizeof(path))) return false;
    return remove(path) == 0;
}

static bool sync_parent(void* ctx) {
    ProbeContext* p = (ProbeContext*)ctx;
    p->sync_parent_calls++;
    return true;
}

static int write_manifest(
    const char* database_filename,
    const TinyDBCompactV2MigrationManifest* manifest) {
    unsigned char encoded[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE];
    size_t encoded_length = 0u;
    char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    FILE* file;
    if (!tinydb_compact_v2_migration_manifest_encode(
            manifest, encoded, sizeof(encoded), &encoded_length)) return 0;
    if (!tinydb_compact_v2_migration_manifest_file_path(
            database_filename, path, sizeof(path))) return 0;
    file = fopen(path, "wb");
    if (file == NULL) return 0;
    if (fwrite(encoded, 1u, encoded_length, file) != encoded_length) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int write_corrupt_manifest(const char* database_filename) {
    char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    FILE* file;
    if (!tinydb_compact_v2_migration_manifest_file_path(
            database_filename, path, sizeof(path))) return 0;
    file = fopen(path, "wb");
    if (file == NULL) return 0;
    if (fwrite("bad", 1u, 3u, file) != 3u) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) return 100;
    const char* database = argv[1];
    Pager* pager = pager_open(database);
    void* page0 = get_page(pager, 0u);
    memset(page0, 0, PAGE_SIZE);
    mark_page_dirty(pager, 0u);
    pager_checkpoint(pager);

    pager_begin_transaction(pager);
    for (uint32_t expected = 1u; expected <= 3u; expected++) {
        uint32_t page_num = get_unused_page_num(pager);
        if (!expect(page_num == expected, "unexpected allocation")) return 1;
        void* page = get_page(pager, page_num);
        memset(page, (int)(0x30u + expected), PAGE_USABLE_SIZE);
        mark_page_dirty(pager, page_num);
    }
    pager_commit(pager);
    pager_checkpoint(pager);

    ProbeContext ctx = {database, 3u, UINT64_C(20), 0, 0};
    TinyDBCompactV2MigrationPagerRecoveryAdapter adapter = {
        pager, &ctx, read_catalog, NULL, remove_manifest, sync_parent, false
    };
    TinyDBCompactV2MigrationOpenRecoveryWorkspace* workspace =
        (TinyDBCompactV2MigrationOpenRecoveryWorkspace*)calloc(1u, sizeof(*workspace));
    TinyDBCompactV2MigrationRecoveryResult recovery;
    char message[160];
    if (workspace == NULL) return 2;

    TinyDBCompactV2MigrationOpenRecoveryStatus status =
        tinydb_compact_v2_migration_recover_open_file(
            database, &adapter, workspace, &recovery, message, sizeof(message));
    if (!expect(status == TINYDB_COMPACT_V2_MIGRATION_OPEN_NO_MIGRATION,
                "absent sidecar was not a no-op")) return 3;
    if (!expect(!pager->in_transaction && pager->free_page_count == 0u,
                "absent sidecar mutated Pager state")) return 4;

    if (!expect(write_corrupt_manifest(database), "unable to write corrupt sidecar")) return 5;
    status = tinydb_compact_v2_migration_recover_open_file(
        database, &adapter, workspace, &recovery, message, sizeof(message));
    if (!expect(status == TINYDB_COMPACT_V2_MIGRATION_OPEN_INVALID_MANIFEST,
                "corrupt sidecar did not fail closed")) return 6;
    if (!expect(!pager->in_transaction && pager->free_page_count == 0u &&
                ctx.remove_calls == 0 && ctx.sync_parent_calls == 0,
                "invalid sidecar crossed recovery boundary")) return 7;
    {
        char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
        if (!tinydb_compact_v2_migration_manifest_file_path(database, path, sizeof(path))) return 8;
        if (remove(path) != 0) return 9;
    }

    uint32_t claims[2] = {1u, 2u};
    TinyDBCompactV2MigrationManifest manifest = {
        9u, 3u, 1u, UINT64_C(20), UINT64_C(21),
        TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED,
        2u, claims
    };
    if (!expect(write_manifest(database, &manifest), "unable to write valid sidecar")) return 10;
    status = tinydb_compact_v2_migration_recover_open_file(
        database, &adapter, workspace, &recovery, message, sizeof(message));
    if (!expect(status == TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERED,
                "valid interrupted migration did not recover")) return 11;
    if (!expect(recovery.action == TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING,
                "unexpected recovery action")) return 12;
    if (!expect(!pager->in_transaction && pager->free_page_count == 2u &&
                ctx.remove_calls == 1 && ctx.sync_parent_calls == 1,
                "recovery did not durably reclaim then clean sidecar")) return 13;

    ctx.root = 3u;
    ctx.generation = UINT64_C(999);
    if (!expect(write_manifest(database, &manifest), "unable to rewrite sidecar")) return 14;
    status = tinydb_compact_v2_migration_recover_open_file(
        database, &adapter, workspace, &recovery, message, sizeof(message));
    if (!expect(status == TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERY_FAILED,
                "mixed catalog generation did not fail closed")) return 15;
    if (!expect(!pager->in_transaction && pager->free_page_count == 2u &&
                ctx.remove_calls == 1 && ctx.sync_parent_calls == 1,
                "failed recovery mutated durable allocator or removed sidecar")) return 16;

    free(workspace);
    if (!expect(pager_try_close(pager), "unable to close pager")) return 17;
    puts("absent_noop=yes");
    puts("invalid_fail_closed=yes");
    puts("valid_recovered=yes");
    puts("mixed_catalog_fail_closed=yes");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-open-recovery-runtime-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBOpenRecoveryRuntimeProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "find_package(Threads REQUIRED)\nadd_compile_options(-Wall -Wextra -Werror)\n"
            f'add_executable(open_recovery_probe probe.c "{(ROOT / "src" / "pager.c").as_posix()}" "{(ROOT / "src" / "pager_checkpoint_order.c").as_posix()}")\n'
            f'target_include_directories(open_recovery_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
            f'set_source_files_properties("{(ROOT / "src" / "pager.c").as_posix()}" PROPERTIES COMPILE_DEFINITIONS "pager_checkpoint=pager_checkpoint_legacy_base")\n'
            "target_link_libraries(open_recovery_probe PRIVATE Threads::Threads)\n"
        )
        build = configure_and_build(tmp_path, cmake)
        run = subprocess.run(
            [str(build / "open_recovery_probe"), str(tmp_path / "recovery.db")],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in [
            "absent_noop=yes",
            "invalid_fail_closed=yes",
            "valid_recovered=yes",
            "mixed_catalog_fail_closed=yes",
        ]:
            if marker not in run.stdout:
                raise AssertionError(f"missing runtime marker {marker}: {run.stdout}")


def main():
    test_source_contract()
    compile_header_probe()
    run_real_open_recovery_probe_on_posix()
    print("PASS: compact-V2 database-open orchestration loads sidecars before strict Pager recovery and fails closed")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
