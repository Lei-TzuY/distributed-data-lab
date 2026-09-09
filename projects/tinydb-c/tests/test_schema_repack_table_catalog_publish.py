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
#include "schema_repack_table_catalog_publish.h"

typedef struct ProbeState {
    uint32_t catalog_calls;
    uint32_t write_temp_calls;
    uint32_t sync_temp_calls;
    uint32_t publish_temp_calls;
    uint32_t sync_parent_calls;
    bool fail_catalog;
    bool fail_sync_parent;
    TinyDBCompactV2MigrationPhase observed_phase;
} ProbeState;

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

static bool publish_catalog_durable(void* context,
                                    uint32_t table_id,
                                    uint32_t expected_old_root_page_num,
                                    uint64_t expected_old_schema_generation,
                                    uint32_t new_root_page_num,
                                    uint64_t new_schema_generation) {
    ProbeState* state = (ProbeState*)context;
    state->catalog_calls++;
    if (table_id != 7u || expected_old_root_page_num != 77u ||
        expected_old_schema_generation != UINT64_C(12) ||
        new_root_page_num != 103u || new_schema_generation != UINT64_C(13)) {
        return false;
    }
    return !state->fail_catalog;
}

static bool write_temp(void* context, const unsigned char* data, size_t length) {
    ProbeState* state = (ProbeState*)context;
    TinyDBCompactV2MigrationManifest decoded;
    uint32_t claims[8] = {0u};
    state->write_temp_calls++;
    if (!tinydb_compact_v2_migration_manifest_decode(
            data, length, &decoded, claims, 8u)) return false;
    state->observed_phase = decoded.phase;
    return true;
}
static bool sync_temp(void* context) {
    ((ProbeState*)context)->sync_temp_calls++;
    return true;
}
static bool publish_temp(void* context) {
    ((ProbeState*)context)->publish_temp_calls++;
    return true;
}
static bool sync_parent(void* context) {
    ProbeState* state = (ProbeState*)context;
    state->sync_parent_calls++;
    return !state->fail_sync_parent;
}

static void make_prepared(TinyDBSchemaRepackTableMigrationPrepareResult* prepared,
                          uint32_t* claims) {
    memset(prepared, 0, sizeof(*prepared));
    prepared->durable_stage.ready = true;
    prepared->durable_stage.root_page_num = 103u;
    prepared->durable_stage.claimed_page_count = 4u;
    prepared->intent.manifest.table_id = 7u;
    prepared->intent.manifest.old_root_page_num = 77u;
    prepared->intent.manifest.staged_root_page_num = 103u;
    prepared->intent.manifest.old_schema_generation = UINT64_C(12);
    prepared->intent.manifest.new_schema_generation = UINT64_C(13);
    prepared->intent.manifest.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED;
    prepared->intent.manifest.claimed_page_count = 4u;
    prepared->intent.manifest.claimed_pages = claims;
    prepared->intent.ready = true;
    prepared->destination_durable = true;
    prepared->recovery_intent_durable = true;
    prepared->ready = true;
}

static void reset_state(ProbeState* state) {
    memset(state, 0, sizeof(*state));
}

int main(void) {
    uint32_t claims[4] = {101u, 102u, 103u, 104u};
    unsigned char scratch[256];
    char message[192];
    ProbeState state;
    TinyDBSchemaRepackTableMigrationPrepareResult prepared;
    TinyDBSchemaRepackTableCatalogPublishResult result;
    TinyDBSchemaRepackCatalogPublishOps catalog_ops = {&state, publish_catalog_durable};
    TinyDBCompactV2MigrationManifestStoreOps store_ops = {
        &state, write_temp, sync_temp, publish_temp, sync_parent
    };

    make_prepared(&prepared, claims);
    reset_state(&state);
    if (!expect(tinydb_schema_repack_table_publish_catalog(
                    &prepared, &catalog_ops, scratch, sizeof(scratch), &store_ops,
                    &result, message, sizeof(message)),
                "catalog publication success path failed")) return 1;
    if (!expect(result.ready && result.catalog_published_durable &&
                result.phase_published_durable && result.encoded_length > 0u,
                "success result was not fully published")) return 2;
    if (!expect(result.manifest.phase == TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED &&
                state.observed_phase == TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED,
                "CATALOG_PUBLISHED phase was not persisted")) return 3;
    if (!expect(state.catalog_calls == 1u && state.write_temp_calls == 1u &&
                state.sync_temp_calls == 1u && state.publish_temp_calls == 1u &&
                state.sync_parent_calls == 1u,
                "publication operations did not run exactly once")) return 4;
    if (!expect(tinydb_compact_v2_migration_manifest_same_identity(
                    &prepared.intent.manifest, &result.manifest),
                "phase advance changed migration identity")) return 5;

    /* A failed compare-and-publish catalog switch must not touch the sidecar. */
    make_prepared(&prepared, claims);
    reset_state(&state);
    state.fail_catalog = true;
    memset(&result, 0xA5, sizeof(result));
    if (!expect(!tinydb_schema_repack_table_publish_catalog(
                    &prepared, &catalog_ops, scratch, sizeof(scratch), &store_ops,
                    &result, message, sizeof(message)),
                "failed catalog switch was accepted")) return 6;
    if (!expect(!result.ready && !result.catalog_published_durable &&
                !result.phase_published_durable && result.encoded_length == 0u &&
                state.catalog_calls == 1u && state.write_temp_calls == 0u &&
                state.sync_temp_calls == 0u && state.publish_temp_calls == 0u &&
                state.sync_parent_calls == 0u,
                "failed catalog switch crossed the sidecar boundary")) return 7;

    /*
     * Crash window B0: catalog is durable, sidecar phase rewrite reaches rename
     * but parent sync fails.  The helper must expose the durable catalog fact
     * without claiming phase durability.  Reopen with the still-old phase plus
     * new catalog state must keep the new tree and reclaim the old one.
     */
    make_prepared(&prepared, claims);
    reset_state(&state);
    state.fail_sync_parent = true;
    memset(&result, 0xA5, sizeof(result));
    if (!expect(!tinydb_schema_repack_table_publish_catalog(
                    &prepared, &catalog_ops, scratch, sizeof(scratch), &store_ops,
                    &result, message, sizeof(message)),
                "phase parent-sync failure was accepted")) return 8;
    if (!expect(!result.ready && result.catalog_published_durable &&
                !result.phase_published_durable && result.encoded_length == 0u &&
                result.manifest.phase == TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED,
                "partial publication result lost the durable catalog boundary")) return 9;
    if (!expect(state.catalog_calls == 1u && state.write_temp_calls == 1u &&
                state.sync_temp_calls == 1u && state.publish_temp_calls == 1u &&
                state.sync_parent_calls == 1u,
                "phase failure did not reach the expected durability boundary")) return 10;
    if (!expect(tinydb_compact_v2_migration_manifest_classify_recovery_strict(
                    &result.manifest, 103u, UINT64_C(13)) ==
                TINYDB_COMPACT_V2_MIGRATION_STRICT_KEEP_NEW_RECLAIM_OLD,
                "new catalog plus old manifest phase was not recoverable")) return 11;

    /* Inconsistent prepare identity is rejected before catalog mutation. */
    make_prepared(&prepared, claims);
    prepared.durable_stage.root_page_num = 999u;
    reset_state(&state);
    memset(&result, 0xA5, sizeof(result));
    if (!expect(!tinydb_schema_repack_table_publish_catalog(
                    &prepared, &catalog_ops, scratch, sizeof(scratch), &store_ops,
                    &result, message, sizeof(message)),
                "inconsistent prepared identity was accepted")) return 12;
    if (!expect(state.catalog_calls == 0u && state.write_temp_calls == 0u &&
                !result.ready && !result.catalog_published_durable,
                "inconsistent identity caused publication side effects")) return 13;

    puts("SCHEMA_REPACK_TABLE_CATALOG_PUBLISH_OK");
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
            sys.stdout.write(build.stdout); sys.stderr.write(build.stderr); return build.returncode
        run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        sys.stdout.write(run.stdout); sys.stderr.write(run.stderr)
        if run.returncode != 0: return run.returncode
        if "SCHEMA_REPACK_TABLE_CATALOG_PUBLISH_OK" not in run.stdout:
            raise AssertionError("probe did not report success")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
