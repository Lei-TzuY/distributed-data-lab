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
#include "compact_v2_migration_manifest_file.h"
#include "schema_repack_migration_manifest.h"

typedef struct StoreProbe {
    unsigned char bytes[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE];
    size_t length;
    uint32_t step;
    bool fail_sync_parent;
} StoreProbe;

static bool write_temp(void* context, const unsigned char* data, size_t length) {
    StoreProbe* probe = (StoreProbe*)context;
    if (probe == NULL || data == NULL || length > sizeof(probe->bytes) || probe->step != 0u) return false;
    memcpy(probe->bytes, data, length);
    probe->length = length;
    probe->step = 1u;
    return true;
}
static bool sync_temp(void* context) {
    StoreProbe* probe = (StoreProbe*)context;
    if (probe == NULL || probe->step != 1u) return false;
    probe->step = 2u;
    return true;
}
static bool publish_temp(void* context) {
    StoreProbe* probe = (StoreProbe*)context;
    if (probe == NULL || probe->step != 2u) return false;
    probe->step = 3u;
    return true;
}
static bool sync_parent(void* context) {
    StoreProbe* probe = (StoreProbe*)context;
    if (probe == NULL || probe->step != 3u) return false;
    probe->step = 4u;
    return !probe->fail_sync_parent;
}

static TinyDBCompactV2MigrationManifestStoreOps make_ops(StoreProbe* probe) {
    TinyDBCompactV2MigrationManifestStoreOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = probe;
    ops.write_temp = write_temp;
    ops.sync_temp = sync_temp;
    ops.publish_temp = publish_temp;
    ops.sync_parent = sync_parent;
    return ops;
}

int main(void) {
    TinyDBSchemaRepackDurableStageResult durable;
    TinyDBSchemaRepackMigrationIntentResult result;
    TinyDBCompactV2MigrationManifest decoded;
    TinyDBCompactV2MigrationManifestStoreOps ops;
    StoreProbe probe;
    unsigned char scratch[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE];
    uint32_t decoded_claims[8];
    const uint32_t claims[4] = {41u, 42u, 43u, 44u};

    memset(&durable, 0, sizeof(durable));
    durable.root_page_num = 44u;
    durable.leaf_page_count = 3u;
    durable.internal_page_count = 1u;
    durable.claimed_page_count = 4u;
    durable.row_count = 120u;
    durable.ready = true;

    memset(&probe, 0, sizeof(probe));
    ops = make_ops(&probe);
    if (!tinydb_schema_repack_publish_durable_migration_intent(
            &durable, 7u, 9u, 12u, 13u, claims, 4u,
            scratch, sizeof(scratch), &ops, &result)) return 1;
    if (!result.ready || result.encoded_length == 0u || probe.step != 4u ||
        result.manifest.phase != TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED ||
        result.manifest.table_id != 7u || result.manifest.old_root_page_num != 9u ||
        result.manifest.staged_root_page_num != 44u ||
        result.manifest.old_schema_generation != 12u ||
        result.manifest.new_schema_generation != 13u ||
        result.manifest.claimed_page_count != 4u) return 2;

    memset(&decoded, 0, sizeof(decoded));
    memset(decoded_claims, 0, sizeof(decoded_claims));
    if (!tinydb_compact_v2_migration_manifest_decode(
            probe.bytes, probe.length, &decoded, decoded_claims, 8u)) return 3;
    if (tinydb_compact_v2_migration_manifest_classify_recovery(&decoded, 9u, 12u) !=
        TINYDB_COMPACT_V2_MIGRATION_RECOVERY_RECLAIM_STAGING) return 4;

    durable.ready = false;
    memset(&probe, 0, sizeof(probe));
    ops = make_ops(&probe);
    if (tinydb_schema_repack_publish_durable_migration_intent(
            &durable, 7u, 9u, 12u, 13u, claims, 4u,
            scratch, sizeof(scratch), &ops, &result)) return 5;
    if (result.ready || result.encoded_length != 0u || probe.step != 0u) return 6;
    durable.ready = true;

    memset(&probe, 0, sizeof(probe));
    ops = make_ops(&probe);
    if (tinydb_schema_repack_publish_durable_migration_intent(
            &durable, 7u, 9u, 12u, 13u, claims, 3u,
            scratch, sizeof(scratch), &ops, &result)) return 7;
    if (result.ready || probe.step != 0u) return 8;

    {
        const uint32_t bad_claims[4] = {9u, 42u, 43u, 44u};
        memset(&probe, 0, sizeof(probe));
        ops = make_ops(&probe);
        if (tinydb_schema_repack_publish_durable_migration_intent(
                &durable, 7u, 9u, 12u, 13u, bad_claims, 4u,
                scratch, sizeof(scratch), &ops, &result)) return 9;
        if (result.ready || probe.step != 0u) return 10;
    }

    memset(&probe, 0, sizeof(probe));
    probe.fail_sync_parent = true;
    ops = make_ops(&probe);
    if (tinydb_schema_repack_publish_durable_migration_intent(
            &durable, 7u, 9u, 12u, 13u, claims, 4u,
            scratch, sizeof(scratch), &ops, &result)) return 11;
    if (result.ready || result.encoded_length != 0u || probe.step != 4u) return 12;

    puts("SCHEMA_REPACK_MIGRATION_INTENT_OK");
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
            command = [compiler, "-std=c11", "-D_XOPEN_SOURCE=700",
                       "-Wall", "-Wextra", "-Werror", "-pthread",
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
        if "SCHEMA_REPACK_MIGRATION_INTENT_OK" not in run.stdout:
            raise AssertionError("probe did not report migration intent success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
