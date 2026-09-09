import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_manifest_store.h"


def test_manifest_store_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "write_temp",
        "sync_temp",
        "publish_temp",
        "sync_parent",
        "tinydb_compact_v2_migration_manifest_publish_durable(",
        "tinydb_compact_v2_migration_manifest_mark_catalog_published_durable(",
        "*encoded_length_out = encoded_length",
        "TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED",
        "tinydb_compact_v2_migration_manifest_same_identity(",
    ]:
        assert token in text, f"missing durable manifest-store invariant: {token}"

    publish = text.index("if (!ops->write_temp")
    sync_temp = text.index("if (!ops->sync_temp", publish)
    rename = text.index("if (!ops->publish_temp", sync_temp)
    sync_parent = text.index("if (!ops->sync_parent", rename)
    expose = text.index("*encoded_length_out = encoded_length", sync_parent)
    assert publish < sync_temp < rename < sync_parent < expose


def compile_and_run_manifest_store_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for migration manifest-store regression")

    c_source = r'''#include "compact_v2_migration_manifest_store.h"
#include <stdio.h>
#include <string.h>

typedef struct ProbeStore {
    int calls[8];
    int count;
    int fail_step;
    TinyDBCompactV2MigrationPhase observed_phase;
} ProbeStore;

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

static bool record_step(ProbeStore* store, int step) {
    store->calls[store->count++] = step;
    return store->fail_step != step;
}

static bool write_temp(void* context, const unsigned char* data, size_t length) {
    ProbeStore* store = (ProbeStore*)context;
    TinyDBCompactV2MigrationManifest decoded;
    uint32_t claims[8] = {0u};
    if (!record_step(store, 1)) return false;
    if (!tinydb_compact_v2_migration_manifest_decode(
            data, length, &decoded, claims, 8u)) return false;
    store->observed_phase = decoded.phase;
    return true;
}

static bool sync_temp(void* context) {
    return record_step((ProbeStore*)context, 2);
}

static bool publish_temp(void* context) {
    return record_step((ProbeStore*)context, 3);
}

static bool sync_parent(void* context) {
    return record_step((ProbeStore*)context, 4);
}

static TinyDBCompactV2MigrationManifest make_manifest(uint32_t* pages) {
    TinyDBCompactV2MigrationManifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.table_id = 7u;
    manifest.old_root_page_num = 9u;
    manifest.staged_root_page_num = 103u;
    manifest.old_schema_generation = UINT64_C(41);
    manifest.new_schema_generation = UINT64_C(42);
    manifest.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED;
    manifest.claimed_page_count = 4u;
    manifest.claimed_pages = pages;
    return manifest;
}

int main(void) {
    uint32_t pages[4] = {101u, 102u, 103u, 104u};
    TinyDBCompactV2MigrationManifest manifest = make_manifest(pages);
    unsigned char scratch[256];
    size_t encoded_length = 999u;
    ProbeStore store;
    memset(&store, 0, sizeof(store));
    TinyDBCompactV2MigrationManifestStoreOps ops = {
        &store, write_temp, sync_temp, publish_temp, sync_parent
    };

    if (!expect(tinydb_compact_v2_migration_manifest_publish_durable(
                    &manifest, scratch, sizeof(scratch), &ops, &encoded_length),
                "durable manifest publication failed")) return 1;
    if (!expect(store.count == 4 && store.calls[0] == 1 && store.calls[1] == 2 &&
                store.calls[2] == 3 && store.calls[3] == 4,
                "durability operations ran out of order")) return 2;
    if (!expect(encoded_length == 64u, "encoded length was not published last")) return 3;
    if (!expect(store.observed_phase == TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED,
                "write callback observed wrong initial phase")) return 4;

    memset(&store, 0, sizeof(store));
    store.fail_step = 2;
    encoded_length = 777u;
    if (!expect(!tinydb_compact_v2_migration_manifest_publish_durable(
                    &manifest, scratch, sizeof(scratch), &ops, &encoded_length),
                "sync-temp failure was accepted")) return 5;
    if (!expect(store.count == 2 && encoded_length == 0u,
                "sync-temp failure crossed the rename boundary or exposed length")) return 6;

    memset(&store, 0, sizeof(store));
    store.fail_step = 4;
    encoded_length = 777u;
    if (!expect(!tinydb_compact_v2_migration_manifest_publish_durable(
                    &manifest, scratch, sizeof(scratch), &ops, &encoded_length),
                "parent-sync failure was accepted")) return 7;
    if (!expect(store.count == 4 && encoded_length == 0u,
                "parent-sync failure incorrectly reported durable publication")) return 8;

    memset(&store, 0, sizeof(store));
    TinyDBCompactV2MigrationManifest published;
    encoded_length = 0u;
    if (!expect(tinydb_compact_v2_migration_manifest_mark_catalog_published_durable(
                    &manifest, scratch, sizeof(scratch), &ops, &published, &encoded_length),
                "catalog-published phase transition failed")) return 9;
    if (!expect(published.phase == TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED &&
                store.observed_phase == TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED,
                "catalog-published phase was not persisted")) return 10;
    if (!expect(tinydb_compact_v2_migration_manifest_same_identity(&manifest, &published),
                "phase transition changed migration identity")) return 11;

    memset(&store, 0, sizeof(store));
    encoded_length = 123u;
    TinyDBCompactV2MigrationManifest second_output;
    if (!expect(!tinydb_compact_v2_migration_manifest_mark_catalog_published_durable(
                    &published, scratch, sizeof(scratch), &ops, &second_output, &encoded_length),
                "non-monotonic phase transition was accepted")) return 12;
    if (!expect(store.count == 0 && encoded_length == 0u && second_output.table_id == 0u,
                "rejected phase transition published state")) return 13;

    puts("durable_order=yes");
    puts("failure_boundaries=yes");
    puts("phase_transition=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-migration-manifest-store-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(c_source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBMigrationManifestStoreProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\n"
            "set(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n"
            "  add_compile_options(/W4 /WX /utf-8)\n"
            "else()\n"
            "  add_compile_options(-Wall -Wextra -Werror)\n"
            "endif()\n"
            "add_executable(migration_manifest_store_probe probe.c)\n"
            f'target_include_directories(migration_manifest_store_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
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

        exe = build / "migration_manifest_store_probe"
        if sys.platform.startswith("win"):
            exe = build / "Debug" / "migration_manifest_store_probe.exe"
        run = subprocess.run(
            [str(exe)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in ["durable_order=yes", "failure_boundaries=yes", "phase_transition=yes"]:
            if marker not in run.stdout:
                raise AssertionError(f"missing runtime marker {marker}: {run.stdout}")


def main():
    test_manifest_store_source_contract()
    compile_and_run_manifest_store_probe()
    print("PASS: compact V2 migration manifest publication enforces write/sync/rename/parent-sync ordering and a monotonic durable phase transition")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)
