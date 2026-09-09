#ifndef SCHEMA_REPACK_TABLE_CATALOG_PUBLISH_H
#define SCHEMA_REPACK_TABLE_CATALOG_PUBLISH_H

#include "compact_v2_migration_recovery.h"
#include "schema_repack_table_migration_prepare.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Catalog-publication seam for a fully prepared schema-repack migration.
 *
 * Safe ordering is deliberately explicit:
 *
 *   durable destination tree
 *     -> durable DURABLE_UNPUBLISHED recovery intent
 *     -> durable catalog switch to staged root/new schema generation
 *     -> durable CATALOG_PUBLISHED sidecar rewrite
 *
 * The strict reopen classifier intentionally accepts the new catalog state
 * while the manifest is still DURABLE_UNPUBLISHED.  Therefore a crash after
 * the durable catalog switch but before the phase rewrite is recoverable: the
 * new tree stays authoritative and recovery reclaims the old tree.
 *
 * This helper does not prescribe the catalog implementation.  The supplied
 * callback must atomically and durably publish the table root/generation while
 * verifying the expected old root/generation.  This keeps schema-repack logic
 * independent of the current catalog representation while preserving the
 * compare-and-publish contract needed to reject stale migrations.
 */
typedef bool (*TinyDBSchemaRepackPublishCatalogDurableFn)(
    void* context,
    uint32_t table_id,
    uint32_t expected_old_root_page_num,
    uint64_t expected_old_schema_generation,
    uint32_t new_root_page_num,
    uint64_t new_schema_generation);

typedef struct TinyDBSchemaRepackCatalogPublishOps {
    void* context;
    TinyDBSchemaRepackPublishCatalogDurableFn publish_catalog_durable;
} TinyDBSchemaRepackCatalogPublishOps;

typedef struct TinyDBSchemaRepackTableCatalogPublishResult {
    TinyDBCompactV2MigrationManifest manifest;
    size_t encoded_length;
    bool catalog_published_durable;
    bool phase_published_durable;
    bool ready;
} TinyDBSchemaRepackTableCatalogPublishResult;

static inline bool tinydb_schema_repack_catalog_publish_ops_are_valid(
    const TinyDBSchemaRepackCatalogPublishOps* ops) {
    return ops != NULL && ops->publish_catalog_durable != NULL;
}

static inline bool tinydb_schema_repack_table_publish_catalog(
    const TinyDBSchemaRepackTableMigrationPrepareResult* prepared,
    const TinyDBSchemaRepackCatalogPublishOps* catalog_ops,
    unsigned char* manifest_encode_scratch,
    size_t manifest_encode_scratch_capacity,
    const TinyDBCompactV2MigrationManifestStoreOps* store_ops,
    TinyDBSchemaRepackTableCatalogPublishResult* result,
    char* message,
    size_t message_size) {
    const TinyDBCompactV2MigrationManifest* manifest;
    TinyDBCompactV2MigrationManifest published_manifest;
    size_t encoded_length = 0u;

    if (result != NULL) memset(result, 0, sizeof(*result));
    if (message != NULL && message_size > 0u) message[0] = '\0';
    memset(&published_manifest, 0, sizeof(published_manifest));

    if (prepared == NULL || result == NULL || manifest_encode_scratch == NULL ||
        !tinydb_schema_repack_catalog_publish_ops_are_valid(catalog_ops) ||
        !tinydb_compact_v2_migration_manifest_store_ops_are_valid(store_ops) ||
        !prepared->ready || !prepared->destination_durable ||
        !prepared->recovery_intent_durable || !prepared->durable_stage.ready ||
        !prepared->intent.ready) {
        tinydb_schema_repack_table_durable_set_message(
            message, message_size,
            "schema repack catalog publication arguments are invalid");
        return false;
    }

    manifest = &prepared->intent.manifest;
    if (!tinydb_compact_v2_migration_manifest_is_valid(manifest) ||
        manifest->phase != TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED ||
        manifest->staged_root_page_num != prepared->durable_stage.root_page_num ||
        manifest->claimed_page_count != prepared->durable_stage.claimed_page_count ||
        prepared->durable_stage.root_page_num == 0u ||
        prepared->durable_stage.claimed_page_count == 0u) {
        tinydb_schema_repack_table_durable_set_message(
            message, message_size,
            "schema repack prepared migration identity is inconsistent");
        return false;
    }

    if (!catalog_ops->publish_catalog_durable(
            catalog_ops->context,
            manifest->table_id,
            manifest->old_root_page_num,
            manifest->old_schema_generation,
            manifest->staged_root_page_num,
            manifest->new_schema_generation)) {
        tinydb_schema_repack_table_durable_set_message(
            message, message_size,
            "schema repack catalog publication failed before durable switch");
        return false;
    }

    /*
     * From this point onward the new catalog state is authoritative.  Preserve
     * that fact even if the phase-sidecar rewrite fails: reopen can safely
     * classify DURABLE_UNPUBLISHED + new root/generation as KEEP_NEW_RECLAIM_OLD.
     */
    result->manifest = *manifest;
    result->catalog_published_durable = true;

    if (!tinydb_compact_v2_migration_manifest_mark_catalog_published_durable(
            manifest,
            manifest_encode_scratch,
            manifest_encode_scratch_capacity,
            store_ops,
            &published_manifest,
            &encoded_length)) {
        tinydb_schema_repack_table_durable_set_message(
            message, message_size,
            "schema repack catalog is durable but phase publication failed");
        return false;
    }

    result->manifest = published_manifest;
    result->encoded_length = encoded_length;
    result->phase_published_durable = true;
    result->ready = true;
    return true;
}

#endif /* SCHEMA_REPACK_TABLE_CATALOG_PUBLISH_H */
