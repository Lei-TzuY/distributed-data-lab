import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "fixed_v1_tree_reclaim.h"
RECOVERY = ROOT / "src" / "compact_v2_migration_pager_recovery.h"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_fixed_v1_tree_collect_ownership(",
        "pager_pin_page_handle(",
        "pager_page_handle_acquire_read(",
        "seen[child] != 0u",
        "parent == expected_parent[page_num]",
        "separator != subtree_max[child]",
        "leaf_next[prev] != page_num",
        "leaf_prev[next] != page_num",
        "tinydb_compact_v2_migration_pager_reclaim_claims(",
        "tinydb_fixed_v1_tree_page_is_free(pager, old_root_page_num)",
    ]:
        assert token in text, f"missing fixed-V1 reclaim invariant: {token}"

    collect = text.index("tinydb_fixed_v1_tree_collect_ownership(")
    reclaim = text.index("tinydb_compact_v2_migration_pager_reclaim_claims(", collect)
    assert collect < reclaim, "reclaim must happen only after full ownership validation"

    recovery = RECOVERY.read_text(encoding="utf-8")
    assert '#include "fixed_v1_tree_reclaim.h"' in recovery
    assert "if (adapter->reclaim_old_tree != NULL)" in recovery
    assert "tinydb_fixed_v1_tree_reclaim(adapter->pager, old_root_page_num)" in recovery


def configure_and_build(tmp_path: Path, cmake_text: str):
    (tmp_path / "CMakeLists.txt").write_text(cmake_text, encoding="utf-8")
    build = tmp_path / "build"
    configured = subprocess.run(
        ["cmake", "-S", str(tmp_path), "-B", str(build)],
        capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=60,
    )
    if configured.returncode != 0:
        raise AssertionError(configured.stdout + configured.stderr)
    compiled = subprocess.run(
        ["cmake", "--build", str(build), "--config", "Debug"],
        capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=120,
    )
    if compiled.returncode != 0:
        raise AssertionError(compiled.stdout + compiled.stderr)
    return build


def compile_header_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for fixed-V1 reclaim regression")
    source = r'''#include "fixed_v1_tree_reclaim.h"

bool fixed_v1_reclaim_header_probe(Pager* pager, uint32_t root) {
    TinyDBFixedV1TreeOwnership ownership = {0};
    bool ok = tinydb_fixed_v1_tree_collect_ownership(pager, root, &ownership);
    tinydb_fixed_v1_tree_ownership_destroy(&ownership);
    return ok || tinydb_fixed_v1_tree_reclaim(pager, root);
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-fixed-v1-reclaim-compile-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBFixedV1ReclaimHeaderProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_library(fixed_v1_reclaim_header_probe OBJECT probe.c)\n"
            f'target_include_directories(fixed_v1_reclaim_header_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        )
        configure_and_build(tmp_path, cmake)


def run_real_pager_probe_on_posix():
    if sys.platform.startswith("win"):
        print("fixed_v1_reclaim_runtime=covered_by_posix")
        return

    source = r'''#include "compact_v2_migration_pager_recovery.h"
#include <stdio.h>
#include <string.h>

typedef struct ProbeContext {
    uint32_t root;
    uint64_t generation;
    int fail_remove_once;
    int remove_calls;
    int sync_calls;
} ProbeContext;

static void put_u32(void* page, uint32_t offset, uint32_t value) {
    memcpy((unsigned char*)page + offset, &value, sizeof(value));
}

static void init_leaf(void* page, uint32_t parent, uint32_t prev,
                      uint32_t next, uint32_t key) {
    memset(page, 0, PAGE_SIZE);
    ((unsigned char*)page)[NODE_TYPE_OFFSET] = (unsigned char)NODE_LEAF;
    ((unsigned char*)page)[IS_ROOT_OFFSET] = 0u;
    put_u32(page, PARENT_POINTER_OFFSET, parent);
    put_u32(page, LEAF_NODE_NUM_CELLS_OFFSET, 1u);
    put_u32(page, LEAF_NODE_PREV_LEAF_OFFSET, prev);
    put_u32(page, LEAF_NODE_NEXT_LEAF_OFFSET, next);
    put_u32(page, LEAF_NODE_HEADER_SIZE + LEAF_NODE_KEY_OFFSET, key);
}

static void init_internal_root(void* page) {
    memset(page, 0, PAGE_SIZE);
    ((unsigned char*)page)[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    ((unsigned char*)page)[IS_ROOT_OFFSET] = 1u;
    put_u32(page, PARENT_POINTER_OFFSET, 0u);
    put_u32(page, INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    put_u32(page, INTERNAL_NODE_RIGHT_CHILD_OFFSET, 3u);
    put_u32(page, INTERNAL_NODE_HEADER_SIZE, 2u);
    put_u32(page, INTERNAL_NODE_HEADER_SIZE + INTERNAL_NODE_CHILD_SIZE, 10u);
}

static bool read_catalog(void* ctx, uint32_t table_id, uint32_t* root, uint64_t* gen) {
    ProbeContext* p = (ProbeContext*)ctx;
    if (table_id != 7u) return false;
    *root = p->root;
    *gen = p->generation;
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
    p->sync_calls++;
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
    void* page0 = get_page(pager, 0u);
    memset(page0, 0, PAGE_SIZE);
    mark_page_dirty(pager, 0u);
    pager_checkpoint(pager);

    pager_begin_transaction(pager);
    for (uint32_t expected = 1u; expected <= 4u; expected++) {
        uint32_t page_num = get_unused_page_num(pager);
        if (!expect(page_num == expected, "unexpected allocation identity")) return 1;
        void* page = get_page(pager, page_num);
        memset(page, 0, PAGE_SIZE);
        mark_page_dirty(pager, page_num);
    }
    init_internal_root(get_page(pager, 1u));
    init_leaf(get_page(pager, 2u), 1u, 0u, 3u, 10u);
    init_leaf(get_page(pager, 3u), 1u, 2u, 0u, 20u);
    mark_page_dirty(pager, 1u);
    mark_page_dirty(pager, 2u);
    mark_page_dirty(pager, 3u);
    pager_commit(pager);
    pager_checkpoint(pager);

    /* Corruption must be detected before the first allocator mutation. */
    pager_begin_transaction(pager);
    void* leaf3 = get_page(pager, 3u);
    put_u32(leaf3, PARENT_POINTER_OFFSET, 99u);
    mark_page_dirty(pager, 3u);
    if (!expect(!tinydb_fixed_v1_tree_reclaim(pager, 1u),
                "bad parent pointer was accepted")) return 2;
    if (!expect(pager->free_page_count == 0u,
                "failed preflight partially reclaimed the tree")) return 3;
    pager_rollback(pager);

    uint32_t claims[1] = {4u};
    TinyDBCompactV2MigrationManifest manifest = {
        7u, 1u, 4u, UINT64_C(10), UINT64_C(11),
        TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED,
        1u, claims
    };
    ProbeContext ctx = {4u, UINT64_C(11), 1, 0, 0};
    TinyDBCompactV2MigrationPagerRecoveryAdapter adapter = {
        pager, &ctx, read_catalog, NULL, remove_manifest, sync_parent, false
    };
    TinyDBCompactV2MigrationRecoveryResult result;

    if (!expect(!tinydb_compact_v2_migration_pager_recover_reopen(
                    &manifest, &adapter, &result),
                "manifest failure was reported as success")) return 4;
    if (!expect(!pager->in_transaction && pager->free_page_count == 3u,
                "old tree was not durably reclaimed before manifest cleanup")) return 5;
    if (!expect(tinydb_fixed_v1_tree_page_is_free(pager, 1u) &&
                tinydb_fixed_v1_tree_page_is_free(pager, 2u) &&
                tinydb_fixed_v1_tree_page_is_free(pager, 3u),
                "ownership set was not fully reclaimed")) return 6;

    if (!expect(tinydb_compact_v2_migration_pager_recover_reopen(
                    &manifest, &adapter, &result),
                "checkpointed old-tree reclaim was not idempotent")) return 7;
    if (!expect(result.action == TINYDB_COMPACT_V2_MIGRATION_STRICT_KEEP_NEW_RECLAIM_OLD,
                "wrong recovery action")) return 8;
    if (!expect(pager->free_page_count == 3u && ctx.remove_calls == 2 &&
                ctx.sync_calls == 1,
                "retry duplicated frees or cleanup")) return 9;

    if (!expect(pager_try_close(pager), "unable to close pager")) return 10;
    pager = pager_open(argv[1]);
    if (!expect(pager->free_page_count == 3u,
                "old-tree reclaim did not survive reopen")) return 11;
    if (!expect(pager_try_close(pager), "unable to close reopened pager")) return 12;

    puts("corrupt_tree_fail_closed=yes");
    puts("whole_tree_reclaimed=yes");
    puts("checkpoint_retry_idempotent=yes");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-fixed-v1-reclaim-runtime-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        cmake = (
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBFixedV1ReclaimRuntimeProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "find_package(Threads REQUIRED)\nadd_compile_options(-Wall -Wextra -Werror)\n"
            f'add_executable(fixed_v1_reclaim_probe probe.c "{(ROOT / "src" / "pager.c").as_posix()}" "{(ROOT / "src" / "pager_checkpoint_order.c").as_posix()}")\n'
            f'target_include_directories(fixed_v1_reclaim_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
            f'set_source_files_properties("{(ROOT / "src" / "pager.c").as_posix()}" PROPERTIES COMPILE_DEFINITIONS "pager_checkpoint=pager_checkpoint_legacy_base")\n'
            "target_link_libraries(fixed_v1_reclaim_probe PRIVATE Threads::Threads)\n"
        )
        build = configure_and_build(tmp_path, cmake)
        run = subprocess.run(
            [str(build / "fixed_v1_reclaim_probe"), str(tmp_path / "reclaim.db")],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in [
            "corrupt_tree_fail_closed=yes",
            "whole_tree_reclaimed=yes",
            "checkpoint_retry_idempotent=yes",
        ]:
            if marker not in run.stdout:
                raise AssertionError(f"missing runtime marker {marker}: {run.stdout}")


def main():
    test_source_contract()
    compile_header_probe()
    run_real_pager_probe_on_posix()
    print("PASS: fixed-V1 ownership walk validates the whole old tree before transactional reclaim and recovery retry")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
