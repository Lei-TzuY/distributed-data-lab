#ifndef TINYDB_INTERNAL_ROOT_MERGE_STAGE_H
#define TINYDB_INTERNAL_ROOT_MERGE_STAGE_H

#include "leaf_format.h"
#include "leaf_page_access.h"
#include "leaf_sibling_relink_stage.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Stage the first bounded internal-merge case needed by V2 DELETE.
 *
 * Shape before deletion:
 *
 *              stable root (one key)
 *                 /             \
 *        left internal       right internal
 *          /      \             /      \
 *        leaf    leaf           leaf    leaf
 *
 * Both non-root internal children have exactly two leaf children, so neither
 * side can donate a child without underflowing. The removed page is a singleton
 * slotted-V2 leaf at the boundary between the two internal subtrees: either the
 * right child of the left parent or the left child of the right parent.
 *
 * Rather than constructing an unnecessary intermediate internal page, the
 * staged result contracts the tree by one level: the three surviving leaves
 * become direct children of the stable root. The two leaves adjacent to the
 * removed page are relinked and all three surviving leaves are reparented to the
 * root page number. The old left/right internal images and the removed leaf are
 * source-only and remain byte-for-byte unchanged; a future Pager/WAL route may
 * reclaim those three pages only after publishing the staged root/leaf images.
 *
 * Every mutation is performed in scratch images first. On failure, root_page
 * and all surviving_leaf_pages are untouched. Only PAGE_USABLE_SIZE is copied
 * on success so Pager-owned checksum trailer bytes remain unchanged.
 */

static inline uint32_t tinydb_internal_root_merge_read_u32(
    const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static inline void tinydb_internal_root_merge_write_u32(
    unsigned char* p,
    uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static inline bool tinydb_internal_root_merge_leaf_info(
    const unsigned char* page,
    size_t page_capacity,
    uint32_t expected_parent_page_num,
    uint32_t* min_key_out,
    uint32_t* max_key_out,
    uint32_t* prev_out,
    uint32_t* next_out) {
    if (page == NULL || page_capacity < PAGE_SIZE ||
        min_key_out == NULL || max_key_out == NULL || prev_out == NULL ||
        next_out == NULL || page[NODE_TYPE_OFFSET] != (unsigned char)NODE_LEAF ||
        page[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_root_merge_read_u32(page + PARENT_POINTER_OFFSET) !=
            expected_parent_page_num) {
        return false;
    }

    TinyDBLeafPageFormat format =
        tinydb_leaf_format_detect_page(page, page_capacity);
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        if (!tinydb_slotted_leaf_v2_validate(page, page_capacity)) return false;
    } else if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        if (!tinydb_leaf_page_is_fixed_v1(page, page_capacity)) return false;
    } else {
        return false;
    }

    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(page, page_capacity, &count) || count == 0u ||
        !tinydb_leaf_page_key_at(page, page_capacity, 0u, min_key_out) ||
        !tinydb_leaf_page_key_at(page,
                                 page_capacity,
                                 count - 1u,
                                 max_key_out) ||
        *min_key_out > *max_key_out ||
        !tinydb_leaf_page_prev(page, page_capacity, prev_out) ||
        !tinydb_leaf_page_next(page, page_capacity, next_out) ||
        *prev_out == INVALID_PAGE_NUM || *next_out == INVALID_PAGE_NUM) {
        return false;
    }
    return true;
}

static inline bool tinydb_stage_internal_root_merge_after_v2_leaf_removal(
    void* root_page,
    size_t root_capacity,
    uint32_t root_page_num,
    const void* left_internal_page,
    size_t left_internal_capacity,
    uint32_t left_internal_page_num,
    const void* right_internal_page,
    size_t right_internal_capacity,
    uint32_t right_internal_page_num,
    const void* removed_leaf_page,
    size_t removed_leaf_capacity,
    uint32_t removed_leaf_page_num,
    uint32_t removed_key,
    void* const surviving_leaf_pages[3],
    const uint32_t surviving_leaf_page_nums[3]) {
    if (root_page == NULL || left_internal_page == NULL ||
        right_internal_page == NULL || removed_leaf_page == NULL ||
        surviving_leaf_pages == NULL || surviving_leaf_page_nums == NULL ||
        root_capacity < PAGE_SIZE || left_internal_capacity < PAGE_SIZE ||
        right_internal_capacity < PAGE_SIZE || removed_leaf_capacity < PAGE_SIZE ||
        root_page_num == INVALID_PAGE_NUM ||
        left_internal_page_num == 0u ||
        left_internal_page_num == INVALID_PAGE_NUM ||
        right_internal_page_num == 0u ||
        right_internal_page_num == INVALID_PAGE_NUM ||
        left_internal_page_num == right_internal_page_num ||
        left_internal_page_num == root_page_num ||
        right_internal_page_num == root_page_num ||
        removed_leaf_page_num == 0u ||
        removed_leaf_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num == root_page_num ||
        removed_leaf_page_num == left_internal_page_num ||
        removed_leaf_page_num == right_internal_page_num) {
        return false;
    }

    const unsigned char* root = (const unsigned char*)root_page;
    const unsigned char* left = (const unsigned char*)left_internal_page;
    const unsigned char* right = (const unsigned char*)right_internal_page;
    const unsigned char* removed = (const unsigned char*)removed_leaf_page;

    if (!tinydb_parent_stage_validate(root, root_capacity) ||
        root[IS_ROOT_OFFSET] == 0u ||
        tinydb_internal_root_merge_read_u32(root + PARENT_POINTER_OFFSET) != 0u ||
        tinydb_parent_stage_read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(root, 0u) != left_internal_page_num ||
        tinydb_parent_stage_child_at(root, 1u) != right_internal_page_num ||
        !tinydb_parent_stage_validate(left, left_internal_capacity) ||
        left[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_root_merge_read_u32(left + PARENT_POINTER_OFFSET) !=
            root_page_num ||
        tinydb_parent_stage_read_u32(left + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_parent_stage_validate(right, right_internal_capacity) ||
        right[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_root_merge_read_u32(right + PARENT_POINTER_OFFSET) !=
            root_page_num ||
        tinydb_parent_stage_read_u32(right + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return false;
    }

    uint32_t left0 = tinydb_parent_stage_child_at(left, 0u);
    uint32_t left1 = tinydb_parent_stage_child_at(left, 1u);
    uint32_t right0 = tinydb_parent_stage_child_at(right, 0u);
    uint32_t right1 = tinydb_parent_stage_child_at(right, 1u);
    if (left0 == 0u || left1 == 0u || right0 == 0u || right1 == 0u ||
        left0 == INVALID_PAGE_NUM || left1 == INVALID_PAGE_NUM ||
        right0 == INVALID_PAGE_NUM || right1 == INVALID_PAGE_NUM ||
        left0 == left1 || left0 == right0 || left0 == right1 ||
        left1 == right0 || left1 == right1 || right0 == right1) {
        return false;
    }

    bool removed_from_left = left1 == removed_leaf_page_num;
    bool removed_from_right = right0 == removed_leaf_page_num;
    if (removed_from_left == removed_from_right ||
        left0 == removed_leaf_page_num || right1 == removed_leaf_page_num) {
        return false;
    }

    uint32_t expected_survivors[3];
    uint32_t expected_parents[3];
    if (removed_from_left) {
        expected_survivors[0] = left0;
        expected_survivors[1] = right0;
        expected_survivors[2] = right1;
        expected_parents[0] = left_internal_page_num;
        expected_parents[1] = right_internal_page_num;
        expected_parents[2] = right_internal_page_num;
    } else {
        expected_survivors[0] = left0;
        expected_survivors[1] = left1;
        expected_survivors[2] = right1;
        expected_parents[0] = left_internal_page_num;
        expected_parents[1] = left_internal_page_num;
        expected_parents[2] = right_internal_page_num;
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        if (surviving_leaf_pages[i] == NULL ||
            surviving_leaf_page_nums[i] != expected_survivors[i] ||
            surviving_leaf_page_nums[i] == root_page_num ||
            surviving_leaf_page_nums[i] == left_internal_page_num ||
            surviving_leaf_page_nums[i] == right_internal_page_num ||
            surviving_leaf_page_nums[i] == removed_leaf_page_num) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (surviving_leaf_pages[i] == surviving_leaf_pages[j] ||
                surviving_leaf_page_nums[i] == surviving_leaf_page_nums[j]) {
                return false;
            }
        }
    }

    uint32_t min_key[3];
    uint32_t max_key[3];
    uint32_t prev[3];
    uint32_t next[3];
    for (uint32_t i = 0u; i < 3u; i++) {
        if (!tinydb_internal_root_merge_leaf_info(
                (const unsigned char*)surviving_leaf_pages[i],
                PAGE_SIZE,
                expected_parents[i],
                &min_key[i],
                &max_key[i],
                &prev[i],
                &next[i])) {
            return false;
        }
    }
    if (max_key[0] >= min_key[1] || max_key[1] >= min_key[2]) return false;

    uint32_t removed_min = 0u;
    uint32_t removed_max = 0u;
    uint32_t removed_prev = INVALID_PAGE_NUM;
    uint32_t removed_next = INVALID_PAGE_NUM;
    uint32_t removed_parent =
        removed_from_left ? left_internal_page_num : right_internal_page_num;
    if (tinydb_leaf_format_detect_page(removed, removed_leaf_capacity) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed, removed_leaf_capacity) ||
        !tinydb_internal_root_merge_leaf_info(removed,
                                              removed_leaf_capacity,
                                              removed_parent,
                                              &removed_min,
                                              &removed_max,
                                              &removed_prev,
                                              &removed_next) ||
        removed_min != removed_key || removed_max != removed_key) {
        return false;
    }

    uint32_t left_separator = tinydb_parent_stage_key_at(left, 0u);
    uint32_t right_separator = tinydb_parent_stage_key_at(right, 0u);
    uint32_t root_separator = tinydb_parent_stage_key_at(root, 0u);
    if (left_separator != max_key[0]) return false;

    if (removed_from_left) {
        if (right_separator != max_key[1] || root_separator != removed_key ||
            prev[0] != 0u || next[0] != removed_leaf_page_num ||
            removed_prev != surviving_leaf_page_nums[0] ||
            removed_next != surviving_leaf_page_nums[1] ||
            prev[1] != removed_leaf_page_num ||
            next[1] != surviving_leaf_page_nums[2] ||
            prev[2] != surviving_leaf_page_nums[1] || next[2] != 0u ||
            max_key[0] >= removed_key || removed_key >= min_key[1]) {
            return false;
        }
    } else {
        if (right_separator != removed_key || root_separator != max_key[1] ||
            prev[0] != 0u || next[0] != surviving_leaf_page_nums[1] ||
            prev[1] != surviving_leaf_page_nums[0] ||
            next[1] != removed_leaf_page_num ||
            removed_prev != surviving_leaf_page_nums[1] ||
            removed_next != surviving_leaf_page_nums[2] ||
            prev[2] != removed_leaf_page_num || next[2] != 0u ||
            max_key[1] >= removed_key || removed_key >= min_key[2]) {
            return false;
        }
    }

    unsigned char root_scratch[PAGE_SIZE];
    unsigned char leaf_scratch[3][PAGE_SIZE];
    memcpy(root_scratch, root, PAGE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(leaf_scratch[i], surviving_leaf_pages[i], PAGE_SIZE);
    }

    if (removed_from_left) {
        if (!tinydb_stage_leaf_sibling_relink(leaf_scratch[0],
                                              PAGE_SIZE,
                                              true,
                                              removed_leaf_page_num,
                                              surviving_leaf_page_nums[1]) ||
            !tinydb_stage_leaf_sibling_relink(leaf_scratch[1],
                                              PAGE_SIZE,
                                              false,
                                              removed_leaf_page_num,
                                              surviving_leaf_page_nums[0])) {
            return false;
        }
    } else {
        if (!tinydb_stage_leaf_sibling_relink(leaf_scratch[1],
                                              PAGE_SIZE,
                                              true,
                                              removed_leaf_page_num,
                                              surviving_leaf_page_nums[2]) ||
            !tinydb_stage_leaf_sibling_relink(leaf_scratch[2],
                                              PAGE_SIZE,
                                              false,
                                              removed_leaf_page_num,
                                              surviving_leaf_page_nums[1])) {
            return false;
        }
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        tinydb_internal_root_merge_write_u32(
            leaf_scratch[i] + PARENT_POINTER_OFFSET,
            root_page_num);
    }

    memset(root_scratch, 0, PAGE_USABLE_SIZE);
    root_scratch[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root_scratch[IS_ROOT_OFFSET] = 1u;
    tinydb_internal_root_merge_write_u32(root_scratch + PARENT_POINTER_OFFSET,
                                         0u);
    tinydb_internal_root_merge_write_u32(
        root_scratch + INTERNAL_NODE_NUM_KEYS_OFFSET,
        2u);
    for (uint32_t i = 0u; i < 2u; i++) {
        unsigned char* cell =
            root_scratch + INTERNAL_NODE_HEADER_SIZE +
            (size_t)i * INTERNAL_NODE_CELL_SIZE;
        tinydb_internal_root_merge_write_u32(cell,
                                             surviving_leaf_page_nums[i]);
        tinydb_internal_root_merge_write_u32(cell + INTERNAL_NODE_CHILD_SIZE,
                                             max_key[i]);
    }
    tinydb_internal_root_merge_write_u32(
        root_scratch + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
        surviving_leaf_page_nums[2]);

    if (!tinydb_parent_stage_validate(root_scratch, PAGE_SIZE) ||
        root_scratch[IS_ROOT_OFFSET] == 0u ||
        tinydb_internal_root_merge_read_u32(
            root_scratch + PARENT_POINTER_OFFSET) != 0u) {
        return false;
    }

    uint32_t checked_min[3];
    uint32_t checked_max[3];
    uint32_t checked_prev[3];
    uint32_t checked_next[3];
    for (uint32_t i = 0u; i < 3u; i++) {
        if (!tinydb_internal_root_merge_leaf_info(leaf_scratch[i],
                                                  PAGE_SIZE,
                                                  root_page_num,
                                                  &checked_min[i],
                                                  &checked_max[i],
                                                  &checked_prev[i],
                                                  &checked_next[i]) ||
            checked_min[i] != min_key[i] || checked_max[i] != max_key[i]) {
            return false;
        }
    }
    if (checked_prev[0] != 0u ||
        checked_next[0] != surviving_leaf_page_nums[1] ||
        checked_prev[1] != surviving_leaf_page_nums[0] ||
        checked_next[1] != surviving_leaf_page_nums[2] ||
        checked_prev[2] != surviving_leaf_page_nums[1] ||
        checked_next[2] != 0u) {
        return false;
    }

    memcpy(root_page, root_scratch, PAGE_USABLE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(surviving_leaf_pages[i], leaf_scratch[i], PAGE_USABLE_SIZE);
    }
    return true;
}

#endif /* TINYDB_INTERNAL_ROOT_MERGE_STAGE_H */
