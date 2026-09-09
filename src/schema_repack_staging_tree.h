#ifndef SCHEMA_REPACK_STAGING_TREE_H
#define SCHEMA_REPACK_STAGING_TREE_H

#include "compact_v2_staging_hierarchy.h"
#include "schema_repack_staging.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Final pure-memory publication boundary for schema-repack staging.
 *
 * Rows are first streamed through TinyDBSchemaRepackStaging into a private
 * compact-V2 leaf chain.  This layer finishes the row-set validation and then
 * turns that chain into a complete unpublished B+tree: a single leaf is made
 * the private root directly, while two or more leaves are connected through
 * TinyDBCompactV2StagingHierarchy.
 *
 * No Pager page, WAL record, migration manifest, catalog root, or schema
 * generation is changed here.  The result is published to the caller only
 * after the full private topology validates, so durable migration code can
 * treat result.ready as the boundary between a discardable prefix and a
 * complete candidate tree.
 */
typedef struct {
    uint32_t root_page_num;
    uint32_t leaf_page_count;
    uint32_t internal_page_count;
    uint32_t level_count;
    uint64_t row_count;
    bool ready;
} TinyDBSchemaRepackStagingTreeResult;

static inline void tinydb_schema_repack_staging_tree_set_message(
    char* message,
    size_t message_size,
    const char* detail) {
    if (message == NULL || message_size == 0u) return;
    if (detail == NULL) detail = "schema repack staging tree build failed";
    (void)snprintf(message, message_size, "%s", detail);
}

static inline bool tinydb_schema_repack_staging_tree_validate_result(
    const TinyDBSchemaRepackStaging* staging,
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    const TinyDBSchemaRepackStagingTreeResult* result) {
    if (staging == NULL || staging->chain == NULL || result == NULL ||
        !result->ready || result->root_page_num == 0u ||
        result->leaf_page_count != staging->chain->page_count ||
        result->row_count != staging->chain->row_count ||
        result->row_count != staging->rows_staged ||
        !tinydb_compact_v2_staging_leaf_chain_validate(staging->chain)) {
        return false;
    }

    if (result->leaf_page_count == 1u) {
        const unsigned char* root =
            tinydb_compact_v2_staging_page_const(staging->chain, 0u);
        return hierarchy == NULL && result->internal_page_count == 0u &&
               result->level_count == 0u &&
               result->root_page_num == staging->chain->page_numbers[0] &&
               root != NULL && get_node_type((void*)root) == NODE_LEAF &&
               is_node_root((void*)root) && *node_parent((void*)root) == 0u;
    }

    return hierarchy != NULL && hierarchy->built &&
           result->internal_page_count == hierarchy->internal_count &&
           result->level_count == hierarchy->level_count &&
           result->root_page_num == hierarchy->root_page_num &&
           tinydb_compact_v2_staging_hierarchy_validate(hierarchy);
}

static inline bool tinydb_schema_repack_staging_build_tree(
    TinyDBSchemaRepackStaging* staging,
    uint64_t expected_rows,
    TinyDBCompactV2StagingHierarchy* hierarchy,
    unsigned char* internal_images,
    const uint32_t* internal_page_numbers,
    uint32_t internal_capacity,
    TinyDBSchemaRepackStagingTreeResult* result,
    char* message,
    size_t message_size) {
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (hierarchy != NULL) memset(hierarchy, 0, sizeof(*hierarchy));
    if (message != NULL && message_size > 0u) message[0] = '\0';

    if (staging == NULL || staging->chain == NULL || result == NULL) {
        tinydb_schema_repack_staging_tree_set_message(
            message, message_size, "schema repack staging tree arguments are invalid");
        return false;
    }

    char finish_message[192];
    if (!tinydb_schema_repack_staging_finish(staging,
                                             expected_rows,
                                             finish_message,
                                             sizeof(finish_message))) {
        tinydb_schema_repack_staging_tree_set_message(
            message, message_size, finish_message);
        return false;
    }

    TinyDBCompactV2StagingLeafChain* leaves = staging->chain;
    if (leaves->page_count == 1u) {
        unsigned char* root = tinydb_compact_v2_staging_page(leaves, 0u);
        if (root == NULL || leaves->page_numbers[0] == 0u) {
            tinydb_schema_repack_staging_tree_set_message(
                message, message_size, "single-leaf repack root is invalid");
            return false;
        }
        set_node_root(root, true);
        *node_parent(root) = 0u;

        result->root_page_num = leaves->page_numbers[0];
        result->leaf_page_count = 1u;
        result->internal_page_count = 0u;
        result->level_count = 0u;
        result->row_count = leaves->row_count;
        result->ready = true;
        if (!tinydb_schema_repack_staging_tree_validate_result(
                staging, NULL, result)) {
            memset(result, 0, sizeof(*result));
            tinydb_schema_repack_staging_tree_set_message(
                message, message_size, "single-leaf repack tree failed validation");
            return false;
        }
        return true;
    }

    if (hierarchy == NULL || internal_images == NULL ||
        internal_page_numbers == NULL || internal_capacity == 0u ||
        !tinydb_compact_v2_staging_hierarchy_build(hierarchy,
                                                   leaves,
                                                   internal_images,
                                                   internal_page_numbers,
                                                   internal_capacity)) {
        tinydb_schema_repack_staging_tree_set_message(
            message, message_size, "multi-leaf repack hierarchy could not be built");
        return false;
    }

    result->root_page_num = hierarchy->root_page_num;
    result->leaf_page_count = leaves->page_count;
    result->internal_page_count = hierarchy->internal_count;
    result->level_count = hierarchy->level_count;
    result->row_count = leaves->row_count;
    result->ready = true;
    if (!tinydb_schema_repack_staging_tree_validate_result(
            staging, hierarchy, result)) {
        memset(result, 0, sizeof(*result));
        tinydb_schema_repack_staging_tree_set_message(
            message, message_size, "multi-leaf repack tree failed validation");
        return false;
    }
    return true;
}

#endif /* SCHEMA_REPACK_STAGING_TREE_H */
