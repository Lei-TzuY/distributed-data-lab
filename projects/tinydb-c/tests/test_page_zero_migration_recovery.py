import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "src" / "compact_v2_migration_manifest.h"
RECLAIM = ROOT / "src" / "fixed_v1_tree_reclaim.h"


def test_source_contract():
    manifest = MANIFEST.read_text(encoding="utf-8")
    assert "manifest->old_root_page_num == 0u" not in manifest
    assert "page_num == 0u || page_num == manifest->old_root_page_num" in manifest

    reclaim = RECLAIM.read_text(encoding="utf-8")
    for token in [
        "tinydb_fixed_v1_tree_collect_ownership_impl(",
        "allow_page_zero_root",
        "tinydb_fixed_v1_tree_retire_page_zero_root(",
        "ownership.pages + 1u",
        "ownership.page_count - 1u",
        "tinydb_fixed_v1_page_zero_is_retired(pager)",
        "root[NODE_TYPE_OFFSET] = (uint8_t)NODE_LEAF",
        "tinydb_fixed_v1_write_u32(root, LEAF_NODE_NUM_CELLS_OFFSET, 0u)",
        "mark_page_dirty(pager, 0u)",
    ]:
        assert token in reclaim, f"missing page-zero retirement invariant: {token}"
    collect = reclaim.index("tinydb_fixed_v1_tree_collect_ownership_impl(", reclaim.index("tinydb_fixed_v1_tree_retire_page_zero_root("))
    descendants = reclaim.index("ownership.pages + 1u", collect)
    tombstone = reclaim.index("root[NODE_TYPE_OFFSET] = (uint8_t)NODE_LEAF", descendants)
    assert collect < descendants < tombstone


def configure_and_build(tmp_path: Path, source: str, executable: bool):
    (tmp_path / "probe.c").write_text(source, encoding="utf-8")
    target = "add_executable(page_zero_probe probe.c)" if executable else "add_library(page_zero_probe OBJECT probe.c)"
    extra = ""
    if executable:
        extra = (
            f' "{(ROOT / "src" / "pager.c").as_posix()}"'
            f' "{(ROOT / "src" / "pager_checkpoint_order.c").as_posix()}"'
        )
        target = f"add_executable(page_zero_probe probe.c{extra})"
    cmake = (
        "cmake_minimum_required(VERSION 3.10)\n"
        "project(TinyDBPageZeroRecoveryProbe C)\n"
        "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
        "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
        + ("find_package(Threads REQUIRED)\n" if executable else "")
        + target + "\n"
        + f'target_include_directories(page_zero_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
        + (f'set_source_files_properties("{(ROOT / "src" / "pager.c").as_posix()}" PROPERTIES COMPILE_DEFINITIONS "pager_checkpoint=pager_checkpoint_legacy_base")\n'
           "target_link_libraries(page_zero_probe PRIVATE Threads::Threads)\n" if executable else "")
    )
    (tmp_path / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    build = tmp_path / "build"
    configured = subprocess.run(["cmake", "-S", str(tmp_path), "-B", str(build)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=60)
    if configured.returncode != 0:
        raise AssertionError(configured.stdout + configured.stderr)
    compiled = subprocess.run(["cmake", "--build", str(build), "--config", "Debug"], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=120)
    if compiled.returncode != 0:
        raise AssertionError(compiled.stdout + compiled.stderr)
    return build


def compile_header_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for page-zero recovery regression")
    source = r'''#include "compact_v2_migration_pager_recovery.h"

bool page_zero_header_probe(Pager* pager) {
    uint32_t claims[1] = {1u};
    TinyDBCompactV2MigrationManifest manifest = {
        7u, 0u, 1u, UINT64_C(10), UINT64_C(11),
        TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED, 1u, claims
    };
    return tinydb_compact_v2_migration_manifest_is_valid(&manifest) &&
           (!pager || tinydb_fixed_v1_tree_reclaim(pager, 0u));
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-page-zero-compile-") as tmp:
        configure_and_build(Path(tmp), source, False)


def run_runtime_probe_on_posix():
    if sys.platform.startswith("win"):
        print("page_zero_runtime=covered_by_posix")
        return
    source = r'''#include "compact_v2_migration_pager_recovery.h"
#include <stdio.h>
#include <string.h>

typedef struct ProbeContext {
    int fail_remove_once;
    int remove_calls;
    int sync_calls;
} ProbeContext;

static void put_u32(void* page, uint32_t offset, uint32_t value) {
    memcpy((unsigned char*)page + offset, &value, sizeof(value));
}

static void init_leaf(void* page, uint32_t parent, uint32_t prev, uint32_t next, uint32_t key) {
    memset(page, 0, PAGE_SIZE);
    ((unsigned char*)page)[NODE_TYPE_OFFSET] = (unsigned char)NODE_LEAF;
    ((unsigned char*)page)[IS_ROOT_OFFSET] = 0u;
    put_u32(page, PARENT_POINTER_OFFSET, parent);
    put_u32(page, LEAF_NODE_NUM_CELLS_OFFSET, 1u);
    put_u32(page, LEAF_NODE_PREV_LEAF_OFFSET, prev);
    put_u32(page, LEAF_NODE_NEXT_LEAF_OFFSET, next);
    put_u32(page, LEAF_NODE_HEADER_SIZE + LEAF_NODE_KEY_OFFSET, key);
}

static void init_page_zero_root(void* page) {
    memset(page, 0, PAGE_SIZE);
    ((unsigned char*)page)[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    ((unsigned char*)page)[IS_ROOT_OFFSET] = 1u;
    put_u32(page, PARENT_POINTER_OFFSET, 0u);
    put_u32(page, INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    put_u32(page, INTERNAL_NODE_RIGHT_CHILD_OFFSET, 2u);
    put_u32(page, INTERNAL_NODE_HEADER_SIZE, 1u);
    put_u32(page, INTERNAL_NODE_HEADER_SIZE + INTERNAL_NODE_CHILD_SIZE, 10u);
}

static bool read_catalog(void* context, uint32_t table_id, uint32_t* root, uint64_t* generation) {
    (void)context;
    if (table_id != 7u) return false;
    *root = 3u;
    *generation = UINT64_C(11);
    return true;
}

static bool remove_manifest(void* context) {
    ProbeContext* ctx = (ProbeContext*)context;
    ctx->remove_calls++;
    if (ctx->fail_remove_once) {
        ctx->fail_remove_once = 0;
        return false;
    }
    return true;
}

static bool sync_parent(void* context) {
    ProbeContext* ctx = (ProbeContext*)context;
    ctx->sync_calls++;
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
    void* root = get_page(pager, 0u);
    init_page_zero_root(root);
    mark_page_dirty(pager, 0u);
    pager_checkpoint(pager);

    pager_begin_transaction(pager);
    for (uint32_t expected = 1u; expected <= 3u; expected++) {
        uint32_t page_num = get_unused_page_num(pager);
        if (!expect(page_num == expected, "unexpected allocation identity")) return 1;
        void* page = get_page(pager, page_num);
        memset(page, 0, PAGE_SIZE);
        mark_page_dirty(pager, page_num);
    }
    init_leaf(get_page(pager, 1u), 0u, 0u, 2u, 10u);
    init_leaf(get_page(pager, 2u), 0u, 1u, 0u, 20u);
    mark_page_dirty(pager, 1u);
    mark_page_dirty(pager, 2u);
    pager_commit(pager);
    pager_checkpoint(pager);

    uint32_t claims[1] = {3u};
    TinyDBCompactV2MigrationManifest manifest = {
        7u, 0u, 3u, UINT64_C(10), UINT64_C(11),
        TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED, 1u, claims
    };
    if (!expect(tinydb_compact_v2_migration_manifest_is_valid(&manifest),
                "page-zero old root manifest rejected")) return 2;

    ProbeContext ctx = {1, 0, 0};
    TinyDBCompactV2MigrationPagerRecoveryAdapter adapter = {
        pager, &ctx, read_catalog, NULL, remove_manifest, sync_parent, false
    };
    TinyDBCompactV2MigrationRecoveryResult result;

    if (!expect(!tinydb_compact_v2_migration_pager_recover_reopen(&manifest, &adapter, &result),
                "manifest cleanup failure was reported as recovery success")) return 3;
    if (!expect(!pager->in_transaction && pager->free_page_count == 2u,
                "page-zero descendants were not durably reclaimed")) return 4;
    if (!expect(!tinydb_fixed_v1_tree_page_is_free(pager, 0u) &&
                tinydb_fixed_v1_tree_page_is_free(pager, 1u) &&
                tinydb_fixed_v1_tree_page_is_free(pager, 2u),
                "page-zero reservation/free-list invariant broken")) return 5;
    if (!expect(tinydb_fixed_v1_page_zero_is_retired(pager),
                "page zero was not replaced with the retirement tombstone")) return 6;

    if (!expect(tinydb_compact_v2_migration_pager_recover_reopen(&manifest, &adapter, &result),
                "checkpointed page-zero retirement was not idempotent")) return 7;
    if (!expect(pager->free_page_count == 2u && ctx.remove_calls == 2 && ctx.sync_calls == 1,
                "retry duplicated descendant frees or cleanup")) return 8;

    if (!expect(pager_try_close(pager), "unable to close pager")) return 9;
    pager = pager_open(argv[1]);
    if (!expect(pager->free_page_count == 2u && tinydb_fixed_v1_page_zero_is_retired(pager),
                "page-zero retirement did not survive reopen")) return 10;
    if (!expect(pager_try_close(pager), "unable to close reopened pager")) return 11;

    puts("page_zero_manifest=yes");
    puts("descendants_reclaimed=yes");
    puts("page_zero_reserved=yes");
    puts("retry_idempotent=yes");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-page-zero-runtime-") as tmp:
        tmp_path = Path(tmp)
        build = configure_and_build(tmp_path, source, True)
        run = subprocess.run([str(build / "page_zero_probe"), str(tmp_path / "page-zero.db")], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30)
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in ["page_zero_manifest=yes", "descendants_reclaimed=yes", "page_zero_reserved=yes", "retry_idempotent=yes"]:
            if marker not in run.stdout:
                raise AssertionError(f"missing runtime marker {marker}: {run.stdout}")


def main():
    test_source_contract()
    compile_header_probe()
    run_runtime_probe_on_posix()
    print("PASS: historical page-zero fixed-V1 roots retire crash-safely without ever entering the Pager free list")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
