#ifndef SCHEMA_REPACK_TABLE_DURABLE_H
#define SCHEMA_REPACK_TABLE_DURABLE_H

#include "record_payload_try_scan.h"
#include "schema_repack_staging_durable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Production read-to-durability seam for a schema-repack migration.
 *
 * The source side is the authoritative schema-sized payload scanner. Accepted
 * rows first enter caller-owned private compact-V2 leaf images only. The Pager
 * is not touched until the complete source scan succeeds and its row count
 * agrees with the staging builder. The complete destination row set is then
 * rebound to transaction-scoped Pager page identities, materialized as a
 * validated hierarchy, WAL-committed, checkpointed, and read back before the
 * durable result is published.
 *
 * This API deliberately stops before migration-manifest or catalog publication.
 * A successful result therefore means that the widened destination tree is
 * durable but still non-authoritative. On any failure after Pager page claims,
 * the caller must roll back/abort the dedicated migration transaction according
 * to the Pager contract; result remains zero/not-ready.
 *
 * The leaf chain must be freshly initialized with private non-zero page numbers
 * and enough capacity for the complete destination row set. Multi-leaf output
 * is currently required by the Pager schema-repack materializer.
 */
static inline void tinydb_schema_repack_table_durable_set_message(
    char* message,
    size_t message_size,
    const char* detail) {
    if (message == NULL || message_size == 0u) return;
    if (detail == NULL || detail[0] == '\0') {
        detail = "schema repack authoritative scan failed before durable staging";
    }
    (void)snprintf(message, message_size, "%s", detail);
}

static inline bool tinydb_schema_repack_table_make_durable_unpublished(
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
    TinyDBSchemaRepackDurableStageResult* result,
    char* message,
    size_t message_size) {
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (message != NULL && message_size > 0u) message[0] = '\0';

    if (table == NULL || table->pager == NULL || !table->pager->in_transaction ||
        source_schema == NULL || destination_schema == NULL || leaves == NULL ||
        staging == NULL || hierarchy == NULL || internal_images == NULL ||
        claimed_pages == NULL || result == NULL) {
        tinydb_schema_repack_table_durable_set_message(
            message, message_size, "schema repack durable table-scan arguments are invalid");
        return false;
    }

    char detail[192];
    if (!tinydb_schema_repack_staging_init(staging,
                                           source_schema,
                                           destination_schema,
                                           leaves,
                                           detail,
                                           sizeof(detail))) {
        tinydb_schema_repack_table_durable_set_message(message, message_size, detail);
        return false;
    }

    bool scan_complete = false;
    uint32_t rows_scanned = tinydb_record_payload_try_scan(
        table,
        source_schema,
        tinydb_schema_repack_staging_visit,
        staging,
        &scan_complete,
        detail,
        sizeof(detail));

    if (!scan_complete || staging->failed ||
        staging->rows_staged != (uint64_t)rows_scanned ||
        leaves->row_count != (uint64_t)rows_scanned ||
        !tinydb_compact_v2_staging_leaf_chain_validate(leaves)) {
        const char* failure = staging->message[0] != '\0'
            ? staging->message
            : detail;
        tinydb_schema_repack_table_durable_set_message(message, message_size, failure);
        return false;
    }

    if (!tinydb_schema_repack_staging_make_durable_unpublished(
            table->pager,
            staging,
            (uint64_t)rows_scanned,
            hierarchy,
            internal_images,
            internal_capacity,
            claimed_pages,
            claimed_capacity,
            result,
            message,
            message_size) ||
        !result->ready || result->row_count != (uint64_t)rows_scanned ||
        result->root_page_num == 0u || result->claimed_page_count == 0u) {
        if (message != NULL && message_size > 0u && message[0] == '\0') {
            tinydb_schema_repack_table_durable_set_message(
                message, message_size,
                "schema repack authoritative scan did not produce a durable complete tree");
        }
        memset(result, 0, sizeof(*result));
        return false;
    }

    return true;
}

#endif /* SCHEMA_REPACK_TABLE_DURABLE_H */
