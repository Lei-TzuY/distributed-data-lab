import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_recovery.h"


def test_recovery_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_compact_v2_migration_manifest_classify_recovery_strict(",
        "TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED",
        "TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING",
        "TINYDB_COMPACT_V2_MIGRATION_STRICT_KEEP_NEW_RECLAIM_OLD",
        "tinydb_compact_v2_migration_recover_reopen(",
        "reclaim_staging_pages",
        "reclaim_old_tree",
        "sync_reclaim",
        "remove_manifest",
        "sync_parent",
    ]:
        assert token in text, f"missing recovery invariant: {token}"

    read_catalog = text.index("if (!ops->read_catalog")
    classify = text.index("action = tinydb_compact_v2_migration_manifest_classify_recovery_strict", read_catalog)
    sync_reclaim = text.index("if (!ops->sync_reclaim", classify)
    remove_manifest = text.index("if (!ops->remove_manifest", sync_reclaim)
    sync_parent = text.index("if (!ops->sync_parent", remove_manifest)
    expose = text.index("*result_out = candidate", sync_parent)
    assert read_catalog < classify < sync_reclaim < remove_manifest < sync_parent < expose


def compile_and_run_recovery_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for migration recovery regression")

    c_source = r'''#include "compact_v2_migration_recovery.h"
#include <stdio.h>
#include <string.h>

typedef struct Probe {
    uint32_t catalog_root;
    uint64_t catalog_generation;
    int calls[16];
    int count;
    int fail_step;
    uint32_t reclaimed_old_root;
    uint32_t reclaimed_claim_count;
} Probe;

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

static bool step(Probe* p, int n) {
    p->calls[p->count++] = n;
    return p->fail_step != n;
}

static bool read_catalog(void* context, uint32_t table_id, uint32_t* root, uint64_t* generation) {
    Probe* p = (Probe*)context;
    if (table_id != 7u || !step(p, 1)) return false;
    *root = p->catalog_root;
    *generation = p->catalog_generation;
    return true;
}

static bool reclaim_staging(void* context, const uint32_t* pages, uint32_t count) {
    Probe* p = (Probe*)context;
    if (!step(p, 2)) return false;
    if (pages == NULL || count != 4u || pages[2] != 103u) return false;
    p->reclaimed_claim_count = count;
    return true;
}

static bool reclaim_old(void* context, uint32_t old_root) {
    Probe* p = (Probe*)context;
    if (!step(p, 3)) return false;
    p->reclaimed_old_root = old_root;
    return true;
}

static bool sync_reclaim(void* context) { return step((Probe*)context, 4); }
static bool remove_manifest(void* context) { return step((Probe*)context, 5); }
static bool sync_parent(void* context) { return step((Probe*)context, 6); }

static TinyDBCompactV2MigrationManifest make_manifest(
    uint32_t* pages, TinyDBCompactV2MigrationPhase phase) {
    TinyDBCompactV2MigrationManifest m;
    memset(&m, 0, sizeof(m));
    m.table_id = 7u;
    m.old_root_page_num = 9u;
    m.staged_root_page_num = 103u;
    m.old_schema_generation = UINT64_C(41);
    m.new_schema_generation = UINT64_C(42);
    m.phase = phase;
    m.claimed_page_count = 4u;
    m.claimed_pages = pages;
    return m;
}

int main(void) {
    uint32_t pages[4] = {101u, 102u, 103u, 104u};
    TinyDBCompactV2MigrationManifest m = make_manifest(
        pages, TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED);
    Probe p;
    TinyDBCompactV2MigrationRecoveryOps ops;
    TinyDBCompactV2MigrationRecoveryResult result;
    memset(&p, 0, sizeof(p));
    ops.context = &p;
    ops.read_catalog = read_catalog;
    ops.reclaim_staging_pages = reclaim_staging;
    ops.reclaim_old_tree = reclaim_old;
    ops.sync_reclaim = sync_reclaim;
    ops.remove_manifest = remove_manifest;
    ops.sync_parent = sync_parent;

    p.catalog_root = 9u;
    p.catalog_generation = UINT64_C(41);
    memset(&result, 0x7f, sizeof(result));
    if (!expect(tinydb_compact_v2_migration_recover_reopen(&m, &ops, &result),
                "old-catalog recovery failed")) return 1;
    if (!expect(result.action == TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING &&
                p.reclaimed_claim_count == 4u && p.reclaimed_old_root == 0u,
                "old catalog did not reclaim staging only")) return 2;
    if (!expect(p.count == 5 && p.calls[0] == 1 && p.calls[1] == 2 && p.calls[2] == 4 &&
                p.calls[3] == 5 && p.calls[4] == 6,
                "staging recovery ordering was wrong")) return 3;

    memset(&p, 0, sizeof(p));
    p.catalog_root = 103u;
    p.catalog_generation = UINT64_C(42);
    ops.context = &p;
    if (!expect(tinydb_compact_v2_migration_recover_reopen(&m, &ops, &result),
                "post-publication crash recovery failed")) return 4;
    if (!expect(result.action == TINYDB_COMPACT_V2_MIGRATION_STRICT_KEEP_NEW_RECLAIM_OLD &&
                p.reclaimed_old_root == 9u && p.reclaimed_claim_count == 0u,
                "new catalog did not reclaim old tree only")) return 5;
    if (!expect(p.count == 5 && p.calls[0] == 1 && p.calls[1] == 3 && p.calls[2] == 4 &&
                p.calls[3] == 5 && p.calls[4] == 6,
                "old-tree recovery ordering was wrong")) return 6;

    m.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED;
    memset(&p, 0, sizeof(p));
    p.catalog_root = 9u;
    p.catalog_generation = UINT64_C(41);
    ops.context = &p;
    memset(&result, 0x7f, sizeof(result));
    if (!expect(!tinydb_compact_v2_migration_recover_reopen(&m, &ops, &result),
                "contradictory published-phase/old-catalog state was accepted")) return 7;
    if (!expect(p.count == 1 && result.action == TINYDB_COMPACT_V2_MIGRATION_STRICT_INVALID &&
                result.authoritative_root_page_num == 0u,
                "invalid recovery state crossed reclaim or exposed output")) return 8;

    memset(&p, 0, sizeof(p));
    p.catalog_root = 103u;
    p.catalog_generation = UINT64_C(42);
    p.fail_step = 5;
    ops.context = &p;
    memset(&result, 0x7f, sizeof(result));
    if (!expect(!tinydb_compact_v2_migration_recover_reopen(&m, &ops, &result),
                "manifest-removal failure was accepted")) return 9;
    if (!expect(p.count == 4 && p.calls[0] == 1 && p.calls[1] == 3 && p.calls[2] == 4 &&
                p.calls[3] == 5 && result.action == TINYDB_COMPACT_V2_MIGRATION_STRICT_INVALID,
                "manifest-removal failure crossed parent sync or exposed result")) return 10;

    memset(&p, 0, sizeof(p));
    p.catalog_root = 103u;
    p.catalog_generation = UINT64_C(42);
    p.fail_step = 6;
    ops.context = &p;
    memset(&result, 0x7f, sizeof(result));
    if (!expect(!tinydb_compact_v2_migration_recover_reopen(&m, &ops, &result),
                "parent-sync failure was accepted")) return 11;
    if (!expect(p.count == 5 && result.action == TINYDB_COMPACT_V2_MIGRATION_STRICT_INVALID,
                "parent-sync failure exposed result")) return 12;

    puts("strict_phase=yes");
    puts("reclaim_order=yes");
    puts("retry_boundary=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-migration-recovery-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(c_source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBMigrationRecoveryProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\n"
            "set(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n"
            "  add_compile_options(/W4 /WX /utf-8)\n"
            "else()\n"
            "  add_compile_options(-Wall -Wextra -Werror)\n"
            "endif()\n"
            "add_executable(migration_recovery_probe probe.c)\n"
            f'target_include_directories(migration_recovery_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
            encoding="utf-8",
        )
        build = tmp_path / "build"
        configure = subprocess.run(
            ["cmake", "-S", str(tmp_path), "-B", str(build)],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=60,
        )
        if configure.returncode != 0:
            raise AssertionError(configure.stdout + configure.stderr)
        compile_result = subprocess.run(
            ["cmake", "--build", str(build), "--config", "Debug"],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=120,
        )
        if compile_result.returncode != 0:
            raise AssertionError(compile_result.stdout + compile_result.stderr)

        exe = build / "migration_recovery_probe"
        if sys.platform.startswith("win"):
            exe = build / "Debug" / "migration_recovery_probe.exe"
        run = subprocess.run(
            [str(exe)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in ["strict_phase=yes", "reclaim_order=yes", "retry_boundary=yes"]:
            if marker not in run.stdout:
                raise AssertionError(f"missing runtime marker {marker}: {run.stdout}")


def main():
    test_recovery_source_contract()
    compile_and_run_recovery_probe()
    print("PASS: compact V2 reopen recovery rejects contradictory phases and durably orders idempotent reclaim before manifest removal")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
