import os
import shutil
import subprocess
import sys
import tempfile


PROBE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "schema_repack_migration_reopen.h"

typedef struct ReopenProbe {
    const char* database_filename;
    uint32_t root;
    uint64_t generation;
    uint32_t reads;
    uint32_t reclaim_calls;
    uint32_t sync_reclaim_calls;
    uint32_t remove_calls;
    uint32_t sync_parent_calls;
    uint32_t last_claim_count;
} ReopenProbe;

static bool build_manifest_path(const char* database_filename, char* out, size_t cap) {
    return tinydb_compact_v2_migration_manifest_file_path(database_filename, out, cap);
}

static bool write_manifest_file(const char* database_filename,
                                TinyDBCompactV2MigrationPhase phase,
                                bool corrupt) {
    const uint32_t claims[4] = {41u, 42u, 43u, 44u};
    TinyDBCompactV2MigrationManifest manifest;
    unsigned char encoded[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE];
    char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    size_t encoded_length = 0u;
    FILE* file;

    memset(&manifest, 0, sizeof(manifest));
    manifest.table_id = 7u;
    manifest.old_root_page_num = 9u;
    manifest.staged_root_page_num = 44u;
    manifest.old_schema_generation = 12u;
    manifest.new_schema_generation = 13u;
    manifest.phase = phase;
    manifest.claimed_page_count = 4u;
    manifest.claimed_pages = claims;
    if (!tinydb_compact_v2_migration_manifest_encode(
            &manifest, encoded, sizeof(encoded), &encoded_length)) return false;
    if (corrupt) encoded[0] ^= 0x7fu;
    if (!build_manifest_path(database_filename, path, sizeof(path))) return false;
    file = fopen(path, "wb");
    if (file == NULL) return false;
    if (fwrite(encoded, 1u, encoded_length, file) != encoded_length) {
        fclose(file);
        return false;
    }
    return fclose(file) == 0;
}

static bool manifest_exists(const char* database_filename) {
    char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    FILE* file;
    if (!build_manifest_path(database_filename, path, sizeof(path))) return false;
    file = fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static bool read_catalog(void* context, uint32_t table_id,
                         uint32_t* root_out, uint64_t* generation_out) {
    ReopenProbe* probe = (ReopenProbe*)context;
    if (probe == NULL || table_id != 7u || root_out == NULL || generation_out == NULL) return false;
    probe->reads++;
    *root_out = probe->root;
    *generation_out = probe->generation;
    return true;
}

static bool reclaim_staging(void* context, const uint32_t* pages, uint32_t count) {
    ReopenProbe* probe = (ReopenProbe*)context;
    if (probe == NULL || pages == NULL || count != 4u) return false;
    if (pages[0] != 41u || pages[1] != 42u || pages[2] != 43u || pages[3] != 44u) return false;
    probe->reclaim_calls++;
    probe->last_claim_count = count;
    return true;
}

static bool reclaim_old_tree(void* context, uint32_t old_root_page_num) {
    (void)context;
    (void)old_root_page_num;
    return false;
}

static bool sync_reclaim(void* context) {
    ReopenProbe* probe = (ReopenProbe*)context;
    if (probe == NULL) return false;
    probe->sync_reclaim_calls++;
    return true;
}

static bool remove_manifest(void* context) {
    ReopenProbe* probe = (ReopenProbe*)context;
    char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    if (probe == NULL || !build_manifest_path(probe->database_filename, path, sizeof(path))) return false;
    probe->remove_calls++;
    return remove(path) == 0;
}

static bool sync_parent(void* context) {
    ReopenProbe* probe = (ReopenProbe*)context;
    if (probe == NULL) return false;
    probe->sync_parent_calls++;
    return true;
}

static TinyDBCompactV2MigrationRecoveryOps make_ops(ReopenProbe* probe) {
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

int main(int argc, char** argv) {
    TinyDBSchemaRepackReopenResult result;
    TinyDBCompactV2MigrationRecoveryOps ops;
    ReopenProbe probe;
    char message[160];
    const char* database_filename;

    if (argc != 2) return 90;
    database_filename = argv[1];

    /* No sidecar is a successful no-op and must not inspect catalog state. */
    memset(&probe, 0, sizeof(probe));
    probe.database_filename = database_filename;
    probe.root = 9u;
    probe.generation = 12u;
    ops = make_ops(&probe);
    if (tinydb_schema_repack_recover_file_on_reopen(
            database_filename, &ops, &result, message, sizeof(message)) !=
        TINYDB_SCHEMA_REPACK_REOPEN_NO_MANIFEST) return 1;
    if (!result.ready || result.status != TINYDB_SCHEMA_REPACK_REOPEN_NO_MANIFEST ||
        probe.reads != 0u || probe.reclaim_calls != 0u) return 2;

    /* A real encoded sidecar is loaded, decoded, recovered, and removed. */
    if (!write_manifest_file(database_filename,
                             TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED,
                             false)) return 3;
    if (!manifest_exists(database_filename)) return 4;
    if (tinydb_schema_repack_recover_file_on_reopen(
            database_filename, &ops, &result, message, sizeof(message)) !=
        TINYDB_SCHEMA_REPACK_REOPEN_RECOVERED) return 5;
    if (!result.ready || result.authoritative_root_page_num != 9u ||
        result.authoritative_schema_generation != 12u ||
        result.reclaimed_page_count != 4u || probe.reads != 1u ||
        probe.reclaim_calls != 1u || probe.sync_reclaim_calls != 1u ||
        probe.remove_calls != 1u || probe.sync_parent_calls != 1u ||
        probe.last_claim_count != 4u || manifest_exists(database_filename)) return 6;

    /* Corruption is rejected by the bounded file decoder before callbacks. */
    if (!write_manifest_file(database_filename,
                             TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED,
                             true)) return 7;
    memset(&probe, 0, sizeof(probe));
    probe.database_filename = database_filename;
    probe.root = 9u;
    probe.generation = 12u;
    ops = make_ops(&probe);
    if (tinydb_schema_repack_recover_file_on_reopen(
            database_filename, &ops, &result, message, sizeof(message)) !=
        TINYDB_SCHEMA_REPACK_REOPEN_INVALID_MANIFEST) return 8;
    if (result.ready || probe.reads != 0u || probe.reclaim_calls != 0u ||
        !manifest_exists(database_filename)) return 9;
    {
        char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
        if (!build_manifest_path(database_filename, path, sizeof(path)) || remove(path) != 0) return 10;
    }

    /* Catalog already at the new generation is crash-window B: fail closed and retain intent. */
    if (!write_manifest_file(database_filename,
                             TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED,
                             false)) return 11;
    memset(&probe, 0, sizeof(probe));
    probe.database_filename = database_filename;
    probe.root = 44u;
    probe.generation = 13u;
    ops = make_ops(&probe);
    if (tinydb_schema_repack_recover_file_on_reopen(
            database_filename, &ops, &result, message, sizeof(message)) !=
        TINYDB_SCHEMA_REPACK_REOPEN_RECOVERY_FAILED) return 12;
    if (result.ready || probe.reads != 1u || probe.reclaim_calls != 0u ||
        probe.remove_calls != 0u || !manifest_exists(database_filename)) return 13;
    {
        char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
        if (!build_manifest_path(database_filename, path, sizeof(path)) || remove(path) != 0) return 14;
    }

    puts("SCHEMA_REPACK_FILE_REOPEN_OK");
    return 0;
}
'''


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    src = os.path.join(repo, "src")
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.c")
        database = os.path.join(tmp, "reopen.db")
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
        run = subprocess.run([exe, database], capture_output=True, text=True, timeout=120)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        if run.returncode != 0:
            return run.returncode
        if "SCHEMA_REPACK_FILE_REOPEN_OK" not in run.stdout:
            raise AssertionError("probe did not report file-backed reopen success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
