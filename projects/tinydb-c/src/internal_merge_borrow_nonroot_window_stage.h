#ifndef TINYDB_INTERNAL_MERGE_BORROW_NONROOT_WINDOW_STAGE_H
#define TINYDB_INTERNAL_MERGE_BORROW_NONROOT_WINDOW_STAGE_H

#include "internal_merge_borrow_root_cascade_left_stage.h"
#include "internal_merge_borrow_root_cascade_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Project one adjacent grandparent pair of a wider non-root ancestor into a
 * synthetic two-child root, reuse the proven root-level cascade, then copy the
 * resulting pair separator back into the original ancestor image.
 *
 * This is the important step from boundary-only height-five DELETE handling to
 * inner-position handling: siblings before and after the selected pair remain
 * byte-for-byte unchanged, while the ancestor's own subtree maximum remains
 * unchanged because neither direction changes the selected pair's rightmost
 * subtree maximum.
 */
static inline bool tinydb_nonroot_window_build_synthetic_root(
    unsigned char root[PAGE_SIZE],
    uint32_t ancestor_page_num,
    uint32_t left_grand_page_num,
    uint32_t right_grand_page_num,
    uint32_t separator) {
    if (root == NULL || ancestor_page_num == INVALID_PAGE_NUM ||
        left_grand_page_num == 0u || left_grand_page_num == INVALID_PAGE_NUM ||
        right_grand_page_num == 0u || right_grand_page_num == INVALID_PAGE_NUM ||
        left_grand_page_num == right_grand_page_num) {
        return false;
    }
    memset(root, 0, PAGE_SIZE);
    root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root[IS_ROOT_OFFSET] = 1u;
    tinydb_parent_stage_write_u32(root + PARENT_POINTER_OFFSET, 0u);
    tinydb_parent_stage_write_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    tinydb_parent_stage_write_u32(
        tinydb_parent_stage_cell(root, 0u), left_grand_page_num);
    tinydb_parent_stage_write_u32(
        tinydb_parent_stage_cell(root, 0u) + INTERNAL_NODE_CHILD_SIZE,
        separator);
    tinydb_parent_stage_write_u32(
        root + INTERNAL_NODE_RIGHT_CHILD_OFFSET, right_grand_page_num);
    return tinydb_parent_stage_validate(root, PAGE_SIZE) &&
           tinydb_parent_stage_child_at(root, 0u) == left_grand_page_num &&
           tinydb_parent_stage_child_at(root, 1u) == right_grand_page_num;
}

static inline bool tinydb_nonroot_window_validate_ancestor_pair(
    const unsigned char* ancestor,
    size_t ancestor_capacity,
    uint32_t ancestor_parent_page_num,
    uint32_t pair_index,
    uint32_t left_grand_page_num,
    uint32_t right_grand_page_num) {
    if (!tinydb_parent_stage_validate(ancestor, ancestor_capacity) ||
        ancestor[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(ancestor + PARENT_POINTER_OFFSET) !=
            ancestor_parent_page_num) {
        return false;
    }
    uint32_t keys = tinydb_parent_stage_read_u32(
        ancestor + INTERNAL_NODE_NUM_KEYS_OFFSET);
    return keys >= 1u && pair_index < keys &&
           tinydb_parent_stage_child_at(ancestor, pair_index) ==
               left_grand_page_num &&
           tinydb_parent_stage_child_at(ancestor, pair_index + 1u) ==
               right_grand_page_num;
}

static inline bool tinydb_stage_internal_merge_borrow_window_from_right(
    void* ancestor_page,
    size_t ancestor_capacity,
    uint32_t ancestor_page_num,
    uint32_t ancestor_parent_page_num,
    uint32_t pair_index,
    void* left_grand_page,
    size_t left_grand_capacity,
    uint32_t left_grand_page_num,
    void* right_grand_page,
    size_t right_grand_capacity,
    uint32_t right_grand_page_num,
    const void* obsolete_bottom_parent_page,
    size_t obsolete_bottom_capacity,
    uint32_t obsolete_bottom_parent_page_num,
    void* kept_bottom_parent_page,
    size_t kept_bottom_capacity,
    uint32_t kept_bottom_parent_page_num,
    void* donor_parent_page,
    size_t donor_parent_capacity,
    uint32_t donor_parent_page_num,
    const void* removed_leaf_page,
    size_t removed_leaf_capacity,
    uint32_t removed_leaf_page_num,
    uint32_t removed_key,
    void* const survivor_leaf_pages[3],
    const uint32_t survivor_leaf_page_nums[3],
    const void* const donor_leaf_pages[2],
    const uint32_t donor_leaf_page_nums[2],
    const void* next_subtree_leftmost_leaf_page,
    size_t next_subtree_leftmost_capacity,
    uint32_t next_subtree_leftmost_leaf_page_num) {
    if (ancestor_page == NULL || left_grand_page == NULL ||
        right_grand_page == NULL || kept_bottom_parent_page == NULL ||
        donor_parent_page == NULL || survivor_leaf_pages == NULL ||
        survivor_leaf_page_nums == NULL || ancestor_capacity < PAGE_SIZE ||
        left_grand_capacity < PAGE_SIZE || right_grand_capacity < PAGE_SIZE ||
        kept_bottom_capacity < PAGE_SIZE || donor_parent_capacity < PAGE_SIZE ||
        ancestor_page_num == 0u || ancestor_page_num == INVALID_PAGE_NUM ||
        ancestor_parent_page_num == INVALID_PAGE_NUM ||
        !tinydb_nonroot_window_validate_ancestor_pair(
            (const unsigned char*)ancestor_page,
            ancestor_capacity,
            ancestor_parent_page_num,
            pair_index,
            left_grand_page_num,
            right_grand_page_num)) {
        return false;
    }

    unsigned char ancestor_scratch[PAGE_SIZE];
    unsigned char synthetic_root[PAGE_SIZE];
    unsigned char left_grand_scratch[PAGE_SIZE];
    unsigned char right_grand_scratch[PAGE_SIZE];
    unsigned char kept_scratch[PAGE_SIZE];
    unsigned char donor_scratch[PAGE_SIZE];
    unsigned char survivor_scratch[3][PAGE_SIZE];
    void* survivor_ptrs[3] = {
        survivor_scratch[0], survivor_scratch[1], survivor_scratch[2]
    };
    memcpy(ancestor_scratch, ancestor_page, PAGE_SIZE);
    memcpy(left_grand_scratch, left_grand_page, PAGE_SIZE);
    memcpy(right_grand_scratch, right_grand_page, PAGE_SIZE);
    memcpy(kept_scratch, kept_bottom_parent_page, PAGE_SIZE);
    memcpy(donor_scratch, donor_parent_page, PAGE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_leaf_pages[i] == NULL) return false;
        memcpy(survivor_scratch[i], survivor_leaf_pages[i], PAGE_SIZE);
    }

    uint32_t old_separator = tinydb_parent_stage_key_at(
        ancestor_scratch, pair_index);
    if (!tinydb_nonroot_window_build_synthetic_root(
            synthetic_root,
            ancestor_page_num,
            left_grand_page_num,
            right_grand_page_num,
            old_separator) ||
        !tinydb_stage_internal_merge_borrow_from_right_grandparent(
            synthetic_root,
            PAGE_SIZE,
            ancestor_page_num,
            left_grand_scratch,
            PAGE_SIZE,
            left_grand_page_num,
            right_grand_scratch,
            PAGE_SIZE,
            right_grand_page_num,
            obsolete_bottom_parent_page,
            obsolete_bottom_capacity,
            obsolete_bottom_parent_page_num,
            kept_scratch,
            PAGE_SIZE,
            kept_bottom_parent_page_num,
            donor_scratch,
            PAGE_SIZE,
            donor_parent_page_num,
            removed_leaf_page,
            removed_leaf_capacity,
            removed_leaf_page_num,
            removed_key,
            survivor_ptrs,
            survivor_leaf_page_nums,
            donor_leaf_pages,
            donor_leaf_page_nums,
            next_subtree_leftmost_leaf_page,
            next_subtree_leftmost_capacity,
            next_subtree_leftmost_leaf_page_num)) {
        return false;
    }

    uint32_t new_separator = tinydb_parent_stage_key_at(synthetic_root, 0u);
    if (new_separator <= old_separator) return false;
    tinydb_parent_stage_write_u32(
        tinydb_parent_stage_cell(ancestor_scratch, pair_index) +
            INTERNAL_NODE_CHILD_SIZE,
        new_separator);
    if (!tinydb_nonroot_window_validate_ancestor_pair(
            ancestor_scratch,
            PAGE_SIZE,
            ancestor_parent_page_num,
            pair_index,
            left_grand_page_num,
            right_grand_page_num) ||
        tinydb_parent_stage_key_at(ancestor_scratch, pair_index) !=
            new_separator ||
        tinydb_parent_stage_read_u32(
            left_grand_scratch + PARENT_POINTER_OFFSET) != ancestor_page_num ||
        tinydb_parent_stage_read_u32(
            right_grand_scratch + PARENT_POINTER_OFFSET) != ancestor_page_num) {
        return false;
    }

    memcpy(ancestor_page, ancestor_scratch, PAGE_USABLE_SIZE);
    memcpy(left_grand_page, left_grand_scratch, PAGE_USABLE_SIZE);
    memcpy(right_grand_page, right_grand_scratch, PAGE_USABLE_SIZE);
    memcpy(kept_bottom_parent_page, kept_scratch, PAGE_USABLE_SIZE);
    memcpy(donor_parent_page, donor_scratch, PAGE_USABLE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(survivor_leaf_pages[i], survivor_scratch[i], PAGE_USABLE_SIZE);
    }
    return true;
}

static inline bool tinydb_stage_internal_merge_borrow_window_from_left(
    void* ancestor_page,
    size_t ancestor_capacity,
    uint32_t ancestor_page_num,
    uint32_t ancestor_parent_page_num,
    uint32_t pair_index,
    void* left_grand_page,
    size_t left_grand_capacity,
    uint32_t left_grand_page_num,
    void* right_grand_page,
    size_t right_grand_capacity,
    uint32_t right_grand_page_num,
    void* donor_parent_page,
    size_t donor_parent_capacity,
    uint32_t donor_parent_page_num,
    const void* obsolete_bottom_parent_page,
    size_t obsolete_bottom_capacity,
    uint32_t obsolete_bottom_parent_page_num,
    void* kept_bottom_parent_page,
    size_t kept_bottom_capacity,
    uint32_t kept_bottom_parent_page_num,
    const void* removed_leaf_page,
    size_t removed_leaf_capacity,
    uint32_t removed_leaf_page_num,
    uint32_t removed_key,
    void* const survivor_leaf_pages[3],
    const uint32_t survivor_leaf_page_nums[3],
    const void* const donor_leaf_pages[2],
    const uint32_t donor_leaf_page_nums[2],
    const void* previous_subtree_rightmost_leaf_page,
    size_t previous_subtree_rightmost_capacity,
    uint32_t previous_subtree_rightmost_leaf_page_num) {
    if (ancestor_page == NULL || left_grand_page == NULL ||
        right_grand_page == NULL || donor_parent_page == NULL ||
        kept_bottom_parent_page == NULL || survivor_leaf_pages == NULL ||
        survivor_leaf_page_nums == NULL || ancestor_capacity < PAGE_SIZE ||
        left_grand_capacity < PAGE_SIZE || right_grand_capacity < PAGE_SIZE ||
        donor_parent_capacity < PAGE_SIZE || kept_bottom_capacity < PAGE_SIZE ||
        ancestor_page_num == 0u || ancestor_page_num == INVALID_PAGE_NUM ||
        ancestor_parent_page_num == INVALID_PAGE_NUM ||
        !tinydb_nonroot_window_validate_ancestor_pair(
            (const unsigned char*)ancestor_page,
            ancestor_capacity,
            ancestor_parent_page_num,
            pair_index,
            left_grand_page_num,
            right_grand_page_num)) {
        return false;
    }

    unsigned char ancestor_scratch[PAGE_SIZE];
    unsigned char synthetic_root[PAGE_SIZE];
    unsigned char left_grand_scratch[PAGE_SIZE];
    unsigned char right_grand_scratch[PAGE_SIZE];
    unsigned char donor_scratch[PAGE_SIZE];
    unsigned char kept_scratch[PAGE_SIZE];
    unsigned char survivor_scratch[3][PAGE_SIZE];
    void* survivor_ptrs[3] = {
        survivor_scratch[0], survivor_scratch[1], survivor_scratch[2]
    };
    memcpy(ancestor_scratch, ancestor_page, PAGE_SIZE);
    memcpy(left_grand_scratch, left_grand_page, PAGE_SIZE);
    memcpy(right_grand_scratch, right_grand_page, PAGE_SIZE);
    memcpy(donor_scratch, donor_parent_page, PAGE_SIZE);
    memcpy(kept_scratch, kept_bottom_parent_page, PAGE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_leaf_pages[i] == NULL) return false;
        memcpy(survivor_scratch[i], survivor_leaf_pages[i], PAGE_SIZE);
    }

    uint32_t old_separator = tinydb_parent_stage_key_at(
        ancestor_scratch, pair_index);
    if (!tinydb_nonroot_window_build_synthetic_root(
            synthetic_root,
            ancestor_page_num,
            left_grand_page_num,
            right_grand_page_num,
            old_separator) ||
        !tinydb_stage_internal_merge_borrow_from_left_grandparent(
            synthetic_root,
            PAGE_SIZE,
            ancestor_page_num,
            left_grand_scratch,
            PAGE_SIZE,
            left_grand_page_num,
            right_grand_scratch,
            PAGE_SIZE,
            right_grand_page_num,
            donor_scratch,
            PAGE_SIZE,
            donor_parent_page_num,
            obsolete_bottom_parent_page,
            obsolete_bottom_capacity,
            obsolete_bottom_parent_page_num,
            kept_scratch,
            PAGE_SIZE,
            kept_bottom_parent_page_num,
            removed_leaf_page,
            removed_leaf_capacity,
            removed_leaf_page_num,
            removed_key,
            survivor_ptrs,
            survivor_leaf_page_nums,
            donor_leaf_pages,
            donor_leaf_page_nums,
            previous_subtree_rightmost_leaf_page,
            previous_subtree_rightmost_capacity,
            previous_subtree_rightmost_leaf_page_num)) {
        return false;
    }

    uint32_t new_separator = tinydb_parent_stage_key_at(synthetic_root, 0u);
    if (new_separator >= old_separator) return false;
    tinydb_parent_stage_write_u32(
        tinydb_parent_stage_cell(ancestor_scratch, pair_index) +
            INTERNAL_NODE_CHILD_SIZE,
        new_separator);
    if (!tinydb_nonroot_window_validate_ancestor_pair(
            ancestor_scratch,
            PAGE_SIZE,
            ancestor_parent_page_num,
            pair_index,
            left_grand_page_num,
            right_grand_page_num) ||
        tinydb_parent_stage_key_at(ancestor_scratch, pair_index) !=
            new_separator ||
        tinydb_parent_stage_read_u32(
            left_grand_scratch + PARENT_POINTER_OFFSET) != ancestor_page_num ||
        tinydb_parent_stage_read_u32(
            right_grand_scratch + PARENT_POINTER_OFFSET) != ancestor_page_num) {
        return false;
    }

    memcpy(ancestor_page, ancestor_scratch, PAGE_USABLE_SIZE);
    memcpy(left_grand_page, left_grand_scratch, PAGE_USABLE_SIZE);
    memcpy(right_grand_page, right_grand_scratch, PAGE_USABLE_SIZE);
    memcpy(donor_parent_page, donor_scratch, PAGE_USABLE_SIZE);
    memcpy(kept_bottom_parent_page, kept_scratch, PAGE_USABLE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(survivor_leaf_pages[i], survivor_scratch[i], PAGE_USABLE_SIZE);
    }
    return true;
}

#endif /* TINYDB_INTERNAL_MERGE_BORROW_NONROOT_WINDOW_STAGE_H */
