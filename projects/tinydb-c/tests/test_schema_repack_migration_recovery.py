import os
import shutil
import subprocess
import sys
import tempfile


PROBE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "schema_repack_migration_recovery.h"

typedef struct RecoveryProbe {
    uint32_t root;
    uint64_t generation;
    uint32_t reads;
    uint32_t reclaim_calls;
    uint32_t sync_reclaim_calls;
    uint32_t remove_calls;
    uint32_t sync_parent_calls;
    uint32_t last_claim_count;
    uint32_t last_claims[8];
    bool fail_sync_reclaim;
    bool fail_remove;
    bool fail_sync_parent;
} RecoveryProbe;

static bool read_catalog(void* context, uint32_t table_id,
                         uint32_t* root_out, uint64_t* generation_out) {
    RecoveryProbe* probe = (RecoveryProbe*)context;
    if (probe == NULL || table_id != 7u || root_out == NULL || generation_out == NULL) return false;
    probe->reads++;
    *root_out = probe->root;
    *generation_out = probe->generation;
    return true;
}

static bool reclaim_staging(void* context, const uint32_t* pages, uint32_t count) {
    RecoveryProbe* probe = (RecoveryProbe*)context;
    uint32_t i;
    if (probe == NULL || pages == NULL || count > 8u) return false;
    probe->reclaim_calls++;
    probe->last_claim_count = count;
    for (i = 0u; i < count; ++i) probe->last_claims[i] = pages[i];
    return true;
}

static bool reclaim_old_tree(void* context, uint32_t old_root_page_num) {
    (void)context;
    (void)old_root_page_num;
    return false; /* crash-window A must never call this path */
}

static bool sync_reclaim(void* context) {
    RecoveryProbe* probe = (RecoveryProbe*)context;
    if (probe == NULL) return false;
    probe->sync_reclaim_calls++;
    return !probe->fail_sync_reclaim;
}

static bool remove_manifest(void* context) {
    RecoveryProbe* probe = (RecoveryProbe*)context;
    if (probe == NULL) return false;
    probe->remove_calls++;
    return !probe->fail_remove;
}

static bool sync_parent(void* context) {
    RecoveryProbe* probe = (RecoveryProbe*)context;
    if (probe == NULL) return false;
    probe->sync_parent_calls++;
    return !probe->fail_sync_parent;
}

static TinyDBCompactV2MigrationRecoveryOps make_ops(RecoveryProbe* probe) {
    TinyDBCompactV2MigrationRecoveryOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = probe;
    ops.read_catalog = read_catalog;
    ops.reclaim_staging_pages = reclaim_staging;
    ops.reclaim_old_tree = reclaim_old_tree;
    ops.sync_reclaim = sync_reclaim;
    ops.remove_manifest = remove_manifest;
    ops.sync_parent = sync_parent;
    return ops;
}

static TinyDBCompactV2MigrationManifest make_manifest(const uint32_t* claims) {
    TinyDBCompactV2MigrationManifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.table_id = 7u;
    manifest.old_root_page_num = 9u;
    manifest.staged_root_page_num = 44u;
    manifest.old_schema_generation = 12u;
    manifest.new_schema_generation = 13u;
    manifest.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED;
    manifest.claimed_page_count = 4u;
    manifest.claimed_pages = claims;
    return manifest;
}

int main(void) {
    const uint32_t claims[4] = {41u, 42u, 43u, 44u};
    TinyDBCompactV2MigrationManifest manifest = make_manifest(claims);
    TinyDBSchemaRepackPreCatalogRecoveryResult result;
    TinyDBCompactV2MigrationRecoveryOps ops;
    RecoveryProbe probe;

    /* Happy crash-window A recovery: old catalog remains authoritative. */
    memset(&probe, 0, sizeof(probe));
    probe.root = 9u;
    probe.generation = 12u;
    ops = make_ops(&probe);
    if (!tinydb_schema_repack_recover_durable_unpublished(&manifest, &ops, &result)) return 1;
    if (!result.ready || result.authoritative_root_page_num != 9u ||
        result.authoritative_schema_generation != 12u ||
        result.reclaimed_page_count != 4u) return 2;
    if (probe.reads != 1u || probe.reclaim_calls != 1u ||
        probe.sync_reclaim_calls != 1u || probe.remove_calls != 1u ||
        probe.sync_parent_calls != 1u || probe.last_claim_count != 4u ||
        memcmp(probe.last_claims, claims, sizeof(claims)) != 0) return 3;

    /* New catalog state belongs to crash-window B and must have zero side effects. */
    memset(&probe, 0, sizeof(probe));
    probe.root = 44u;
    probe.generation = 13u;
    ops = make_ops(&probe);
    if (tinydb_schema_repack_recover_durable_unpublished(&manifest, &ops, &result)) return 4;
    if (result.ready || result.reclaimed_page_count != 0u || probe.reads != 1u ||
        probe.reclaim_calls != 0u || probe.sync_reclaim_calls != 0u ||
        probe.remove_calls != 0u || probe.sync_parent_calls != 0u) return 5;

    /* A durable CATALOG_PUBLISHED phase cannot enter the pre-catalog helper. */
    manifest.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED;
    memset(&probe, 0, sizeof(probe));
    probe.root = 9u;
    probe.generation = 12u;
    ops = make_ops(&probe);
    if (tinydb_schema_repack_recover_durable_unpublished(&manifest, &ops, &result)) return 6;
    if (probe.reads != 0u || probe.reclaim_calls != 0u) return 7;
    manifest.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED;

    /* Failure after reclaim remains unpublished and therefore safely retryable. */
    memset(&probe, 0, sizeof(probe));
    probe.root = 9u;
    probe.generation = 12u;
    probe.fail_remove = true;
    ops = make_ops(&probe);
    if (tinydb_schema_repack_recover_durable_unpublished(&manifest, &ops, &result)) return 8;
    if (result.ready || result.authoritative_root_page_num != 0u ||
        result.authoritative_schema_generation != 0u ||
        result.reclaimed_page_count != 0u) return 9;
    if (probe.reclaim_calls != 1u || probe.sync_reclaim_calls != 1u ||
        probe.remove_calls != 1u || probe.sync_parent_calls != 0u) return 10;

    /* Retry repeats the idempotent reclaim and completes cleanup. */
    probe.fail_remove = false;
    if (!tinydb_schema_repack_recover_durable_unpublished(&manifest, &ops, &result)) return 11;
    if (!result.ready || probe.reads != 2u || probe.reclaim_calls != 2u ||
        probe.sync_reclaim_calls != 2u || probe.remove_calls != 2u ||
        probe.sync_parent_calls != 1u) return 12;

    /* Parent-sync failure is also not success even though manifest removal ran. */
    memset(&probe, 0, sizeof(probe));
    probe.root = 9u;
    probe.generation = 12u;
    probe.fail_sync_parent = true;
    ops = make_ops(&probe);
    if (tinydb_schema_repack_recover_durable_unpublished(&manifest, &ops, &result)) return 13;
    if (result.ready || probe.reclaim_calls != 1u || probe.remove_calls != 1u ||
        probe.sync_parent_calls != 1u) return 14;

    puts("SCHEMA_REPACK_PRECATALOG_RECOVERY_OK");
    return 0;
}
'''


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    src = os.path.join(repo, "src")
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.c")
        with open(probe, "w", newline="\n") as handle:
            handle.write(PROBE)
        if os.name == "nt":
            compiler = shutil.which("cl")
            if compiler is None:
                print("SKIP: MSVC compiler not available")
                return 0
            exe = os.path.join(tmp, "probe.exe")
            command = [compiler, "/nologo", "/std:c11", "/W4", "/WX",
                       f"/I{src}", probe, f"/Fe:{exe}"]
        else:
            compiler = shutil.which("gcc") or shutil.which("cc")
            if compiler is None:
                print("SKIP: C compiler not available")
                return 0
            exe = os.path.join(tmp, "probe")
            command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       f"-I{src}", probe, "-o", exe]
        build = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if build.returncode != 0:
            sys.stdout.write(build.stdout)
            sys.stderr.write(build.stderr)
            return build.returncode
        run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        if run.returncode != 0:
            return run.returncode
        if "SCHEMA_REPACK_PRECATALOG_RECOVERY_OK" not in run.stdout:
            raise AssertionError("probe did not report pre-catalog recovery success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
