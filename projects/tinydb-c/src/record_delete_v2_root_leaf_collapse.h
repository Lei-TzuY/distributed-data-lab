#ifndef TINYDB_RECORD_DELETE_V2_ROOT_LEAF_COLLAPSE_H
#define TINYDB_RECORD_DELETE_V2_ROOT_LEAF_COLLAPSE_H

#include "generic_index_epoch.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "leaf_sibling_relink_stage.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_v2_publish_batch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    TINYDB_ROOT_LEAF_COLLAPSE_NOT_APPLICABLE = 0,
    TINYDB_ROOT_LEAF_COLLAPSE_SUCCESS,
    TINYDB_ROOT_LEAF_COLLAPSE_FAILURE
} TinyDBRootLeafCollapseResult;

static inline void tinydb_root_leaf_collapse_set_message(
    char* message,
    size_t message_size,
    const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static inline uint32_t tinydb_root_leaf_collapse_read_u32(
    const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static inline void tinydb_root_leaf_collapse_write_u32(
    unsigned char* p,
    uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static inline bool tinydb_root_leaf_collapse_page_is_free(
    const Pager* pager,
    uint32_t page_num) {
    if (pager == NULL) return true;
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

/*
 * Contract a height-two tree whose root has exactly two leaf children after
 * DELETE empties one of those children. The schema's root page number is kept
 * stable: the surviving leaf image is copied into the existing root page,
 * marked root/parentless, and detached from the old leaf chain. Both old child
 * pages are then returned to the Pager free list.
 *
 * The helper is deliberately narrow. Internal-child root contraction remains
 * outside this path because it also requires rewriting every surviving
 * grandchild's parent pointer. Non-root parent underflow likewise remains a
 * separate rebalance problem.
 */
static inline TinyDBRootLeafCollapseResult
 tinydb_try_delete_v2_root_leaf_collapse(
    Table* table,
    const TableSchema* schema,
    uint32_t removed_leaf_page_num,
    const unsigned char removed_leaf_before[PAGE_SIZE],
    uint32_t removed_key,
    char* message,
    size_t message_size) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        removed_leaf_before == NULL ||
        removed_leaf_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num >= table->pager->num_pages ||
        removed_leaf_page_num == schema->root_page_num) {
        return TINYDB_ROOT_LEAF_COLLAPSE_NOT_APPLICABLE;
    }

    if (tinydb_leaf_format_detect_page(removed_leaf_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed_leaf_before, PAGE_SIZE)) {
        return TINYDB_ROOT_LEAF_COLLAPSE_NOT_APPLICABLE;
    }

    uint32_t removed_count = 0u;
    uint32_t only_key = 0u;
    if (!tinydb_leaf_page_count(removed_leaf_before,
                                PAGE_SIZE,
                                &removed_count) ||
        removed_count != 1u ||
        !tinydb_leaf_page_key_at(removed_leaf_before,
                                 PAGE_SIZE,
                                 0u,
                                 &only_key) ||
        only_key != removed_key) {
        return TINYDB_ROOT_LEAF_COLLAPSE_NOT_APPLICABLE;
    }

    uint32_t parent_page_num = tinydb_root_leaf_collapse_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (parent_page_num != schema->root_page_num) {
        return TINYDB_ROOT_LEAF_COLLAPSE_NOT_APPLICABLE;
    }
    if (parent_page_num >= table->pager->num_pages ||
        tinydb_root_leaf_collapse_page_is_free(table->pager,
                                               parent_page_num)) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "slotted V2 root collapse found an invalid root page");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }

    unsigned char root_before[PAGE_SIZE];
    memcpy(root_before,
           get_page(table->pager, parent_page_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root_before, PAGE_SIZE) ||
        root_before[IS_ROOT_OFFSET] == 0u ||
        tinydb_root_leaf_collapse_read_u32(
            root_before + PARENT_POINTER_OFFSET) != 0u) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "slotted V2 root collapse requires a valid internal root");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }

    uint32_t root_key_count = tinydb_parent_stage_read_u32(
        root_before + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (root_key_count != 1u) {
        return TINYDB_ROOT_LEAF_COLLAPSE_NOT_APPLICABLE;
    }

    uint32_t left_child = tinydb_parent_stage_child_at(root_before, 0u);
    uint32_t right_child = tinydb_parent_stage_child_at(root_before, 1u);
    bool removed_left = left_child == removed_leaf_page_num;
    bool removed_right = right_child == removed_leaf_page_num;
    if (removed_left == removed_right) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "slotted V2 root collapse could not identify the removed child");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }

    uint32_t survivor_page_num = removed_left ? right_child : left_child;
    if (survivor_page_num == 0u ||
        survivor_page_num == INVALID_PAGE_NUM ||
        survivor_page_num >= table->pager->num_pages ||
        survivor_page_num == removed_leaf_page_num ||
        survivor_page_num == parent_page_num ||
        tinydb_root_leaf_collapse_page_is_free(table->pager,
                                               survivor_page_num)) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "slotted V2 root collapse found an invalid surviving child");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }

    unsigned char survivor_after[PAGE_SIZE];
    memcpy(survivor_after,
           get_page(table->pager, survivor_page_num),
           PAGE_SIZE);
    TinyDBLeafPageFormat survivor_format =
        tinydb_leaf_format_detect_page(survivor_after, PAGE_SIZE);
    if ((survivor_format != TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 &&
         survivor_format != TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) ||
        get_node_type(survivor_after) != NODE_LEAF) {
        /* A root with two internal children needs grandchild parent rewrites. */
        return TINYDB_ROOT_LEAF_COLLAPSE_NOT_APPLICABLE;
    }
    if (survivor_format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
        !tinydb_slotted_leaf_v2_validate(survivor_after, PAGE_SIZE)) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "slotted V2 root collapse found a corrupt surviving leaf");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }

    uint32_t survivor_count = 0u;
    uint32_t survivor_max = 0u;
    uint32_t survivor_prev = INVALID_PAGE_NUM;
    uint32_t survivor_next = INVALID_PAGE_NUM;
    if (!tinydb_leaf_page_count(survivor_after,
                                PAGE_SIZE,
                                &survivor_count) ||
        survivor_count == 0u ||
        !tinydb_leaf_page_key_at(survivor_after,
                                 PAGE_SIZE,
                                 survivor_count - 1u,
                                 &survivor_max) ||
        !tinydb_leaf_page_prev(survivor_after,
                               PAGE_SIZE,
                               &survivor_prev) ||
        !tinydb_leaf_page_next(survivor_after,
                               PAGE_SIZE,
                               &survivor_next)) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "slotted V2 root collapse could not inspect the surviving leaf");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }

    uint32_t removed_prev = INVALID_PAGE_NUM;
    uint32_t removed_next = INVALID_PAGE_NUM;
    if (!tinydb_leaf_page_prev(removed_leaf_before,
                               PAGE_SIZE,
                               &removed_prev) ||
        !tinydb_leaf_page_next(removed_leaf_before,
                               PAGE_SIZE,
                               &removed_next)) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "slotted V2 root collapse could not inspect removed-leaf links");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }

    uint32_t root_separator = tinydb_parent_stage_key_at(root_before, 0u);
    if (removed_left) {
        if (root_separator != removed_key ||
            removed_prev != 0u || removed_next != survivor_page_num ||
            survivor_prev != removed_leaf_page_num || survivor_next != 0u ||
            survivor_max <= removed_key ||
            !tinydb_stage_leaf_sibling_relink(survivor_after,
                                              PAGE_SIZE,
                                              false,
                                              removed_leaf_page_num,
                                              0u)) {
            tinydb_root_leaf_collapse_set_message(
                message,
                message_size,
                "slotted V2 root collapse found inconsistent left-boundary topology");
            return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
        }
    } else {
        if (root_separator != survivor_max ||
            removed_prev != survivor_page_num || removed_next != 0u ||
            survivor_prev != 0u || survivor_next != removed_leaf_page_num ||
            survivor_max >= removed_key ||
            !tinydb_stage_leaf_sibling_relink(survivor_after,
                                              PAGE_SIZE,
                                              true,
                                              removed_leaf_page_num,
                                              0u)) {
            tinydb_root_leaf_collapse_set_message(
                message,
                message_size,
                "slotted V2 root collapse found inconsistent right-boundary topology");
            return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
        }
    }

    survivor_after[IS_ROOT_OFFSET] = 1u;
    tinydb_root_leaf_collapse_write_u32(
        survivor_after + PARENT_POINTER_OFFSET,
        0u);

    uint32_t collapsed_prev = INVALID_PAGE_NUM;
    uint32_t collapsed_next = INVALID_PAGE_NUM;
    uint32_t collapsed_count = 0u;
    if (tinydb_leaf_format_detect_page(survivor_after, PAGE_SIZE) !=
            survivor_format ||
        get_node_type(survivor_after) != NODE_LEAF ||
        !tinydb_leaf_page_count(survivor_after,
                                PAGE_SIZE,
                                &collapsed_count) ||
        collapsed_count != survivor_count ||
        !tinydb_leaf_page_prev(survivor_after,
                               PAGE_SIZE,
                               &collapsed_prev) ||
        !tinydb_leaf_page_next(survivor_after,
                               PAGE_SIZE,
                               &collapsed_next) ||
        collapsed_prev != 0u || collapsed_next != 0u ||
        (survivor_format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
         !tinydb_slotted_leaf_v2_validate(survivor_after, PAGE_SIZE))) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "slotted V2 root collapse produced an invalid root leaf image");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }

    TinyDBV2PublishEntry entry;
    entry.page_num = parent_page_num;
    entry.target = (unsigned char*)get_page(table->pager, parent_page_num);
    entry.staged = survivor_after;
    if (!tinydb_v2_publish_batch(&entry,
                                 1u,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_root_leaf_collapse_set_message(
            message,
            message_size,
            "unable to atomically publish slotted V2 root collapse");
        return TINYDB_ROOT_LEAF_COLLAPSE_FAILURE;
    }
    mark_page_dirty(table->pager, parent_page_num);

    pager_free_page(table->pager, removed_leaf_page_num);
    pager_free_page(table->pager, survivor_page_num);

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return TINYDB_ROOT_LEAF_COLLAPSE_SUCCESS;
}

#endif
