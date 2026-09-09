#ifndef TINYDB_INTERNAL_MERGE_BORROW_NONROOT_CASCADE_LEFT_STAGE_H
#define TINYDB_INTERNAL_MERGE_BORROW_NONROOT_CASCADE_LEFT_STAGE_H

#include "internal_merge_borrow_root_cascade_left_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Mirror of the proven non-root right-donor adapter.
 *
 * The ancestor is deliberately non-root and has exactly two children. The
 * right/target grandparent is at minimum occupancy while the left/donor
 * grandparent has at least three bottom-parent children. A lower merge inside
 * the target would leave it with one child, so the donor's rightmost complete
 * bottom-parent subtree is moved across the grandparent boundary. The
 * ancestor's separator decreases to the donor grandparent's new maximum.
 * Because the ancestor's rightmost child remains the same target grandparent,
 * the ancestor subtree maximum is unchanged and its own parent requires no
 * separator rewrite.
 *
 * All mutable pages are copied into private scratch buffers. The ancestor
 * scratch image is temporarily marked as a synthetic root so the already
 * validated root-level mirrored cascade can be reused. Only after that helper
 * succeeds do we restore the original non-root identity and publish
 * PAGE_USABLE_SIZE back to caller buffers. Failure is atomic and Pager-owned
 * checksum trailer bytes remain untouched.
 */
static inline bool tinydb_stage_internal_merge_borrow_from_left_nonroot_ancestor(
    void* ancestor_page,
    size_t ancestor_capacity,
    uint32_t ancestor_page_num,
    uint32_t ancestor_parent_page_num,
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
        ancestor_parent_page_num == INVALID_PAGE_NUM) {
        return false;
    }

    const unsigned char* ancestor = (const unsigned char*)ancestor_page;
    if (!tinydb_parent_stage_validate(ancestor, ancestor_capacity) ||
        ancestor[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(ancestor + PARENT_POINTER_OFFSET) !=
            ancestor_parent_page_num ||
        tinydb_parent_stage_read_u32(
            ancestor + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(ancestor, 0u) != left_grand_page_num ||
        tinydb_parent_stage_child_at(ancestor, 1u) != right_grand_page_num) {
        return false;
    }

    unsigned char ancestor_scratch[PAGE_SIZE];
    unsigned char left_grand_scratch[PAGE_SIZE];
    unsigned char right_grand_scratch[PAGE_SIZE];
    unsigned char donor_scratch[PAGE_SIZE];
    unsigned char kept_scratch[PAGE_SIZE];
    unsigned char survivor_scratch[3][PAGE_SIZE];
    void* survivor_scratch_ptrs[3] = {
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

    ancestor_scratch[IS_ROOT_OFFSET] = 1u;
    tinydb_parent_stage_write_u32(
        ancestor_scratch + PARENT_POINTER_OFFSET, 0u);
    if (!tinydb_parent_stage_validate(ancestor_scratch, PAGE_SIZE)) {
        return false;
    }

    if (!tinydb_stage_internal_merge_borrow_from_left_grandparent(
            ancestor_scratch,
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
            survivor_scratch_ptrs,
            survivor_leaf_page_nums,
            donor_leaf_pages,
            donor_leaf_page_nums,
            previous_subtree_rightmost_leaf_page,
            previous_subtree_rightmost_capacity,
            previous_subtree_rightmost_leaf_page_num)) {
        return false;
    }

    ancestor_scratch[IS_ROOT_OFFSET] = 0u;
    tinydb_parent_stage_write_u32(
        ancestor_scratch + PARENT_POINTER_OFFSET,
        ancestor_parent_page_num);
    if (!tinydb_parent_stage_validate(ancestor_scratch, PAGE_SIZE) ||
        ancestor_scratch[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            ancestor_scratch + PARENT_POINTER_OFFSET) !=
            ancestor_parent_page_num ||
        tinydb_parent_stage_child_at(ancestor_scratch, 0u) !=
            left_grand_page_num ||
        tinydb_parent_stage_child_at(ancestor_scratch, 1u) !=
            right_grand_page_num ||
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
        memcpy(survivor_leaf_pages[i],
               survivor_scratch[i],
               PAGE_USABLE_SIZE);
    }
    return true;
}

#endif /* TINYDB_INTERNAL_MERGE_BORROW_NONROOT_CASCADE_LEFT_STAGE_H */
