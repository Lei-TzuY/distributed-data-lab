#ifndef COMPACT_V2_MIGRATION_MANIFEST_STORE_H
#define COMPACT_V2_MIGRATION_MANIFEST_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "compact_v2_migration_manifest.h"

/*
 * Durability-ordering seam for the compact-V2 migration sidecar.
 *
 * The storage adapter deliberately lives outside this header: POSIX and Windows
 * have different fsync/rename primitives.  Callers provide four operations with
 * these semantics:
 *
 *   write_temp  - replace the temporary sidecar contents with the supplied bytes
 *   sync_temp   - make the temporary file contents durable
 *   publish_temp- atomically replace the active sidecar with the temporary file
 *   sync_parent - make the rename/directory-entry update durable
 *
 * The helper never reports success until all four boundaries complete.  This is
 * important because a successful atomic rename without a durable directory entry
 * is not yet a crash-durable manifest publication on filesystems that require an
 * explicit parent-directory sync.
 */

typedef bool (*TinyDBCompactV2MigrationManifestWriteTempFn)(
    void* context,
    const unsigned char* data,
    size_t length);

typedef bool (*TinyDBCompactV2MigrationManifestStoreStepFn)(void* context);

typedef struct TinyDBCompactV2MigrationManifestStoreOps {
    void* context;
    TinyDBCompactV2MigrationManifestWriteTempFn write_temp;
    TinyDBCompactV2MigrationManifestStoreStepFn sync_temp;
    TinyDBCompactV2MigrationManifestStoreStepFn publish_temp;
    TinyDBCompactV2MigrationManifestStoreStepFn sync_parent;
} TinyDBCompactV2MigrationManifestStoreOps;

static inline bool tinydb_compact_v2_migration_manifest_store_ops_are_valid(
    const TinyDBCompactV2MigrationManifestStoreOps* ops) {
    return ops != NULL && ops->write_temp != NULL && ops->sync_temp != NULL &&
           ops->publish_temp != NULL && ops->sync_parent != NULL;
}

/*
 * Publish one manifest using the only safe ordering for a replace-in-place
 * sidecar.  encoded_length_out is deliberately published last; on any failure it
 * remains zero even when an earlier irreversible step (for example rename) has
 * already completed.  Reopen recovery must therefore always trust the active
 * checksummed sidecar rather than an in-memory return code after a crash window.
 */
static inline bool tinydb_compact_v2_migration_manifest_publish_durable(
    const TinyDBCompactV2MigrationManifest* manifest,
    unsigned char* encode_scratch,
    size_t encode_scratch_capacity,
    const TinyDBCompactV2MigrationManifestStoreOps* ops,
    size_t* encoded_length_out) {
    size_t encoded_length = 0u;
    if (encoded_length_out != NULL) *encoded_length_out = 0u;
    if (manifest == NULL || encode_scratch == NULL || encoded_length_out == NULL ||
        !tinydb_compact_v2_migration_manifest_store_ops_are_valid(ops) ||
        !tinydb_compact_v2_migration_manifest_encode(
            manifest, encode_scratch, encode_scratch_capacity, &encoded_length)) {
        return false;
    }

    if (!ops->write_temp(ops->context, encode_scratch, encoded_length)) return false;
    if (!ops->sync_temp(ops->context)) return false;
    if (!ops->publish_temp(ops->context)) return false;
    if (!ops->sync_parent(ops->context)) return false;

    *encoded_length_out = encoded_length;
    return true;
}

static inline bool tinydb_compact_v2_migration_manifest_same_identity(
    const TinyDBCompactV2MigrationManifest* a,
    const TinyDBCompactV2MigrationManifest* b) {
    if (!tinydb_compact_v2_migration_manifest_is_valid(a) ||
        !tinydb_compact_v2_migration_manifest_is_valid(b) ||
        a->table_id != b->table_id ||
        a->old_root_page_num != b->old_root_page_num ||
        a->staged_root_page_num != b->staged_root_page_num ||
        a->old_schema_generation != b->old_schema_generation ||
        a->new_schema_generation != b->new_schema_generation ||
        a->claimed_page_count != b->claimed_page_count) {
        return false;
    }
    for (uint32_t i = 0u; i < a->claimed_page_count; i++) {
        if (a->claimed_pages[i] != b->claimed_pages[i]) return false;
    }
    return true;
}

/*
 * Persist the monotonic phase transition that follows successful catalog/root
 * publication.  No migration identity or page-ownership field may drift while
 * changing the phase.  The caller-owned output is published only after the new
 * sidecar is fully durable.
 */
static inline bool tinydb_compact_v2_migration_manifest_mark_catalog_published_durable(
    const TinyDBCompactV2MigrationManifest* durable_unpublished_manifest,
    unsigned char* encode_scratch,
    size_t encode_scratch_capacity,
    const TinyDBCompactV2MigrationManifestStoreOps* ops,
    TinyDBCompactV2MigrationManifest* published_manifest_out,
    size_t* encoded_length_out) {
    TinyDBCompactV2MigrationManifest candidate;
    if (published_manifest_out != NULL) memset(published_manifest_out, 0, sizeof(*published_manifest_out));
    if (encoded_length_out != NULL) *encoded_length_out = 0u;
    if (durable_unpublished_manifest == NULL || published_manifest_out == NULL ||
        encoded_length_out == NULL ||
        durable_unpublished_manifest->phase !=
            TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED ||
        !tinydb_compact_v2_migration_manifest_is_valid(durable_unpublished_manifest)) {
        return false;
    }

    candidate = *durable_unpublished_manifest;
    candidate.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED;
    if (!tinydb_compact_v2_migration_manifest_same_identity(
            durable_unpublished_manifest, &candidate)) {
        return false;
    }
    if (!tinydb_compact_v2_migration_manifest_publish_durable(
            &candidate,
            encode_scratch,
            encode_scratch_capacity,
            ops,
            encoded_length_out)) {
        return false;
    }

    *published_manifest_out = candidate;
    return true;
}

#endif