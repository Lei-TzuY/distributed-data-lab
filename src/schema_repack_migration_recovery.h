#ifndef SCHEMA_REPACK_MIGRATION_RECOVERY_H
#define SCHEMA_REPACK_MIGRATION_RECOVERY_H

#include "compact_v2_migration_recovery.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Schema-repack reopen boundary for crash window A:
 *
 *   destination tree durable
 *   -> DURABLE_UNPUBLISHED manifest durable
 *   -> crash before catalog publication
 *
 * This wrapper deliberately accepts only the old authoritative catalog state.
 * The generic compact-V2 recovery executor also understands the later
 * catalog-published crash window, but callers recovering the pre-publication
 * schema-repack phase should not silently cross that state boundary.
 *
 * Reclaim callbacks remain idempotent.  A failure after reclaim but before
 * manifest removal/sync is reported as failure with a zeroed result so reopen
 * can retry from the still-durable manifest.
 */
typedef struct TinyDBSchemaRepackPreCatalogRecoveryResult {
    uint32_t authoritative_root_page_num;
    uint64_t authoritative_schema_generation;
    uint32_t reclaimed_page_count;
    bool ready;
} TinyDBSchemaRepackPreCatalogRecoveryResult;

static inline bool tinydb_schema_repack_recover_durable_unpublished(
    const TinyDBCompactV2MigrationManifest* manifest,
    const TinyDBCompactV2MigrationRecoveryOps* ops,
    TinyDBSchemaRepackPreCatalogRecoveryResult* result_out) {
    uint32_t authoritative_root = 0u;
    uint64_t authoritative_generation = UINT64_C(0);

    if (result_out != NULL) memset(result_out, 0, sizeof(*result_out));

    if (manifest == NULL || result_out == NULL ||
        !tinydb_compact_v2_migration_manifest_is_valid(manifest) ||
        manifest->phase !=
            TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED ||
        !tinydb_compact_v2_migration_recovery_ops_are_valid(ops)) {
        return false;
    }

    /*
     * Observe catalog state exactly once before any reclaim side effect.  This
     * intentionally rejects the new-root/new-generation state; that belongs to
     * the later post-publication recovery path.
     */
    if (!ops->read_catalog(
            ops->context,
            manifest->table_id,
            &authoritative_root,
            &authoritative_generation) ||
        authoritative_root != manifest->old_root_page_num ||
        authoritative_generation != manifest->old_schema_generation) {
        return false;
    }

    if (!ops->reclaim_staging_pages(
            ops->context,
            manifest->claimed_pages,
            manifest->claimed_page_count)) {
        return false;
    }
    if (!ops->sync_reclaim(ops->context)) return false;
    if (!ops->remove_manifest(ops->context)) return false;
    if (!ops->sync_parent(ops->context)) return false;

    result_out->authoritative_root_page_num = authoritative_root;
    result_out->authoritative_schema_generation = authoritative_generation;
    result_out->reclaimed_page_count = manifest->claimed_page_count;
    result_out->ready = true;
    return true;
}

#endif /* SCHEMA_REPACK_MIGRATION_RECOVERY_H */
