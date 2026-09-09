#include "diagnostics.h"
#include "leaf_page_access.h"
#include "pager_try_pin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message == NULL || message_size == 0) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

static bool page_is_free(Pager* pager, uint32_t page_num) {
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

typedef struct {
    Table* table;
    uint32_t root_page_num;
    bool* visited;
    PagerTryPinStatus pin_status;
} DatabaseTreeWalk;

static void record_pin_status(DatabaseTreeWalk* walk,
                              PagerTryPinStatus status) {
    if (walk->pin_status == PAGER_TRY_PIN_OK &&
        status != PAGER_TRY_PIN_OK) {
        walk->pin_status = status;
    }
}

static bool release_diagnostic_handle(DatabaseTreeWalk* walk,
                                      PagerPageHandle* handle) {
    if (pager_release_page_handle(handle)) return true;
    record_pin_status(walk, PAGER_TRY_PIN_BUSY);
    return false;
}

static bool reciprocal_leaf_link_ok(DatabaseTreeWalk* walk,
                                    uint32_t page_num,
                                    uint32_t sibling_page_num,
                                    bool sibling_is_previous) {
    if (sibling_page_num == 0u) return true;
    Pager* pager = walk->table->pager;
    if (sibling_page_num >= pager->num_pages ||
        page_is_free(pager, sibling_page_num)) {
        return false;
    }

    PagerPageHandle sibling_handle;
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(
        pager, sibling_page_num, &sibling_handle);
    if (status != PAGER_TRY_PIN_OK) {
        record_pin_status(walk, status);
        return false;
    }

    void* sibling = sibling_handle.data;
    bool ok = get_node_type(sibling) == NODE_LEAF;
    uint32_t reciprocal = 0u;
    if (ok) {
        ok = sibling_is_previous
            ? tinydb_leaf_page_next(sibling, PAGE_SIZE, &reciprocal)
            : tinydb_leaf_page_prev(sibling, PAGE_SIZE, &reciprocal);
        ok = ok && reciprocal == page_num;
    }

    if (!release_diagnostic_handle(walk, &sibling_handle)) return false;
    return ok;
}

static bool walk_tree_dual(DatabaseTreeWalk* walk,
                           uint32_t page_num,
                           uint32_t expected_parent) {
    Pager* pager = walk->table->pager;
    if (page_num >= pager->num_pages || page_is_free(pager, page_num) ||
        walk->visited[page_num]) {
        return false;
    }

    PagerPageHandle node_handle;
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(
        pager, page_num, &node_handle);
    if (status != PAGER_TRY_PIN_OK) {
        record_pin_status(walk, status);
        return false;
    }
    walk->visited[page_num] = true;

    void* node = node_handle.data;
    bool root_page = page_num == walk->root_page_num;
    if (root_page) {
        if (!is_node_root(node)) {
            (void)release_diagnostic_handle(walk, &node_handle);
            return false;
        }
    } else if (is_node_root(node) || *node_parent(node) != expected_parent) {
        (void)release_diagnostic_handle(walk, &node_handle);
        return false;
    }

    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        uint32_t count = 0u;
        if (!tinydb_leaf_page_count(node, PAGE_SIZE, &count)) {
            (void)release_diagnostic_handle(walk, &node_handle);
            return false;
        }
        uint32_t prior_key = 0u;
        for (uint32_t i = 0u; i < count; i++) {
            uint32_t key = 0u;
            if (!tinydb_leaf_page_key_at(node, PAGE_SIZE, i, &key) ||
                (i > 0u && prior_key >= key)) {
                (void)release_diagnostic_handle(walk, &node_handle);
                return false;
            }
            prior_key = key;
        }

        uint32_t previous_leaf = 0u;
        uint32_t next_leaf = 0u;
        if (!tinydb_leaf_page_prev(node, PAGE_SIZE, &previous_leaf) ||
            !tinydb_leaf_page_next(node, PAGE_SIZE, &next_leaf)) {
            (void)release_diagnostic_handle(walk, &node_handle);
            return false;
        }
        if (!release_diagnostic_handle(walk, &node_handle)) return false;

        return reciprocal_leaf_link_ok(walk,
                                       page_num,
                                       previous_leaf,
                                       true) &&
               reciprocal_leaf_link_ok(walk,
                                       page_num,
                                       next_leaf,
                                       false);
    }

    if (type != NODE_INTERNAL) {
        (void)release_diagnostic_handle(walk, &node_handle);
        return false;
    }
    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys > INTERNAL_NODE_MAX_KEYS) {
        (void)release_diagnostic_handle(walk, &node_handle);
        return false;
    }
    for (uint32_t i = 1u; i < num_keys; i++) {
        if (*internal_node_key(node, i - 1u) >= *internal_node_key(node, i)) {
            (void)release_diagnostic_handle(walk, &node_handle);
            return false;
        }
    }

    uint32_t child_count = num_keys + 1u;
    uint32_t* children = (uint32_t*)malloc(sizeof(uint32_t) * child_count);
    if (children == NULL) {
        (void)release_diagnostic_handle(walk, &node_handle);
        return false;
    }
    for (uint32_t i = 0u; i < child_count; i++) {
        children[i] = *internal_node_child(node, i);
    }

    if (!release_diagnostic_handle(walk, &node_handle)) {
        free(children);
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0u; i < child_count; i++) {
        if (!walk_tree_dual(walk, children[i], page_num)) {
            ok = false;
            break;
        }
    }
    free(children);
    return ok;
}

static bool validate_tree_dual(Table* table,
                               const TableSchema* schema,
                               PagerTryPinStatus* pin_status) {
    if (pin_status != NULL) *pin_status = PAGER_TRY_PIN_OK;
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }
    bool* visited = (bool*)calloc(table->pager->num_pages, sizeof(bool));
    if (visited == NULL) return false;
    DatabaseTreeWalk walk;
    walk.table = table;
    walk.root_page_num = schema->root_page_num;
    walk.visited = visited;
    walk.pin_status = PAGER_TRY_PIN_OK;
    bool ok = walk_tree_dual(&walk,
                             schema->root_page_num,
                             schema->root_page_num);
    if (pin_status != NULL) *pin_status = walk.pin_status;
    free(visited);
    return ok;
}

bool tinydb_check_database(Table* table,
                           TinyDBPageOwnershipStats* ownership_stats,
                           char* message,
                           size_t message_size) {
    if (table == NULL || table->pager == NULL || ownership_stats == NULL) {
        set_message(message, message_size, "invalid database diagnostic input");
        return false;
    }

    memset(ownership_stats, 0, sizeof(*ownership_stats));
    if (message != NULL && message_size > 0) message[0] = '\0';

    if (table->catalog.num_tables == 0) {
        set_message(message, message_size, "catalog contains no table roots");
        return false;
    }

    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        const TableSchema* schema = &table->catalog.schemas[i];
        for (uint32_t j = 0; j < i; j++) {
            const TableSchema* previous = &table->catalog.schemas[j];
            if (previous->root_page_num == schema->root_page_num) {
                if (message != NULL && message_size > 0) {
                    snprintf(message,
                             message_size,
                             "tables '%s' and '%s' share root page %u",
                             previous->name,
                             schema->name,
                             schema->root_page_num);
                }
                return false;
            }
        }

        PagerTryPinStatus pin_status = PAGER_TRY_PIN_OK;
        if (!validate_tree_dual(table, schema, &pin_status)) {
            if (message != NULL && message_size > 0) {
                if (pin_status != PAGER_TRY_PIN_OK) {
                    snprintf(message,
                             message_size,
                             "table '%s' B+ tree validation could not acquire page: %s",
                             schema->name,
                             pager_try_pin_status_string(pin_status));
                } else {
                    snprintf(message,
                             message_size,
                             "table '%s' B+ tree structural validation failed",
                             schema->name);
                }
            }
            return false;
        }
    }

    char ownership_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_page_ownership(table,
                                     ownership_stats,
                                     ownership_message,
                                     sizeof(ownership_message))) {
        if (message != NULL && message_size > 0) {
            snprintf(message,
                     message_size,
                     "page ownership: %s",
                     ownership_message);
        }
        return false;
    }

    if (message != NULL && message_size > 0) {
        snprintf(message,
                 message_size,
                 "ok: tables=%u total=%u owned=%u free=%u orphan=%u shared=%u",
                 table->catalog.num_tables,
                 ownership_stats->total_pages,
                 ownership_stats->owned_pages,
                 ownership_stats->free_pages,
                 ownership_stats->orphan_pages,
                 ownership_stats->shared_pages);
    }
    return true;
}

bool tinydb_get_catalog_tree_stats_snapshot(
    Table* table,
    TinyDBCatalogTreeStatsSnapshot* snapshot,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
    if (table == NULL || snapshot == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "invalid catalog tree-stat arguments");
        }
        return false;
    }

    TinyDBCatalogTreeStatsSnapshot collected;
    memset(&collected, 0, sizeof(collected));
    collected.aggregate.table_count = table->catalog.num_tables;

    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        const char* table_name = table->catalog.schemas[i].name;
        TinyDBTreeStats* tree_stats = &collected.table_stats[i];
        char tree_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
        if (!tinydb_get_tree_stats_diagnostic(table,
                                               table_name,
                                               tree_stats,
                                               tree_message,
                                               sizeof(tree_message))) {
            if (message != NULL && message_size > 0u) {
                snprintf(message,
                         message_size,
                         "table '%s': %s",
                         table_name,
                         tree_message[0] != '\0'
                             ? tree_message
                             : "tree statistics unavailable");
            }
            return false;
        }
        if (UINT32_MAX - collected.aggregate.total_rows < tree_stats->total_rows ||
            UINT32_MAX - collected.aggregate.leaf_pages < tree_stats->leaf_pages ||
            UINT32_MAX - collected.aggregate.internal_pages < tree_stats->internal_pages) {
            if (message != NULL && message_size > 0u) {
                snprintf(message,
                         message_size,
                         "catalog tree statistics overflow at table '%s'",
                         table_name);
            }
            return false;
        }
        collected.aggregate.total_rows += tree_stats->total_rows;
        collected.aggregate.leaf_pages += tree_stats->leaf_pages;
        collected.aggregate.internal_pages += tree_stats->internal_pages;
        if (tree_stats->height > collected.aggregate.max_height) {
            collected.aggregate.max_height = tree_stats->height;
        }
    }

    *snapshot = collected;
    if (message != NULL && message_size > 0u) {
        snprintf(message,
                 message_size,
                 "ok: tables=%u rows=%u leaf_pages=%u internal_pages=%u max_height=%u",
                 collected.aggregate.table_count,
                 collected.aggregate.total_rows,
                 collected.aggregate.leaf_pages,
                 collected.aggregate.internal_pages,
                 collected.aggregate.max_height);
    }
    return true;
}

bool tinydb_get_database_tree_stats(
    Table* table,
    TinyDBDatabaseTreeStats* stats,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (stats != NULL) memset(stats, 0, sizeof(*stats));
    if (table == NULL || stats == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "invalid database tree-stat arguments");
        }
        return false;
    }

    TinyDBCatalogTreeStatsSnapshot snapshot;
    if (!tinydb_get_catalog_tree_stats_snapshot(table,
                                                &snapshot,
                                                message,
                                                message_size)) {
        return false;
    }
    *stats = snapshot.aggregate;
    return true;
}

bool tinydb_check_catalog_trees(
    Table* table,
    TinyDBCatalogTreeCheck* result,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (table == NULL || result == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "invalid catalog tree-check arguments");
        }
        return false;
    }
    if (table->catalog.num_tables == 0u) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "catalog contains no table roots");
        }
        return false;
    }

    TinyDBCatalogTreeStatsSnapshot snapshot;
    if (!tinydb_get_catalog_tree_stats_snapshot(table,
                                                &snapshot,
                                                message,
                                                message_size)) {
        return false;
    }

    TinyDBCatalogTreeCheck checked;
    memset(&checked, 0, sizeof(checked));
    checked.table_count = snapshot.aggregate.table_count;
    memcpy(checked.table_stats,
           snapshot.table_stats,
           sizeof(checked.table_stats));

    char ownership_message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_page_ownership(table,
                                     &checked.ownership,
                                     ownership_message,
                                     sizeof(ownership_message))) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "page ownership: %s",
                     ownership_message[0] != '\0'
                         ? ownership_message
                         : "ownership validation failed");
        }
        return false;
    }

    *result = checked;
    if (message != NULL && message_size > 0u) {
        snprintf(message,
                 message_size,
                 "ok: tables=%u owned=%u free=%u",
                 checked.table_count,
                 checked.ownership.owned_pages,
                 checked.ownership.free_pages);
    }
    return true;
}
