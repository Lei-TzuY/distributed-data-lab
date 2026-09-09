#ifndef SCHEMA_REPACK_MIGRATION_MANIFEST_H
#define SCHEMA_REPACK_MIGRATION_MANIFEST_H

#include "compact_v2_migration_manifest_store.h"
#include "schema_repack_staging_durable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Recovery-intent publication seam for schema-repack migrations.
 *
 * A widening migration may publish a DURABLE_UNPUBLISHED manifest only after
 * the destination tree has crossed the durable staging boundary.  This keeps
 * the ordering explicit:
 *
 *   durable destination tree -> durable recovery intent -> catalog switch
 *
 * No catalog state is changed here.  On success, reopen recovery can safely
 * reclaim the claimed staging pages while the catalog still identifies the old
 * root/generation.  Outputs remain zeroed until the sidecar publication itself
 * is durable.
 */
typedef struct TinyDBSchemaRepackMigrationIntentResult {
    TinyDBCompactV2MigrationManifest manifest;
    size_t encoded_length;
    bool ready;
} TinyDBSchemaRepackMigrationIntentResult;

static inline bool tinydb_schema_repack_publish_durable_migration_intent(
    const TinyDBSchemaRepackDurableStageResult* durable_stage,
    uint32_t table_id,
    uint32_t old_root_page_num,
    uint64_t old_schema_generation,
    uint64_t new_schema_generation,
    const uint32_t* claimed_pages,
    uint32_t claimed_page_count,
    unsigned char* encode_scratch,
    size_t encode_scratch_capacity,
    const TinyDBCompactV2MigrationManifestStoreOps* store_ops,
    TinyDBSchemaRepackMigrationIntentResult* result) {
    TinyDBCompactV2MigrationManifest candidate;
    size_t encoded_length = 0u;

    if (result != NULL) memset(result, 0, sizeof(*result));
    if (durable_stage == NULL || claimed_pages == NULL ||
        encode_scratch == NULL || store_ops == NULL || result == NULL ||
        !durable_stage->ready || durable_stage->root_page_num == 0u ||
        durable_stage->claimed_page_count == 0u ||
        durable_stage->claimed_page_count != claimed_page_count ||
        claimed_page_count == 0u || table_id == 0u || old_root_page_num == 0u ||
        old_schema_generation >= new_schema_generation) {
        return false;
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate.table_id = table_id;
    candidate.old_root_page_num = old_root_page_num;
    candidate.staged_root_page_num = durable_stage->root_page_num;
    candidate.old_schema_generation = old_schema_generation;
    candidate.new_schema_generation = new_schema_generation;
    candidate.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED;
    candidate.claimed_page_count = claimed_page_count;
    candidate.claimed_pages = claimed_pages;

    if (!tinydb_compact_v2_migration_manifest_is_valid(&candidate) ||
        !tinydb_compact_v2_migration_manifest_publish_durable(
            &candidate,
            encode_scratch,
            encode_scratch_capacity,
            store_ops,
            &encoded_length)) {
        return false;
    }

    result->manifest = candidate;
    result->encoded_length = encoded_length;
    result->ready = true;
    return true;
}

#endif /* SCHEMA_REPACK_MIGRATION_MANIFEST_H */
