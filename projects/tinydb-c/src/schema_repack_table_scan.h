#ifndef SCHEMA_REPACK_TABLE_SCAN_H
#define SCHEMA_REPACK_TABLE_SCAN_H

#include "record_payload_try_scan.h"
#include "schema_repack_staging_tree.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Production composition seam for the read side of a schema-repack migration.
 *
 * The authoritative source table is traversed through the backpressure-safe
 * schema-sized payload scanner. Accepted rows are written only into the
 * caller-owned private compact-V2 staging tree. No Pager allocation, WAL,
 * migration manifest, catalog root, or schema generation is published here.
 *
 * tinydb_record_payload_try_scan() deliberately permits visitor side effects
 * from an accepted prefix before a later traversal failure. That behaviour is
 * safe here because the prefix lives only in private staging memory. This
 * wrapper publishes TinyDBSchemaRepackStagingTreeResult only after the complete
 * source scan succeeds and the complete private B+tree validates. On any scan,
 * repack, capacity, or hierarchy failure, result remains zero/not-ready and the
 * caller must discard the private staging buffers.
 */
static inline void tinydb_schema_repack_table_scan_set_message(
    char* message,
    size_t message_size,
    const char* detail) {
    if (message == NULL || message_size == 0u) return;
    if (detail == NULL || detail[0] == '\0') {
        detail = "schema repack source scan failed";
    }
    (void)snprintf(message, message_size, "%s", detail);
}

static inline bool tinydb_schema_repack_stage_table_scan(
    Table* table,
    const TableSchema* source_schema,
    const TableSchema* destination_schema,
    TinyDBCompactV2StagingLeafChain* leaves,
    TinyDBSchemaRepackStaging* staging,
    TinyDBCompactV2StagingHierarchy* hierarchy,
    unsigned char* internal_images,
    const uint32_t* internal_page_numbers,
    uint32_t internal_capacity,
    TinyDBSchemaRepackStagingTreeResult* result,
    char* message,
    size_t message_size) {
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (message != NULL && message_size > 0u) message[0] = '\0';

    if (table == NULL || table->pager == NULL || source_schema == NULL ||
        destination_schema == NULL || leaves == NULL || staging == NULL ||
        result == NULL) {
        tinydb_schema_repack_table_scan_set_message(
            message, message_size, "schema repack table-scan arguments are invalid");
        return false;
    }

    char detail[192];
    if (!tinydb_schema_repack_staging_init(staging,
                                           source_schema,
                                           destination_schema,
                                           leaves,
                                           detail,
                                           sizeof(detail))) {
        tinydb_schema_repack_table_scan_set_message(message, message_size, detail);
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
        leaves->row_count != (uint64_t)rows_scanned) {
        const char* failure = staging->message[0] != '\0'
            ? staging->message
            : detail;
        tinydb_schema_repack_table_scan_set_message(message, message_size, failure);
        return false;
    }

    if (!tinydb_schema_repack_staging_build_tree(staging,
                                                 (uint64_t)rows_scanned,
                                                 hierarchy,
                                                 internal_images,
                                                 internal_page_numbers,
                                                 internal_capacity,
                                                 result,
                                                 detail,
                                                 sizeof(detail))) {
        tinydb_schema_repack_table_scan_set_message(message, message_size, detail);
        return false;
    }

    if (!result->ready || result->row_count != (uint64_t)rows_scanned) {
        memset(result, 0, sizeof(*result));
        tinydb_schema_repack_table_scan_set_message(
            message, message_size, "schema repack table scan did not publish a complete tree");
        return false;
    }

    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

#endif /* SCHEMA_REPACK_TABLE_SCAN_H */
