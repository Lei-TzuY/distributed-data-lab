#ifndef TINYDB_INTERNAL_NONROOT_MERGE_STAGE_H
#define TINYDB_INTERNAL_NONROOT_MERGE_STAGE_H

#include "internal_child_remove_stage.h"
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
 * Stage a bounded internal merge one level below a non-root ancestor.
 *
 * The target and adjacent sibling are both minimum two-child internal nodes.
 * The removed page is a singleton V2 leaf on their shared boundary:
 *
 *   target [A, removed] + right sibling [C, D]
 *      -> keep right sibling as [A, C, D], remove target from ancestor
 *
 *   left sibling [A, B] + target [removed, D]
 *      -> keep target as [A, B, D], remove left sibling from ancestor
 *
 * The ancestor must have at least three children before the merge. The obsolete
 * internal child is deliberately chosen on the near side of the kept child, so
 * it is never the ancestor's rightmost child. Therefore removing it cannot
 * change the ancestor subtree maximum and no grandparent separator rewrite is
 * required. This is the first safe building block for deeper V2 DELETE
 * rebalancing without recursively changing higher ancestors.
 *
 * All caller-visible pages are staged in scratch memory and copied back only
 * after the rebuilt ancestor, kept parent, surviving leaf chain, and parent
 * pointers validate. Source images for the removed leaf and obsolete parent are
 * immutable. Only PAGE_USABLE_SIZE is copied so Pager-owned checksum trailers
 * remain untouched.
 */

static inline uint32_t tinydb_nonroot_merge_read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static inline void tinydb_nonroot_merge_write_u32(unsigned char* p,
                                                   uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static inline bool tinydb_nonroot_merge_leaf_valid(
    const unsigned char* page,
    size_t capacity,
    uint32_t expected_parent,
    uint32_t* min_key_out,
    uint32_t* max_key_out,
    uint32_t* prev_out,
    uint32_t* next_out) {
    if (page == NULL || capacity < PAGE_SIZE || min_key_out == NULL ||
        max_key_out == NULL || prev_out == NULL || next_out == NULL ||
        page[NODE_TYPE_OFFSET] != (unsigned char)NODE_LEAF ||
        page[IS_ROOT_OFFSET] != 0u ||
        tinydb_nonroot_merge_read_u32(page + PARENT_POINTER_OFFSET) !=
            expected_parent) {
        return false;
    }

    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, capacity);
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        if (!tinydb_slotted_leaf_v2_validate(page, capacity)) return false;
    } else if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        if (!tinydb_leaf_page_is_fixed_v1(page, capacity)) return false;
    } else {
        return false;
    }

    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(page, capacity, &count) || count == 0u ||
        !tinydb_leaf_page_key_at(page, capacity, 0u, min_key_out) ||
        !tinydb_leaf_page_key_at(page,
                                 capacity,
                                 count - 1u,
                                 max_key_out) ||
        *min_key_out > *max_key_out ||
        !tinydb_leaf_page_prev(page, capacity, prev_out) ||
        !tinydb_leaf_page_next(page, capacity, next_out) ||
        *prev_out == INVALID_PAGE_NUM || *next_out == INVALID_PAGE_NUM) {
        return false;
    }
    return true;
}

static inline bool tinydb_nonroot_merge_build_three_child_internal(
    unsigned char page[PAGE_SIZE],
    uint32_t ancestor_page_num,
    const uint32_t children[3],
    const uint32_t maxes[3]) {
    if (page == NULL || children == NULL || maxes == NULL ||
        ancestor_page_num == INVALID_PAGE_NUM ||
        children[0] == 0u || children[1] == 0u || children[2] == 0u ||
        children[0] == INVALID_PAGE_NUM ||
        children[1] == INVALID_PAGE_NUM ||
        children[2] == INVALID_PAGE_NUM ||
        children[0] == children[1] || children[0] == children[2] ||
        children[1] == children[2] ||
        maxes[0] >= maxes[1] || maxes[1] >= maxes[2]) {
        return false;
    }

    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 0u;
    tinydb_nonroot_merge_write_u32(page + PARENT_POINTER_OFFSET,
                                   ancestor_page_num);
    tinydb_nonroot_merge_write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 2u);
    for (uint32_t i = 0u; i < 2u; i++) {
        unsigned char* cell =
            page + INTERNAL_NODE_HEADER_SIZE +
            (size_t)i * INTERNAL_NODE_CELL_SIZE;
        tinydb_nonroot_merge_write_u32(cell, children[i]);
        tinydb_nonroot_merge_write_u32(cell + INTERNAL_NODE_CHILD_SIZE,
                                       maxes[i]);
    }
    tinydb_nonroot_merge_write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
                                   children[2]);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static inline bool tinydb_stage_internal_nonroot_merge_after_v2_leaf_removal(
    void* ancestor_page,
    size_t ancestor_capacity,
    uint32_t ancestor_page_num,
    void* target_parent_page,
    size_t target_capacity,
    uint32_t target_parent_page_num,
    void* sibling_parent_page,
    size_t sibling_capacity,
    uint32_t sibling_parent_page_num,
    const void* removed_leaf_page,
    size_t removed_leaf_capacity,
    uint32_t removed_leaf_page_num,
    uint32_t removed_key,
    void* const surviving_leaf_pages[3],
    const uint32_t surviving_leaf_page_nums[3],
    uint32_t* kept_parent_page_num_out,
    uint32_t* obsolete_parent_page_num_out) {
    if (kept_parent_page_num_out != NULL) {
        *kept_parent_page_num_out = INVALID_PAGE_NUM;
    }
    if (obsolete_parent_page_num_out != NULL) {
        *obsolete_parent_page_num_out = INVALID_PAGE_NUM;
    }
    if (ancestor_page == NULL || target_parent_page == NULL ||
        sibling_parent_page == NULL || removed_leaf_page == NULL ||
        surviving_leaf_pages == NULL || surviving_leaf_page_nums == NULL ||
        ancestor_capacity < PAGE_SIZE || target_capacity < PAGE_SIZE ||
        sibling_capacity < PAGE_SIZE || removed_leaf_capacity < PAGE_SIZE ||
        ancestor_page_num == 0u || ancestor_page_num == INVALID_PAGE_NUM ||
        target_parent_page_num == 0u ||
        target_parent_page_num == INVALID_PAGE_NUM ||
        sibling_parent_page_num == 0u ||
        sibling_parent_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num == 0u ||
        removed_leaf_page_num == INVALID_PAGE_NUM ||
        target_parent_page_num == sibling_parent_page_num ||
        ancestor_page_num == target_parent_page_num ||
        ancestor_page_num == sibling_parent_page_num ||
        ancestor_page_num == removed_leaf_page_num ||
        target_parent_page_num == removed_leaf_page_num ||
        sibling_parent_page_num == removed_leaf_page_num) {
        return false;
    }

    const unsigned char* ancestor = (const unsigned char*)ancestor_page;
    const unsigned char* target = (const unsigned char*)target_parent_page;
    const unsigned char* sibling = (const unsigned char*)sibling_parent_page;
    const unsigned char* removed = (const unsigned char*)removed_leaf_page;
    if (!tinydb_parent_stage_validate(ancestor, ancestor_capacity) ||
        ancestor[IS_ROOT_OFFSET] != 0u ||
        !tinydb_parent_stage_validate(target, target_capacity) ||
        target[IS_ROOT_OFFSET] != 0u ||
        tinydb_nonroot_merge_read_u32(target + PARENT_POINTER_OFFSET) !=
            ancestor_page_num ||
        tinydb_parent_stage_read_u32(target + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        !tinydb_parent_stage_validate(sibling, sibling_capacity) ||
        sibling[IS_ROOT_OFFSET] != 0u ||
        tinydb_nonroot_merge_read_u32(sibling + PARENT_POINTER_OFFSET) !=
            ancestor_page_num ||
        tinydb_parent_stage_read_u32(sibling + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        tinydb_leaf_format_detect_page(removed, removed_leaf_capacity) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed, removed_leaf_capacity)) {
        return false;
    }

    uint32_t ancestor_key_count = tinydb_parent_stage_read_u32(
        ancestor + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (ancestor_key_count < 2u || ancestor_key_count > INTERNAL_NODE_MAX_KEYS) {
        return false;
    }

    uint32_t target_index = ancestor_key_count + 1u;
    uint32_t sibling_index = ancestor_key_count + 1u;
    for (uint32_t i = 0u; i <= ancestor_key_count; i++) {
        uint32_t child = tinydb_parent_stage_child_at(ancestor, i);
        if (child == target_parent_page_num) {
            if (target_index != ancestor_key_count + 1u) return false;
            target_index = i;
        }
        if (child == sibling_parent_page_num) {
            if (sibling_index != ancestor_key_count + 1u) return false;
            sibling_index = i;
        }
    }
    if (target_index > ancestor_key_count || sibling_index > ancestor_key_count) {
        return false;
    }

    uint32_t target_left = tinydb_parent_stage_child_at(target, 0u);
    uint32_t target_right = tinydb_parent_stage_child_at(target, 1u);
    bool removed_rightmost = target_right == removed_leaf_page_num;
    bool removed_leftmost = target_left == removed_leaf_page_num;
    if (removed_rightmost == removed_leftmost) return false;

    uint32_t kept_parent_num = INVALID_PAGE_NUM;
    uint32_t obsolete_parent_num = INVALID_PAGE_NUM;
    uint32_t obsolete_parent_max = 0u;
    uint32_t survivor_nums[3] = {0u, 0u, 0u};
    uint32_t expected_parents[3] = {0u, 0u, 0u};

    if (removed_rightmost) {
        if (sibling_index != target_index + 1u ||
            target_index >= ancestor_key_count ||
            tinydb_parent_stage_key_at(ancestor, target_index) != removed_key ||
            tinydb_parent_stage_key_at(target, 0u) >= removed_key) {
            return false;
        }
        kept_parent_num = sibling_parent_page_num;
        obsolete_parent_num = target_parent_page_num;
        obsolete_parent_max = removed_key;
        survivor_nums[0] = target_left;
        survivor_nums[1] = tinydb_parent_stage_child_at(sibling, 0u);
        survivor_nums[2] = tinydb_parent_stage_child_at(sibling, 1u);
        expected_parents[0] = target_parent_page_num;
        expected_parents[1] = sibling_parent_page_num;
        expected_parents[2] = sibling_parent_page_num;
    } else {
        if (target_index == 0u || sibling_index + 1u != target_index ||
            tinydb_parent_stage_key_at(target, 0u) != removed_key) {
            return false;
        }
        kept_parent_num = target_parent_page_num;
        obsolete_parent_num = sibling_parent_page_num;
        obsolete_parent_max = tinydb_parent_stage_key_at(ancestor, sibling_index);
        survivor_nums[0] = tinydb_parent_stage_child_at(sibling, 0u);
        survivor_nums[1] = tinydb_parent_stage_child_at(sibling, 1u);
        survivor_nums[2] = target_right;
        expected_parents[0] = sibling_parent_page_num;
        expected_parents[1] = sibling_parent_page_num;
        expected_parents[2] = target_parent_page_num;
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        if (surviving_leaf_pages[i] == NULL ||
            surviving_leaf_page_nums[i] != survivor_nums[i] ||
            survivor_nums[i] == 0u || survivor_nums[i] == INVALID_PAGE_NUM ||
            survivor_nums[i] == ancestor_page_num ||
            survivor_nums[i] == target_parent_page_num ||
            survivor_nums[i] == sibling_parent_page_num ||
            survivor_nums[i] == removed_leaf_page_num) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (survivor_nums[i] == survivor_nums[j] ||
                surviving_leaf_pages[i] == surviving_leaf_pages[j]) {
                return false;
            }
        }
    }

    uint32_t min_keys[3];
    uint32_t max_keys[3];
    uint32_t prevs[3];
    uint32_t nexts[3];
    for (uint32_t i = 0u; i < 3u; i++) {
        if (!tinydb_nonroot_merge_leaf_valid(
                (const unsigned char*)surviving_leaf_pages[i],
                PAGE_SIZE,
                expected_parents[i],
                &min_keys[i],
                &max_keys[i],
                &prevs[i],
                &nexts[i])) {
            return false;
        }
        if (i > 0u && max_keys[i - 1u] >= min_keys[i]) return false;
    }

    uint32_t removed_min = 0u;
    uint32_t removed_max = 0u;
    uint32_t removed_prev = INVALID_PAGE_NUM;
    uint32_t removed_next = INVALID_PAGE_NUM;
    if (!tinydb_nonroot_merge_leaf_valid(removed,
                                         removed_leaf_capacity,
                                         target_parent_page_num,
                                         &removed_min,
                                         &removed_max,
                                         &removed_prev,
                                         &removed_next) ||
        removed_min != removed_key || removed_max != removed_key) {
        return false;
    }

    if (removed_rightmost) {
        if (max_keys[0] != tinydb_parent_stage_key_at(target, 0u) ||
            max_keys[1] != tinydb_parent_stage_key_at(sibling, 0u) ||
            removed_prev != survivor_nums[0] ||
            removed_next != survivor_nums[1] ||
            nexts[0] != removed_leaf_page_num ||
            prevs[1] != removed_leaf_page_num ||
            nexts[1] != survivor_nums[2] || prevs[2] != survivor_nums[1] ||
            max_keys[0] >= removed_key || removed_key >= min_keys[1]) {
            return false;
        }
        if (sibling_index < ancestor_key_count &&
            tinydb_parent_stage_key_at(ancestor, sibling_index) != max_keys[2]) {
            return false;
        }
    } else {
        if (max_keys[0] != tinydb_parent_stage_key_at(sibling, 0u) ||
            max_keys[1] != obsolete_parent_max ||
            removed_prev != survivor_nums[1] ||
            removed_next != survivor_nums[2] ||
            nexts[0] != survivor_nums[1] || prevs[1] != survivor_nums[0] ||
            nexts[1] != removed_leaf_page_num ||
            prevs[2] != removed_leaf_page_num ||
            max_keys[1] >= removed_key || removed_key >= min_keys[2]) {
            return false;
        }
        if (target_index < ancestor_key_count &&
            tinydb_parent_stage_key_at(ancestor, target_index) != max_keys[2]) {
            return false;
        }
    }

    unsigned char ancestor_scratch[PAGE_SIZE];
    unsigned char kept_scratch[PAGE_SIZE];
    unsigned char leaf_scratch[3][PAGE_SIZE];
    memcpy(ancestor_scratch, ancestor, PAGE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(leaf_scratch[i], surviving_leaf_pages[i], PAGE_SIZE);
    }

    if (removed_rightmost) {
        if (!tinydb_stage_leaf_sibling_relink(leaf_scratch[0],
                                              PAGE_SIZE,
                                              true,
                                              removed_leaf_page_num,
                                              survivor_nums[1]) ||
            !tinydb_stage_leaf_sibling_relink(leaf_scratch[1],
                                              PAGE_SIZE,
                                              false,
                                              removed_leaf_page_num,
                                              survivor_nums[0])) {
            return false;
        }
    } else {
        if (!tinydb_stage_leaf_sibling_relink(leaf_scratch[1],
                                              PAGE_SIZE,
                                              true,
                                              removed_leaf_page_num,
                                              survivor_nums[2]) ||
            !tinydb_stage_leaf_sibling_relink(leaf_scratch[2],
                                              PAGE_SIZE,
                                              false,
                                              removed_leaf_page_num,
                                              survivor_nums[1])) {
            return false;
        }
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        tinydb_nonroot_merge_write_u32(leaf_scratch[i] + PARENT_POINTER_OFFSET,
                                       kept_parent_num);
    }
    if (!tinydb_nonroot_merge_build_three_child_internal(kept_scratch,
                                                         ancestor_page_num,
                                                         survivor_nums,
                                                         max_keys)) {
        return false;
    }

    uint32_t removed_index = UINT32_MAX;
    bool ancestor_max_changed = false;
    uint32_t ancestor_new_max = 0u;
    if (!tinydb_stage_internal_child_remove(ancestor_scratch,
                                            PAGE_SIZE,
                                            obsolete_parent_num,
                                            obsolete_parent_max,
                                            &removed_index,
                                            &ancestor_max_changed,
                                            &ancestor_new_max) ||
        ancestor_max_changed || ancestor_new_max != 0u ||
        removed_index != (removed_rightmost ? target_index : sibling_index)) {
        return false;
    }

    uint32_t new_key_count = tinydb_parent_stage_read_u32(
        ancestor_scratch + INTERNAL_NODE_NUM_KEYS_OFFSET);
    bool found_kept = false;
    for (uint32_t i = 0u; i <= new_key_count; i++) {
        if (tinydb_parent_stage_child_at(ancestor_scratch, i) == kept_parent_num) {
            if (found_kept) return false;
            found_kept = true;
            if (i < new_key_count &&
                tinydb_parent_stage_key_at(ancestor_scratch, i) != max_keys[2]) {
                return false;
            }
        }
    }
    if (!found_kept ||
        !tinydb_parent_stage_validate(ancestor_scratch, PAGE_SIZE) ||
        ancestor_scratch[IS_ROOT_OFFSET] != ancestor[IS_ROOT_OFFSET] ||
        tinydb_nonroot_merge_read_u32(
            ancestor_scratch + PARENT_POINTER_OFFSET) !=
            tinydb_nonroot_merge_read_u32(ancestor + PARENT_POINTER_OFFSET)) {
        return false;
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        uint32_t checked_min = 0u;
        uint32_t checked_max = 0u;
        uint32_t checked_prev = INVALID_PAGE_NUM;
        uint32_t checked_next = INVALID_PAGE_NUM;
        if (!tinydb_nonroot_merge_leaf_valid(leaf_scratch[i],
                                             PAGE_SIZE,
                                             kept_parent_num,
                                             &checked_min,
                                             &checked_max,
                                             &checked_prev,
                                             &checked_next) ||
            checked_min != min_keys[i] || checked_max != max_keys[i]) {
            return false;
        }
        if (i > 0u && checked_prev != survivor_nums[i - 1u]) return false;
        if (i + 1u < 3u && checked_next != survivor_nums[i + 1u]) return false;
    }

    memcpy(ancestor_page, ancestor_scratch, PAGE_USABLE_SIZE);
    memcpy(kept_parent_num == target_parent_page_num
               ? target_parent_page
               : sibling_parent_page,
           kept_scratch,
           PAGE_USABLE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(surviving_leaf_pages[i], leaf_scratch[i], PAGE_USABLE_SIZE);
    }
    if (kept_parent_page_num_out != NULL) {
        *kept_parent_page_num_out = kept_parent_num;
    }
    if (obsolete_parent_page_num_out != NULL) {
        *obsolete_parent_page_num_out = obsolete_parent_num;
    }
    return true;
}

#endif /* TINYDB_INTERNAL_NONROOT_MERGE_STAGE_H */
