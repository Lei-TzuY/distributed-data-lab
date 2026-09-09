#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_H

#include "generic_index_epoch.h"
#include "internal_root_collapse_stage.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "leaf_sibling_relink_stage.h"
#include "record_delete_v2_internal_borrow.h"
#include "slotted_v2_publish_batch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE = 0,
    TINYDB_INTERNAL_MERGE_ROOT_SUCCESS,
    TINYDB_INTERNAL_MERGE_ROOT_FAILURE
} TinyDBInternalMergeRootResult;

static inline void tinydb_internal_merge_root_message(char* message,
                                                       size_t message_size,
                                                       const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static inline bool tinydb_internal_merge_root_leaf_valid(
    const unsigned char page[PAGE_SIZE],
    uint32_t expected_parent,
    uint32_t* min_key,
    uint32_t* max_key,
    uint32_t* prev_page,
    uint32_t* next_page) {
    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    if ((format != TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 &&
         format != TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) ||
        page[NODE_TYPE_OFFSET] != (unsigned char)NODE_LEAF ||
        tinydb_internal_borrow_read_u32(page + PARENT_POINTER_OFFSET) !=
            expected_parent ||
        !tinydb_internal_borrow_leaf_bounds(page, min_key, max_key) ||
        !tinydb_leaf_page_prev(page, PAGE_SIZE, prev_page) ||
        !tinydb_leaf_page_next(page, PAGE_SIZE, next_page)) {
        return false;
    }
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
        !tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE)) {
        return false;
    }
    return true;
}

static inline bool tinydb_internal_merge_root_build_internal(
    unsigned char page[PAGE_SIZE],
    uint32_t root_page_num,
    const uint32_t children[3],
    const uint32_t child_maxes[3]) {
    if (page == NULL || children == NULL || child_maxes == NULL ||
        children[0] == 0u || children[1] == 0u || children[2] == 0u ||
        children[0] == INVALID_PAGE_NUM ||
        children[1] == INVALID_PAGE_NUM ||
        children[2] == INVALID_PAGE_NUM ||
        children[0] == children[1] || children[0] == children[2] ||
        children[1] == children[2] ||
        child_maxes[0] >= child_maxes[1] ||
        child_maxes[1] >= child_maxes[2]) {
        return false;
    }

    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 0u;
    tinydb_parent_stage_write_u32(page + PARENT_POINTER_OFFSET,
                                  root_page_num);
    tinydb_parent_stage_write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 2u);
    unsigned char* cell0 = page + INTERNAL_NODE_HEADER_SIZE;
    unsigned char* cell1 = cell0 + INTERNAL_NODE_CELL_SIZE;
    tinydb_parent_stage_write_u32(cell0, children[0]);
    tinydb_parent_stage_write_u32(cell0 + INTERNAL_NODE_CHILD_SIZE,
                                  child_maxes[0]);
    tinydb_parent_stage_write_u32(cell1, children[1]);
    tinydb_parent_stage_write_u32(cell1 + INTERNAL_NODE_CHILD_SIZE,
                                  child_maxes[1]);
    tinydb_parent_stage_write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
                                  children[2]);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

/*
 * Resolve the first true merge case in production V2 DELETE:
 *
 *   one-key root
 *      /      \
 *   2-child  2-child      (both internal parents are at minimum occupancy)
 *
 * A singleton V2 leaf is removed from either side. Since neither internal
 * sibling can lend a child, the three surviving leaves are merged in key order
 * into a scratch internal image. That image is then staged through the proven
 * internal->root contraction primitive so the schema root page stays stable
 * and every surviving leaf is reparented directly to it.
 *
 * No old internal page is published. Root plus the three surviving leaf images
 * form one atomic Pager-visible batch; only after publication do the removed
 * leaf and both obsolete internal parent pages enter the free list. This keeps
 * transaction rollback/WAL allocator restoration aligned with the existing
 * no-steal Pager contract.
 */
static inline TinyDBInternalMergeRootResult
 tinydb_try_delete_v2_internal_merge_root(
    Table* table,
    const TableSchema* schema,
    uint32_t removed_leaf_page_num,
    const unsigned char removed_leaf_before[PAGE_SIZE],
    uint32_t removed_key,
    char* message,
    size_t message_size) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        removed_leaf_before == NULL || removed_leaf_page_num == 0u ||
        removed_leaf_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num >= table->pager->num_pages ||
        removed_leaf_page_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, removed_leaf_page_num) ||
        tinydb_leaf_format_detect_page(removed_leaf_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed_leaf_before, PAGE_SIZE)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t removed_count = 0u;
    uint32_t removed_only_key = 0u;
    if (!tinydb_leaf_page_count(removed_leaf_before, PAGE_SIZE, &removed_count) ||
        removed_count != 1u ||
        !tinydb_leaf_page_key_at(removed_leaf_before,
                                 PAGE_SIZE,
                                 0u,
                                 &removed_only_key) ||
        removed_only_key != removed_key) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t removed_parent_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (removed_parent_num == 0u || removed_parent_num == INVALID_PAGE_NUM ||
        removed_parent_num >= table->pager->num_pages ||
        removed_parent_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, removed_parent_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char removed_parent[PAGE_SIZE];
    memcpy(removed_parent,
           get_page(table->pager, removed_parent_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(removed_parent, PAGE_SIZE) ||
        removed_parent[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            removed_parent + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t root_num = tinydb_internal_borrow_read_u32(
        removed_parent + PARENT_POINTER_OFFSET);
    if (root_num != schema->root_page_num || root_num >= table->pager->num_pages ||
        tinydb_internal_borrow_page_free(table->pager, root_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char root_after[PAGE_SIZE];
    memcpy(root_after, get_page(table->pager, root_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        root_after[IS_ROOT_OFFSET] == 0u ||
        tinydb_internal_borrow_read_u32(root_after + PARENT_POINTER_OFFSET) != 0u ||
        tinydb_parent_stage_read_u32(root_after + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t left_parent_num = tinydb_parent_stage_child_at(root_after, 0u);
    uint32_t right_parent_num = tinydb_parent_stage_child_at(root_after, 1u);
    uint32_t root_separator = tinydb_parent_stage_key_at(root_after, 0u);
    if (left_parent_num == 0u || right_parent_num == 0u ||
        left_parent_num == INVALID_PAGE_NUM ||
        right_parent_num == INVALID_PAGE_NUM ||
        left_parent_num >= table->pager->num_pages ||
        right_parent_num >= table->pager->num_pages ||
        left_parent_num == right_parent_num ||
        (removed_parent_num != left_parent_num &&
         removed_parent_num != right_parent_num) ||
        tinydb_internal_borrow_page_free(table->pager, left_parent_num) ||
        tinydb_internal_borrow_page_free(table->pager, right_parent_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char left_parent[PAGE_SIZE];
    unsigned char right_parent[PAGE_SIZE];
    memcpy(left_parent, get_page(table->pager, left_parent_num), PAGE_SIZE);
    memcpy(right_parent, get_page(table->pager, right_parent_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(left_parent, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(right_parent, PAGE_SIZE) ||
        left_parent[IS_ROOT_OFFSET] != 0u || right_parent[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(left_parent + PARENT_POINTER_OFFSET) !=
            root_num ||
        tinydb_internal_borrow_read_u32(right_parent + PARENT_POINTER_OFFSET) !=
            root_num ||
        tinydb_parent_stage_read_u32(left_parent + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        tinydb_parent_stage_read_u32(right_parent + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t child_nums[4] = {
        tinydb_parent_stage_child_at(left_parent, 0u),
        tinydb_parent_stage_child_at(left_parent, 1u),
        tinydb_parent_stage_child_at(right_parent, 0u),
        tinydb_parent_stage_child_at(right_parent, 1u),
    };
    uint32_t expected_parents[4] = {
        left_parent_num, left_parent_num, right_parent_num, right_parent_num,
    };

    uint32_t removed_index = 4u;
    for (uint32_t i = 0u; i < 4u; i++) {
        if (child_nums[i] == 0u || child_nums[i] == INVALID_PAGE_NUM ||
            child_nums[i] >= table->pager->num_pages ||
            tinydb_internal_borrow_page_free(table->pager, child_nums[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (child_nums[i] == child_nums[j]) {
                return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
            }
        }
        if (child_nums[i] == removed_leaf_page_num) {
            if (removed_index != 4u) return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
            removed_index = i;
        }
    }
    if (removed_index == 4u || expected_parents[removed_index] != removed_parent_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char original_leaves[4][PAGE_SIZE];
    uint32_t mins[4];
    uint32_t maxes[4];
    uint32_t prevs[4];
    uint32_t nexts[4];
    for (uint32_t i = 0u; i < 4u; i++) {
        if (i == removed_index) {
            memcpy(original_leaves[i], removed_leaf_before, PAGE_SIZE);
        } else {
            memcpy(original_leaves[i],
                   get_page(table->pager, child_nums[i]),
                   PAGE_SIZE);
        }
        if (!tinydb_internal_merge_root_leaf_valid(original_leaves[i],
                                                   expected_parents[i],
                                                   &mins[i],
                                                   &maxes[i],
                                                   &prevs[i],
                                                   &nexts[i])) {
            tinydb_internal_merge_root_message(
                message, message_size,
                "slotted V2 internal merge found an invalid leaf child");
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
        if (i > 0u && maxes[i - 1u] >= mins[i]) {
            tinydb_internal_merge_root_message(
                message, message_size,
                "slotted V2 internal merge found overlapping leaf ranges");
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
        uint32_t expected_prev = i == 0u ? 0u : child_nums[i - 1u];
        uint32_t expected_next = i == 3u ? 0u : child_nums[i + 1u];
        if (prevs[i] != expected_prev || nexts[i] != expected_next) {
            tinydb_internal_merge_root_message(
                message, message_size,
                "slotted V2 internal merge found an inconsistent leaf chain");
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
    }

    if (maxes[0] != tinydb_parent_stage_key_at(left_parent, 0u) ||
        maxes[1] != root_separator ||
        maxes[2] != tinydb_parent_stage_key_at(right_parent, 0u)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "slotted V2 internal merge found inconsistent parent separators");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    uint32_t survivor_nums[3];
    uint32_t survivor_maxes[3];
    unsigned char survivor_pages[3][PAGE_SIZE];
    uint32_t out = 0u;
    for (uint32_t i = 0u; i < 4u; i++) {
        if (i == removed_index) continue;
        survivor_nums[out] = child_nums[i];
        survivor_maxes[out] = maxes[i];
        memcpy(survivor_pages[out], original_leaves[i], PAGE_SIZE);
        out++;
    }
    if (out != 3u) return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;

    for (uint32_t i = 0u; i < 3u; i++) {
        uint32_t desired_prev = i == 0u ? 0u : survivor_nums[i - 1u];
        uint32_t desired_next = i == 2u ? 0u : survivor_nums[i + 1u];
        uint32_t current_prev = INVALID_PAGE_NUM;
        uint32_t current_next = INVALID_PAGE_NUM;
        if (!tinydb_leaf_page_prev(survivor_pages[i], PAGE_SIZE, &current_prev) ||
            !tinydb_leaf_page_next(survivor_pages[i], PAGE_SIZE, &current_next)) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
        if (current_prev != desired_prev) {
            if (current_prev != removed_leaf_page_num ||
                !tinydb_stage_leaf_sibling_relink(survivor_pages[i],
                                                  PAGE_SIZE,
                                                  false,
                                                  removed_leaf_page_num,
                                                  desired_prev)) {
                return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
            }
        }
        if (current_next != desired_next) {
            if (current_next != removed_leaf_page_num ||
                !tinydb_stage_leaf_sibling_relink(survivor_pages[i],
                                                  PAGE_SIZE,
                                                  true,
                                                  removed_leaf_page_num,
                                                  desired_next)) {
                return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
            }
        }
        tinydb_parent_stage_write_u32(
            survivor_pages[i] + PARENT_POINTER_OFFSET,
            right_parent_num);
    }

    unsigned char merged_internal[PAGE_SIZE];
    if (!tinydb_internal_merge_root_build_internal(merged_internal,
                                                   root_num,
                                                   survivor_nums,
                                                   survivor_maxes)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "slotted V2 internal merge could not stage the merged parent");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    void* child_pages[3] = {
        survivor_pages[0], survivor_pages[1], survivor_pages[2],
    };
    uint32_t promoted_page_num = INVALID_PAGE_NUM;
    if (!tinydb_stage_internal_root_collapse_to_internal(
            root_after,
            PAGE_SIZE,
            root_num,
            merged_internal,
            PAGE_SIZE,
            right_parent_num,
            survivor_maxes[2],
            left_parent_num,
            root_separator,
            child_pages,
            survivor_nums,
            3u,
            &promoted_page_num) ||
        promoted_page_num != right_parent_num) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "slotted V2 internal merge could not collapse the merged parent into root");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    if (!tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        root_after[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(root_after + PARENT_POINTER_OFFSET) != 0u ||
        tinydb_parent_stage_read_u32(root_after + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            2u ||
        tinydb_parent_stage_child_at(root_after, 0u) != survivor_nums[0] ||
        tinydb_parent_stage_child_at(root_after, 1u) != survivor_nums[1] ||
        tinydb_parent_stage_child_at(root_after, 2u) != survivor_nums[2] ||
        tinydb_parent_stage_key_at(root_after, 0u) != survivor_maxes[0] ||
        tinydb_parent_stage_key_at(root_after, 1u) != survivor_maxes[1]) {
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        uint32_t min_key = 0u;
        uint32_t max_key = 0u;
        uint32_t prev_page = INVALID_PAGE_NUM;
        uint32_t next_page = INVALID_PAGE_NUM;
        if (!tinydb_internal_merge_root_leaf_valid(survivor_pages[i],
                                                   root_num,
                                                   &min_key,
                                                   &max_key,
                                                   &prev_page,
                                                   &next_page) ||
            max_key != survivor_maxes[i] ||
            prev_page != (i == 0u ? 0u : survivor_nums[i - 1u]) ||
            next_page != (i == 2u ? 0u : survivor_nums[i + 1u])) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    TinyDBV2PublishEntry entries[4];
    entries[0].page_num = root_num;
    entries[0].target = (unsigned char*)get_page(table->pager, root_num);
    entries[0].staged = root_after;
    for (uint32_t i = 0u; i < 3u; i++) {
        entries[i + 1u].page_num = survivor_nums[i];
        entries[i + 1u].target =
            (unsigned char*)get_page(table->pager, survivor_nums[i]);
        entries[i + 1u].staged = survivor_pages[i];
    }
    if (!tinydb_v2_publish_batch(entries, 4u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "unable to atomically publish slotted V2 internal merge/root collapse");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }
    for (uint32_t i = 0u; i < 4u; i++) {
        mark_page_dirty(table->pager, entries[i].page_num);
    }

    pager_free_page(table->pager, removed_leaf_page_num);
    pager_free_page(table->pager, left_parent_num);
    pager_free_page(table->pager, right_parent_num);
    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return TINYDB_INTERNAL_MERGE_ROOT_SUCCESS;
}

#endif
