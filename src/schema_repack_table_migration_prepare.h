#ifndef SCHEMA_REPACK_TABLE_MIGRATION_PREPARE_H
#define SCHEMA_REPACK_TABLE_MIGRATION_PREPARE_H

#include "schema_repack_migration_manifest.h"
#include "schema_repack_table_durable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Production pre-catalog preparation seam for a schema-repack migration.
 *
 * Ordering is intentionally strict:
 *
 *   authoritative source scan
 *     -> complete private destination tree
 *     -> Pager materialization + WAL commit + checkpoint
 *     -> durable DURABLE_UNPUBLISHED recovery intent
 *     -> caller may later publish catalog/root + schema generation
 *
 * A subtle but important contract is exposed on manifest-publication failure.
 * Once Pager commit/checkpoint has succeeded the destination pages are already
 * durable and cannot honestly be hidden behind an all-zero result.  The caller
 * must know exactly which durable-but-unpublished root/pages may require local
 * cleanup or conservative reopen handling.  Accordingly, durable_stage is
 * retained when the second step fails while intent/result.ready stay false.
 *
 * Before the durable-stage boundary, all outputs remain zero and neither
 * manifest-store callback nor catalog mutation is attempted here.
 */
typedef struct TinyDBSchemaRepackTableMigrationPrepareResult {
    TinyDBSchemaRepackDurableStageResult durable_stage;
    TinyDBSchemaRepackMigrationIntentResult intent;
    bool destination_durable;
    bool recovery_intent_durable;
    bool ready;
} TinyDBSchemaRepackTableMigrationPrepareResult;

static inline bool tinydb_schema_repack_table_prepare_migration(
    Table* table,
    const TableSchema* source_schema,
    const TableSchema* destination_schema,
    TinyDBCompactV2StagingLeafChain* leaves,
    TinyDBSchemaRepackStaging* staging,
    TinyDBCompactV2StagingHierarchy* hierarchy,
    unsigned char* internal_images,
    uint32_t internal_capacity,
    uint32_t* claimed_pages,
    uint32_t claimed_capacity,
    uint32_t table_id,
    uint32_t old_root_page_num,
    uint64_t old_schema_generation,
    uint64_t new_schema_generation,
    unsigned char* manifest_encode_scratch,
    size_t manifest_encode_scratch_capacity,
    const TinyDBCompactV2MigrationManifestStoreOps* store_ops,
    TinyDBSchemaRepackTableMigrationPrepareResult* result,
    char* message,
    size_t message_size) {
    TinyDBSchemaRepackDurableStageResult durable_stage;
    TinyDBSchemaRepackMigrationIntentResult intent;

    if (result != NULL) memset(result, 0, sizeof(*result));
    if (message != NULL && message_size > 0u) message[0] = '\0';
    memset(&durable_stage, 0, sizeof(durable_stage));
    memset(&intent, 0, sizeof(intent));

    if (result == NULL || manifest_encode_scratch == NULL || store_ops == NULL ||
        table_id == 0u || old_root_page_num == 0u ||
        old_schema_generation >= new_schema_generation) {
        tinydb_schema_repack_table_durable_set_message(
            message, message_size,
            "schema repack migration preparation arguments are invalid");
        return false;
    }

    if (!tinydb_schema_repack_table_make_durable_unpublished(
            table,
            source_schema,
            destination_schema,
            leaves,
            staging,
            hierarchy,
            internal_images,
            internal_capacity,
            claimed_pages,
            claimed_capacity,
            &durable_stage,
            message,
            message_size) ||
        !durable_stage.ready) {
        return false;
    }

    result->durable_stage = durable_stage;
    result->destination_durable = true;

    if (!tinydb_schema_repack_publish_durable_migration_intent(
            &durable_stage,
            table_id,
            old_root_page_num,
            old_schema_generation,
            new_schema_generation,
            claimed_pages,
            durable_stage.claimed_page_count,
            manifest_encode_scratch,
            manifest_encode_scratch_capacity,
            store_ops,
            &intent) ||
        !intent.ready) {
        tinydb_schema_repack_table_durable_set_message(
            message, message_size,
            "schema repack destination is durable but recovery intent publication failed");
        return false;
    }

    result->intent = intent;
    result->recovery_intent_durable = true;
    result->ready = true;
    return true;
}

#endif /* SCHEMA_REPACK_TABLE_MIGRATION_PREPARE_H */
