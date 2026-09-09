#include "diagnostics.h"
#include "pager_try_pin.h"
#include "table_stats_try.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool stats_fail(char* message,
                       size_t message_size,
                       const char* format,
                       ...) {
    if (message != NULL && message_size > 0u) {
        va_list args;
        va_start(args, format);
        vsnprintf(message, message_size, format, args);
        va_end(args);
    }
    return false;
}

static bool stats_page_is_free(const Pager* pager, uint32_t page_num) {
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static const TableSchema* stats_routed_schema(const Table* table) {
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (table->catalog.schemas[i].root_page_num == table->root_page_num) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

/*
 * Additive non-fatal counterpart to the historical void db_get_stats().
 * The legacy API still walks cursors and directly get_page()s allocated pages.
 * This version preserves its routed-row/global-page counting semantics while
 * using diagnostics-aware traversal plus one-page-at-a-time try-pin reads.
 * Nothing is published until the complete pass succeeds.
 */
bool db_try_get_stats(Table* table,
                      TableStats* stats,
                      char* message,
                      size_t message_size) {
    if (stats != NULL) memset(stats, 0, sizeof(*stats));
    if (message != NULL && message_size > 0u) message[0] = '\0';

    if (table == NULL || table->pager == NULL || stats == NULL) {
        return stats_fail(message, message_size, "invalid table-stat arguments");
    }

    const TableSchema* schema = stats_routed_schema(table);
    if (schema == NULL) {
        return stats_fail(message,
                          message_size,
                          "no catalog table owns routed root page %u",
                          table->root_page_num);
    }

    TinyDBTreeStats tree_stats;
    char tree_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_get_tree_stats_diagnostic(table,
                                          schema->name,
                                          &tree_stats,
                                          tree_message,
                                          sizeof(tree_message))) {
        return stats_fail(message,
                          message_size,
                          "routed tree statistics failed: %s",
                          tree_message[0] != '\0'
                              ? tree_message
                              : "unknown diagnostic error");
    }

    TableStats local;
    memset(&local, 0, sizeof(local));
    local.total_pages = table->pager->num_pages;
    local.free_pages = table->pager->free_page_count;
    local.total_rows = tree_stats.total_rows;
    local.leaf_pages = tree_stats.leaf_pages;

    for (uint32_t page_num = 0u; page_num < table->pager->num_pages; page_num++) {
        if (stats_page_is_free(table->pager, page_num)) continue;

        PagerPageHandle handle;
        PagerTryPinStatus status = pager_try_pin_existing_page_handle(
            table->pager,
            page_num,
            &handle);
        if (status != PAGER_TRY_PIN_OK) {
            return stats_fail(message,
                              message_size,
                              "page %u could not be acquired while counting internal pages: %s",
                              page_num,
                              pager_try_pin_status_string(status));
        }

        if (get_node_type(handle.data) == NODE_INTERNAL) {
            local.internal_pages++;
        }

        if (!pager_release_page_handle(&handle)) {
            return stats_fail(message,
                              message_size,
                              "unable to release table-stat pin for page %u",
                              page_num);
        }
    }

    *stats = local;
    if (message != NULL && message_size > 0u) {
        snprintf(message,
                 message_size,
                 "ok: total=%u leaf=%u internal=%u free=%u rows=%u",
                 local.total_pages,
                 local.leaf_pages,
                 local.internal_pages,
                 local.free_pages,
                 local.total_rows);
    }
    return true;
}

/*
 * Keep the historical db_integrity_check(Table*) ABI while routing the
 * implementation through the same fail-closed, try-pin-based whole-database
 * diagnostic used by SQL PRAGMA integrity_check.
 *
 * table.c still compiles its original implementation under the private
 * db_integrity_check_legacy_base name. This linked compatibility entry point
 * avoids process-fatal get_page() misses when the buffer pool is fully pinned
 * and strengthens the old API to validate every catalog root plus ownership.
 */
bool db_integrity_check(Table* table) {
    TinyDBPageOwnershipStats ownership;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];

    bool ok = tinydb_check_database(table,
                                    &ownership,
                                    message,
                                    sizeof(message));
    if (ok) {
        printf("ok\n");
        return true;
    }

    printf("Error: %s.\n",
           message[0] != '\0'
               ? message
               : "database integrity check failed");
    return false;
}
