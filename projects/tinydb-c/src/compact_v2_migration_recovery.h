#ifndef COMPACT_V2_MIGRATION_RECOVERY_H
#define COMPACT_V2_MIGRATION_RECOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "compact_v2_migration_manifest.h"

/*
 * Reopen recovery executor for a durable compact-V2 migration manifest.
 *
 * The manifest remains the durable intent record until cleanup and parent
 * metadata are both durable.  Recovery callbacks therefore MUST be idempotent:
 * a crash can occur after reclaiming pages/tree state but before removing the
 * sidecar, and reopen must safely repeat the same reclaim operation.
 *
 * This header deliberately does not own Pager/catalog filesystem mechanics.
 * It defines the fail-closed state classification and the only safe ordering
 * between catalog observation, reclaim, durability, and manifest removal.
 */

typedef enum TinyDBCompactV2MigrationStrictRecoveryAction {
    TINYDB_COMPACT_V2_MIGRATION_STRICT_INVALID = 0,
    TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING = 1,
    TINYDB_COMPACT_V2_MIGRATION_STRICT_KEEP_NEW_RECLAIM_OLD = 2
} TinyDBCompactV2MigrationStrictRecoveryAction;

typedef bool (*TinyDBCompactV2MigrationReadCatalogFn)(
    void* context,
    uint32_t table_id,
    uint32_t* root_page_num_out,
    uint64_t* schema_generation_out);

typedef bool (*TinyDBCompactV2MigrationReclaimStagingFn)(
    void* context,
    const uint32_t* claimed_pages,
    uint32_t claimed_page_count);

typedef bool (*TinyDBCompactV2MigrationReclaimOldTreeFn)(
    void* context,
    uint32_t old_root_page_num);

typedef bool (*TinyDBCompactV2MigrationRecoveryStepFn)(void* context);

typedef struct TinyDBCompactV2MigrationRecoveryOps {
    void* context;
    TinyDBCompactV2MigrationReadCatalogFn read_catalog;
    TinyDBCompactV2MigrationReclaimStagingFn reclaim_staging_pages;
    TinyDBCompactV2MigrationReclaimOldTreeFn reclaim_old_tree;
    TinyDBCompactV2MigrationRecoveryStepFn sync_reclaim;
    TinyDBCompactV2MigrationRecoveryStepFn remove_manifest;
    TinyDBCompactV2MigrationRecoveryStepFn sync_parent;
} TinyDBCompactV2MigrationRecoveryOps;

typedef struct TinyDBCompactV2MigrationRecoveryResult {
    TinyDBCompactV2MigrationStrictRecoveryAction action;
    uint32_t authoritative_root_page_num;
    uint64_t authoritative_schema_generation;
} TinyDBCompactV2MigrationRecoveryResult;

static inline bool tinydb_compact_v2_migration_recovery_ops_are_valid(
    const TinyDBCompactV2MigrationRecoveryOps* ops) {
    return ops != NULL && ops->read_catalog != NULL &&
           ops->reclaim_staging_pages != NULL && ops->reclaim_old_tree != NULL &&
           ops->sync_reclaim != NULL && ops->remove_manifest != NULL &&
           ops->sync_parent != NULL;
}

/*
 * Stricter reopen classifier than the legacy root/generation-only helper.
 *
 * DURABLE_UNPUBLISHED may legitimately observe either the old catalog state
 * (crash before publication) or the new catalog state (crash after catalog
 * publication but before the phase sidecar rewrite).
 *
 * CATALOG_PUBLISHED may only observe the new catalog state.  Observing the old
 * root after that phase was made durable is a contradictory history and must
 * fail closed instead of reclaiming the staged tree.
 */
static inline TinyDBCompactV2MigrationStrictRecoveryAction
 tinydb_compact_v2_migration_manifest_classify_recovery_strict(
    const TinyDBCompactV2MigrationManifest* manifest,
    uint32_t authoritative_root_page_num,
    uint64_t authoritative_schema_generation) {
    if (!tinydb_compact_v2_migration_manifest_is_valid(manifest)) {
        return TINYDB_COMPACT_V2_MIGRATION_STRICT_INVALID;
    }

    if (authoritative_root_page_num == manifest->old_root_page_num &&
        authoritative_schema_generation == manifest->old_schema_generation) {
        return manifest->phase ==
                       TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED
                   ? TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING
                   : TINYDB_COMPACT_V2_MIGRATION_STRICT_INVALID;
    }

    if (authoritative_root_page_num == manifest->staged_root_page_num &&
        authoritative_schema_generation == manifest->new_schema_generation) {
        return TINYDB_COMPACT_V2_MIGRATION_STRICT_KEEP_NEW_RECLAIM_OLD;
    }

    return TINYDB_COMPACT_V2_MIGRATION_STRICT_INVALID;
}

/*
 * Execute one complete reopen-recovery attempt.
 *
 * Ordering is intentionally strict:
 *   1. read authoritative catalog state
 *   2. classify the manifest/catalog pair fail-closed
 *   3. perform exactly one idempotent reclaim action
 *   4. make reclaim durable
 *   5. remove the manifest
 *   6. sync parent metadata for durable manifest removal
 *   7. publish the result to the caller
 *
 * On any failure result_out remains zeroed.  In particular, a failure after
 * reclaim but before manifest removal is not reported as success; the retained
 * manifest drives the same idempotent recovery action on the next reopen.
 */
static inline bool tinydb_compact_v2_migration_recover_reopen(
    const TinyDBCompactV2MigrationManifest* manifest,
    const TinyDBCompactV2MigrationRecoveryOps* ops,
    TinyDBCompactV2MigrationRecoveryResult* result_out) {
    uint32_t authoritative_root = 0u;
    uint64_t authoritative_generation = UINT64_C(0);
    TinyDBCompactV2MigrationStrictRecoveryAction action =
        TINYDB_COMPACT_V2_MIGRATION_STRICT_INVALID;
    TinyDBCompactV2MigrationRecoveryResult candidate;

    if (result_out != NULL) memset(result_out, 0, sizeof(*result_out));
    if (manifest == NULL || result_out == NULL ||
        !tinydb_compact_v2_migration_manifest_is_valid(manifest) ||
        !tinydb_compact_v2_migration_recovery_ops_are_valid(ops)) {
        return false;
    }

    if (!ops->read_catalog(
            ops->context,
            manifest->table_id,
            &authoritative_root,
            &authoritative_generation)) {
        return false;
    }

    action = tinydb_compact_v2_migration_manifest_classify_recovery_strict(
        manifest, authoritative_root, authoritative_generation);
    if (action == TINYDB_COMPACT_V2_MIGRATION_STRICT_INVALID) return false;

    if (action == TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING) {
        if (!ops->reclaim_staging_pages(
                ops->context,
                manifest->claimed_pages,
                manifest->claimed_page_count)) {
            return false;
        }
    } else {
        if (!ops->reclaim_old_tree(ops->context, manifest->old_root_page_num)) {
            return false;
        }
    }

    if (!ops->sync_reclaim(ops->context)) return false;
    if (!ops->remove_manifest(ops->context)) return false;
    if (!ops->sync_parent(ops->context)) return false;

    memset(&candidate, 0, sizeof(candidate));
    candidate.action = action;
    candidate.authoritative_root_page_num = authoritative_root;
    candidate.authoritative_schema_generation = authoritative_generation;
    *result_out = candidate;
    return true;
}

#endif
