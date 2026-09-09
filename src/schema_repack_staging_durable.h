#ifndef SCHEMA_REPACK_STAGING_DURABLE_H
#define SCHEMA_REPACK_STAGING_DURABLE_H

#include "compact_v2_staging_durable.h"
#include "schema_repack_staging_pager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Durable-but-unpublished composition seam for schema-repack migrations.
 *
 * This joins the two deliberately separate safety boundaries used by widening
 * migrations:
 *   1. build and validate the complete destination row set, then materialize
 *      the complete compact-V2 hierarchy into an isolated Pager transaction;
 *   2. prove that the transaction dirty set is exactly the claimed hierarchy,
 *      WAL-commit it, checkpoint it, and verify the persisted page images.
 *
 * No catalog root, schema metadata, schema generation, or migration manifest is
 * published here.  A successful result therefore means only that the new tree
 * is durable and still non-authoritative.  The caller may then durably record
 * recovery intent before publishing the catalog switch.
 *
 * The externally visible result is kept zeroed until both boundaries succeed.
 * In particular, a merely materialized dirty tree is never exposed as durable.
 */
typedef struct {
    uint32_t root_page_num;
    uint32_t leaf_page_count;
    uint32_t internal_page_count;
    uint32_t claimed_page_count;
    uint64_t row_count;
    bool ready;
} TinyDBSchemaRepackDurableStageResult;

static inline bool tinydb_schema_repack_staging_make_durable_unpublished(
    Pager* pager,
    TinyDBSchemaRepackStaging* staging,
    uint64_t expected_rows,
    TinyDBCompactV2StagingHierarchy* hierarchy,
    unsigned char* internal_images,
    uint32_t internal_capacity,
    uint32_t* claimed_pages,
    uint32_t claimed_capacity,
    TinyDBSchemaRepackDurableStageResult* result,
    char* message,
    size_t message_size) {
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (message != NULL && message_size > 0u) message[0] = '\0';

    if (pager == NULL || staging == NULL || hierarchy == NULL ||
        internal_images == NULL || claimed_pages == NULL || result == NULL) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size,
            "schema repack durable staging arguments are invalid");
        return false;
    }

    TinyDBSchemaRepackPagerStageResult pager_stage;
    if (!tinydb_schema_repack_staging_materialize_pager(
            pager,
            staging,
            expected_rows,
            hierarchy,
            internal_images,
            internal_capacity,
            claimed_pages,
            claimed_capacity,
            &pager_stage,
            message,
            message_size) ||
        !pager_stage.ready || pager_stage.root_page_num == 0u ||
        pager_stage.claimed_page_count == 0u ||
        pager_stage.row_count != expected_rows) {
        if (message != NULL && message_size > 0u && message[0] == '\0') {
            tinydb_schema_repack_staging_pager_set_message(
                message, message_size,
                "schema repack Pager staging did not produce a complete tree");
        }
        return false;
    }

    uint32_t durable_root = 0u;
    if (!tinydb_compact_v2_staging_pager_make_durable_unpublished(
            pager,
            hierarchy,
            claimed_pages,
            pager_stage.claimed_page_count,
            &durable_root) ||
        durable_root == 0u || durable_root != pager_stage.root_page_num) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size,
            "schema repack staging tree failed durable unpublished validation");
        return false;
    }

    result->root_page_num = durable_root;
    result->leaf_page_count = pager_stage.leaf_page_count;
    result->internal_page_count = pager_stage.internal_page_count;
    result->claimed_page_count = pager_stage.claimed_page_count;
    result->row_count = pager_stage.row_count;
    result->ready = true;
    return true;
}

#endif /* SCHEMA_REPACK_STAGING_DURABLE_H */
