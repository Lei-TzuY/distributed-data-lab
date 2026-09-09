import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_pager_recovery.h"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_compact_v2_migration_pager_recover_reopen(",
        "pager_begin_transaction(adapter->pager)",
        "tinydb_compact_v2_migration_pager_reclaim_claims(",
        "pager_commit(adapter->pager)",
        "pager_checkpoint(adapter->pager)",
        "adapter->reclaim_was_checkpointed = true",
        "pager_rollback(adapter->pager)",
        "adapter->remove_manifest(adapter->context)",
        "adapter->sync_parent(adapter->context)",
    ]:
        assert token in text, f"missing pager recovery invariant: {token}"

    commit = text.index("pager_commit(adapter->pager)")
    checkpoint = text.index("pager_checkpoint(adapter->pager)", commit)
    durable = text.index("adapter->reclaim_was_checkpointed = true", checkpoint)
    remove = text.index("adapter->remove_manifest(adapter->context)", durable)
    assert commit < checkpoint < durable < remove


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
        raise AssertionError("cmake is required for pager recovery regression")
    source = r'''#include "compact_v2_migration_pager_recovery.h"

bool pager_recovery_header_probe(const TinyDBCompactV2MigrationManifest* manifest,
                                 TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter,
                                 TinyDBCompactV2MigrationRecoveryResult* result) {
    return tinydb_compact_v2_migration_pager_recover_reopen(manifest, adapter, result);
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-pager-recovery-compile-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBPagerRecoveryHeaderProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_library(pager_recovery_header_probe OBJECT probe.c)\n"
            f'target_include_directories(pager_recovery_header_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        )
        configure_and_build(tmp_path, cmake)


def run_real_pager_probe_on_posix():
    if sys.platform.startswith("win"):
        print("pager_recovery_runtime=covered_by_posix")
        return

    source = r'''#include "compact_v2_migration_pager_recovery.h"
#include <stdio.h>
#include <string.h>

typedef struct ProbeContext {
    uint32_t root;
    uint64_t generation;
    int remove_calls;
    int sync_parent_calls;
    int fail_remove_once;
} ProbeContext;

static bool read_catalog(void* ctx, uint32_t table_id, uint32_t* root, uint64_t* gen) {
    ProbeContext* p = (ProbeContext*)ctx;
    if (table_id != 7u) return false;
    *root = p->root;
    *gen = p->generation;
    return true;
}

static bool reclaim_old_tree(void* ctx, Pager* pager, uint32_t old_root) {
    (void)ctx;
    if (old_root == 0u || !pager->in_transaction) return false;
    pager_free_page(pager, old_root);
    return true;
}

static bool remove_manifest(void* ctx) {
    ProbeContext* p = (ProbeContext*)ctx;
    p->remove_calls++;
    if (p->fail_remove_once) {
        p->fail_remove_once = 0;
        return false;
    }
    return true;
}

static bool sync_parent(void* ctx) {
    ProbeContext* p = (ProbeContext*)ctx;
    p->sync_parent_calls++;
    return true;
}

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) return 100;
    Pager* pager = pager_open(argv[1]);
    void* root_page = get_page(pager, 0u);
    memset(root_page, 0, PAGE_SIZE);
    mark_page_dirty(pager, 0u);
    pager_checkpoint(pager);

    pager_begin_transaction(pager);
    for (uint32_t expected = 1u; expected <= 4u; expected++) {
        uint32_t page_num = get_unused_page_num(pager);
        if (!expect(page_num == expected, "unexpected page allocation")) return 1;
        void* page = get_page(pager, page_num);
        memset(page, (int)(0x40u + expected), PAGE_USABLE_SIZE);
        mark_page_dirty(pager, page_num);
    }
    pager_commit(pager);
    pager_checkpoint(pager);

    uint32_t claims[3] = {1u, 2u, 3u};
    TinyDBCompactV2MigrationManifest manifest = {
        7u, 4u, 1u, UINT64_C(10), UINT64_C(11),
        TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED,
        3u, claims
    };
    ProbeContext ctx = {4u, UINT64_C(10), 0, 0, 1};
    TinyDBCompactV2MigrationPagerRecoveryAdapter adapter = {
        pager, &ctx, read_catalog, reclaim_old_tree, remove_manifest, sync_parent, false
    };
    TinyDBCompactV2MigrationRecoveryResult result;

    if (!expect(!tinydb_compact_v2_migration_pager_recover_reopen(
                    &manifest, &adapter, &result),
                "manifest removal failure was reported as recovery success")) return 2;
    if (!expect(!pager->in_transaction && pager->free_page_count == 3u,
                "durable reclaim was rolled back after manifest failure")) return 3;
    if (!expect(ctx.remove_calls == 1 && ctx.sync_parent_calls == 0,
                "failure ordering crossed manifest removal boundary")) return 4;

    if (!expect(tinydb_compact_v2_migration_pager_recover_reopen(
                    &manifest, &adapter, &result),
                "idempotent recovery retry failed")) return 5;
    if (!expect(result.action == TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING,
                "wrong strict recovery action")) return 6;
    if (!expect(pager->free_page_count == 3u && ctx.remove_calls == 2 &&
                ctx.sync_parent_calls == 1,
                "retry duplicated allocator state or skipped cleanup")) return 7;

    if (!expect(pager_try_close(pager), "unable to close pager")) return 8;
    pager = pager_open(argv[1]);
    if (!expect(pager->free_page_count == 3u,
                "checkpointed recovery allocator state did not survive reopen")) return 9;
    if (!expect(pager_try_close(pager), "unable to close reopened pager")) return 10;

    puts("checkpoint_before_manifest=yes");
    puts("manifest_failure_retry=yes");
    puts("allocator_reopen=yes");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-pager-recovery-runtime-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBPagerRecoveryRuntimeProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "find_package(Threads REQUIRED)\nadd_compile_options(-Wall -Wextra -Werror)\n"
            f'add_executable(pager_recovery_probe probe.c "{(ROOT / "src" / "pager.c").as_posix()}" "{(ROOT / "src" / "pager_checkpoint_order.c").as_posix()}")\n'
            f'target_include_directories(pager_recovery_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
            f'set_source_files_properties("{(ROOT / "src" / "pager.c").as_posix()}" PROPERTIES COMPILE_DEFINITIONS "pager_checkpoint=pager_checkpoint_legacy_base")\n'
            "target_link_libraries(pager_recovery_probe PRIVATE Threads::Threads)\n"
        )
        build = configure_and_build(tmp_path, cmake)
        run = subprocess.run(
            [str(build / "pager_recovery_probe"), str(tmp_path / "recovery.db")],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in ["checkpoint_before_manifest=yes", "manifest_failure_retry=yes", "allocator_reopen=yes"]:
            if marker not in run.stdout:
                raise AssertionError(f"missing runtime marker {marker}: {run.stdout}")


def main():
    test_source_contract()
    compile_header_probe()
    run_real_pager_probe_on_posix()
    print("PASS: Pager-backed compact V2 reopen recovery checkpoints reclaim before manifest cleanup and retries idempotently")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
