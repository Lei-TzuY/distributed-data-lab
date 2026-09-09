#ifndef COMPACT_V2_MIGRATION_PAGER_RECOVERY_H
#define COMPACT_V2_MIGRATION_PAGER_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "compact_v2_migration_pager_reclaim.h"
#include "compact_v2_migration_recovery.h"
#include "fixed_v1_tree_reclaim.h"
#include "pager.h"

/*
 * Pager-backed adapter for the compact-V2 reopen recovery executor.
 *
 * Recovery starts with a fresh Pager transaction.  The staging-reclaim path is
 * implemented directly with the manifest-owned claimed-page primitive.  The
 * publication-success path now has a built-in fixed-V1 ownership walk for
 * nonzero old roots; callers may still override it when a table uses a
 * different topology or the reserved historical page-zero root.
 *
 * sync_reclaim is the durability boundary: it commits the allocator/tree
 * mutation and checkpoints it before manifest removal can begin.  If recovery
 * fails before that boundary, the wrapper rolls the Pager transaction back.
 * If manifest removal fails after the checkpoint, the allocator mutation is
 * intentionally retained and the durable manifest drives an idempotent retry.
 */

typedef bool (*TinyDBCompactV2MigrationPagerReclaimOldTreeFn)(
    void* context,
    Pager* pager,
    uint32_t old_root_page_num);

typedef struct TinyDBCompactV2MigrationPagerRecoveryAdapter {
    Pager* pager;
    void* context;
    TinyDBCompactV2MigrationReadCatalogFn read_catalog;
    TinyDBCompactV2MigrationPagerReclaimOldTreeFn reclaim_old_tree;
    TinyDBCompactV2MigrationRecoveryStepFn remove_manifest;
    TinyDBCompactV2MigrationRecoveryStepFn sync_parent;
    bool reclaim_was_checkpointed;
} TinyDBCompactV2MigrationPagerRecoveryAdapter;

static inline bool tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(
    const TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter) {
    return adapter != NULL && adapter->pager != NULL &&
           adapter->read_catalog != NULL &&
           adapter->remove_manifest != NULL && adapter->sync_parent != NULL;
}

static inline bool tinydb_compact_v2_migration_pager_recovery_read_catalog(
    void* context,
    uint32_t table_id,
    uint32_t* root_page_num_out,
    uint64_t* schema_generation_out) {
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter =
        (TinyDBCompactV2MigrationPagerRecoveryAdapter*)context;
    if (!tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(adapter)) {
        return false;
    }
    return adapter->read_catalog(
        adapter->context, table_id, root_page_num_out, schema_generation_out);
}

static inline bool tinydb_compact_v2_migration_pager_recovery_reclaim_staging(
    void* context,
    const uint32_t* claimed_pages,
    uint32_t claimed_page_count) {
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter =
        (TinyDBCompactV2MigrationPagerRecoveryAdapter*)context;
    if (!tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(adapter) ||
        !adapter->pager->in_transaction) {
        return false;
    }
    return tinydb_compact_v2_migration_pager_reclaim_claims(
        adapter->pager, claimed_pages, claimed_page_count);
}

static inline bool tinydb_compact_v2_migration_pager_recovery_reclaim_old_tree(
    void* context,
    uint32_t old_root_page_num) {
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter =
        (TinyDBCompactV2MigrationPagerRecoveryAdapter*)context;
    if (!tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(adapter) ||
        !adapter->pager->in_transaction) {
        return false;
    }
    if (adapter->reclaim_old_tree != NULL) {
        return adapter->reclaim_old_tree(
            adapter->context, adapter->pager, old_root_page_num);
    }
    return tinydb_fixed_v1_tree_reclaim(adapter->pager, old_root_page_num);
}

static inline bool tinydb_compact_v2_migration_pager_recovery_sync_reclaim(
    void* context) {
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter =
        (TinyDBCompactV2MigrationPagerRecoveryAdapter*)context;
    if (!tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(adapter) ||
        !adapter->pager->in_transaction || adapter->reclaim_was_checkpointed) {
        return false;
    }

    pager_commit(adapter->pager);
    pager_checkpoint(adapter->pager);
    adapter->reclaim_was_checkpointed = true;
    return true;
}

static inline bool tinydb_compact_v2_migration_pager_recovery_remove_manifest(
    void* context) {
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter =
        (TinyDBCompactV2MigrationPagerRecoveryAdapter*)context;
    if (!tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(adapter) ||
        !adapter->reclaim_was_checkpointed || adapter->pager->in_transaction) {
        return false;
    }
    return adapter->remove_manifest(adapter->context);
}

static inline bool tinydb_compact_v2_migration_pager_recovery_sync_parent(
    void* context) {
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter =
        (TinyDBCompactV2MigrationPagerRecoveryAdapter*)context;
    if (!tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(adapter) ||
        !adapter->reclaim_was_checkpointed || adapter->pager->in_transaction) {
        return false;
    }
    return adapter->sync_parent(adapter->context);
}

static inline bool tinydb_compact_v2_migration_pager_recover_reopen(
    const TinyDBCompactV2MigrationManifest* manifest,
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter,
    TinyDBCompactV2MigrationRecoveryResult* result_out) {
    TinyDBCompactV2MigrationRecoveryOps ops;
    bool recovered = false;

    if (result_out != NULL) memset(result_out, 0, sizeof(*result_out));
    if (manifest == NULL || result_out == NULL ||
        !tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(adapter) ||
        adapter->pager->in_transaction) {
        return false;
    }

    adapter->reclaim_was_checkpointed = false;
    memset(&ops, 0, sizeof(ops));
    ops.context = adapter;
    ops.read_catalog = tinydb_compact_v2_migration_pager_recovery_read_catalog;
    ops.reclaim_staging_pages =
        tinydb_compact_v2_migration_pager_recovery_reclaim_staging;
    ops.reclaim_old_tree =
        tinydb_compact_v2_migration_pager_recovery_reclaim_old_tree;
    ops.sync_reclaim = tinydb_compact_v2_migration_pager_recovery_sync_reclaim;
    ops.remove_manifest =
        tinydb_compact_v2_migration_pager_recovery_remove_manifest;
    ops.sync_parent = tinydb_compact_v2_migration_pager_recovery_sync_parent;

    pager_begin_transaction(adapter->pager);
    if (!adapter->pager->in_transaction) return false;

    recovered = tinydb_compact_v2_migration_recover_reopen(
        manifest, &ops, result_out);
    if (!recovered && adapter->pager->in_transaction) {
        pager_rollback(adapter->pager);
    }
    return recovered;
}

#endif
