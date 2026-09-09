#ifndef TINYDB_INTERNAL_MERGE_ROOT_HEIGHT_CASCADE_STAGE_H
#define TINYDB_INTERNAL_MERGE_ROOT_HEIGHT_CASCADE_STAGE_H

#include "internal_nonroot_merge_stage.h"
#include "internal_root_collapse_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Stage the first recursive V2 DELETE case where neither root grandparent can
 * lend a bottom-parent subtree.
 *
 * Before:
 *
 *                         stable root
 *                        /           \
 *                  left grand      right grand
 *                    /    \           /    \
 *              obsolete   kept      q0      q1
 *                /  \     /  \      /\      /\
 *               A    B removed C   ...     ...
 *
 * Both grandparents are minimum two-child internal nodes. Removing the
 * singleton leftmost leaf of kept requires a lower merge. Keeping kept as
 * [A,B,C] would leave left grand with one child, while right grand cannot lend
 * because it is also minimum. The correct bounded repair is therefore to merge
 * at the grandparent level and contract the stable root by one level:
 *
 *                         stable root
 *                       /      |      \
 *                    kept      q0      q1
 *
 * The root page number never changes. kept is first rebuilt below right grand
 * only as a temporary valid image; a proven root-collapse helper then promotes
 * the temporary [kept,q0,q1] right-grand image into the stable root and
 * reparents all three direct children to the root.
 *
 * Caller-visible mutation is staged entirely in scratch memory. left grand,
 * right grand, obsolete bottom parent, and removed leaf are source-only; a live
 * Pager/WAL caller may reclaim those four pages only after publishing root,
 * kept, q0, q1, and the three surviving leaf images. Only PAGE_USABLE_SIZE is
 * copied on success so Pager-owned checksum trailers are preserved.
 */
static inline bool tinydb_stage_internal_merge_root_height_cascade(
    void* root_page,
    size_t root_capacity,
    uint32_t root_page_num,
    const void* left_grand_page,
    size_t left_grand_capacity,
    uint32_t left_grand_page_num,
    const void* right_grand_page,
    size_t right_grand_capacity,
    uint32_t right_grand_page_num,
    const void* obsolete_bottom_parent_page,
    size_t obsolete_bottom_capacity,
    uint32_t obsolete_bottom_parent_page_num,
    void* kept_bottom_parent_page,
    size_t kept_bottom_capacity,
    uint32_t kept_bottom_parent_page_num,
    void* right_left_parent_page,
    size_t right_left_parent_capacity,
    uint32_t right_left_parent_page_num,
    void* right_right_parent_page,
    size_t right_right_parent_capacity,
    uint32_t right_right_parent_page_num,
    const void* removed_leaf_page,
    size_t removed_leaf_capacity,
    uint32_t removed_leaf_page_num,
    uint32_t removed_key,
    void* const survivor_leaf_pages[3],
    const uint32_t survivor_leaf_page_nums[3],
    const void* right_subtree_leftmost_leaf_page,
    size_t right_subtree_leftmost_capacity,
    uint32_t right_subtree_leftmost_leaf_page_num,
    const void* right_subtree_rightmost_leaf_page,
    size_t right_subtree_rightmost_capacity,
    uint32_t right_subtree_rightmost_leaf_page_num) {
    if (root_page == NULL || left_grand_page == NULL ||
        right_grand_page == NULL || obsolete_bottom_parent_page == NULL ||
        kept_bottom_parent_page == NULL || right_left_parent_page == NULL ||
        right_right_parent_page == NULL || removed_leaf_page == NULL ||
        survivor_leaf_pages == NULL || survivor_leaf_page_nums == NULL ||
        right_subtree_leftmost_leaf_page == NULL ||
        right_subtree_rightmost_leaf_page == NULL ||
        root_capacity < PAGE_SIZE || left_grand_capacity < PAGE_SIZE ||
        right_grand_capacity < PAGE_SIZE || obsolete_bottom_capacity < PAGE_SIZE ||
        kept_bottom_capacity < PAGE_SIZE ||
        right_left_parent_capacity < PAGE_SIZE ||
        right_right_parent_capacity < PAGE_SIZE ||
        removed_leaf_capacity < PAGE_SIZE ||
        right_subtree_leftmost_capacity < PAGE_SIZE ||
        right_subtree_rightmost_capacity < PAGE_SIZE ||
        root_page_num == INVALID_PAGE_NUM ||
        left_grand_page_num == 0u || left_grand_page_num == INVALID_PAGE_NUM ||
        right_grand_page_num == 0u || right_grand_page_num == INVALID_PAGE_NUM ||
        obsolete_bottom_parent_page_num == 0u ||
        obsolete_bottom_parent_page_num == INVALID_PAGE_NUM ||
        kept_bottom_parent_page_num == 0u ||
        kept_bottom_parent_page_num == INVALID_PAGE_NUM ||
        right_left_parent_page_num == 0u ||
        right_left_parent_page_num == INVALID_PAGE_NUM ||
        right_right_parent_page_num == 0u ||
        right_right_parent_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num == 0u || removed_leaf_page_num == INVALID_PAGE_NUM ||
        right_subtree_leftmost_leaf_page_num == 0u ||
        right_subtree_leftmost_leaf_page_num == INVALID_PAGE_NUM ||
        right_subtree_rightmost_leaf_page_num == 0u ||
        right_subtree_rightmost_leaf_page_num == INVALID_PAGE_NUM) {
        return false;
    }

    const unsigned char* root = (const unsigned char*)root_page;
    const unsigned char* left_grand = (const unsigned char*)left_grand_page;
    const unsigned char* right_grand = (const unsigned char*)right_grand_page;
    const unsigned char* obsolete =
        (const unsigned char*)obsolete_bottom_parent_page;
    const unsigned char* kept = (const unsigned char*)kept_bottom_parent_page;
    const unsigned char* right_left =
        (const unsigned char*)right_left_parent_page;
    const unsigned char* right_right =
        (const unsigned char*)right_right_parent_page;
    const unsigned char* removed = (const unsigned char*)removed_leaf_page;
    const unsigned char* right_first =
        (const unsigned char*)right_subtree_leftmost_leaf_page;
    const unsigned char* right_last =
        (const unsigned char*)right_subtree_rightmost_leaf_page;

    if (!tinydb_parent_stage_validate(root, root_capacity) ||
        root[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(root + PARENT_POINTER_OFFSET) != 0u ||
        tinydb_parent_stage_read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(root, 0u) != left_grand_page_num ||
        tinydb_parent_stage_child_at(root, 1u) != right_grand_page_num ||
        !tinydb_parent_stage_validate(left_grand, left_grand_capacity) ||
        left_grand[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(left_grand + PARENT_POINTER_OFFSET) !=
            root_page_num ||
        tinydb_parent_stage_read_u32(
            left_grand + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(left_grand, 0u) !=
            obsolete_bottom_parent_page_num ||
        tinydb_parent_stage_child_at(left_grand, 1u) !=
            kept_bottom_parent_page_num ||
        !tinydb_parent_stage_validate(right_grand, right_grand_capacity) ||
        right_grand[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(right_grand + PARENT_POINTER_OFFSET) !=
            root_page_num ||
        tinydb_parent_stage_read_u32(
            right_grand + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(right_grand, 0u) !=
            right_left_parent_page_num ||
        tinydb_parent_stage_child_at(right_grand, 1u) !=
            right_right_parent_page_num ||
        !tinydb_parent_stage_validate(obsolete, obsolete_bottom_capacity) ||
        obsolete[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(obsolete + PARENT_POINTER_OFFSET) !=
            left_grand_page_num ||
        tinydb_parent_stage_read_u32(
            obsolete + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_parent_stage_validate(kept, kept_bottom_capacity) ||
        kept[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(kept + PARENT_POINTER_OFFSET) !=
            left_grand_page_num ||
        tinydb_parent_stage_read_u32(kept + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_parent_stage_validate(right_left, right_left_parent_capacity) ||
        right_left[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(right_left + PARENT_POINTER_OFFSET) !=
            right_grand_page_num ||
        tinydb_parent_stage_read_u32(
            right_left + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_parent_stage_validate(right_right, right_right_parent_capacity) ||
        right_right[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(right_right + PARENT_POINTER_OFFSET) !=
            right_grand_page_num ||
        tinydb_parent_stage_read_u32(
            right_right + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_leaf_format_detect_page(removed, removed_leaf_capacity) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed, removed_leaf_capacity)) {
        return false;
    }

    uint32_t obsolete_left = tinydb_parent_stage_child_at(obsolete, 0u);
    uint32_t obsolete_right = tinydb_parent_stage_child_at(obsolete, 1u);
    uint32_t kept_left = tinydb_parent_stage_child_at(kept, 0u);
    uint32_t kept_right = tinydb_parent_stage_child_at(kept, 1u);
    if (kept_left != removed_leaf_page_num ||
        survivor_leaf_page_nums[0] != obsolete_left ||
        survivor_leaf_page_nums[1] != obsolete_right ||
        survivor_leaf_page_nums[2] != kept_right ||
        tinydb_parent_stage_key_at(kept, 0u) != removed_key) {
        return false;
    }

    uint32_t survivor_min[3];
    uint32_t survivor_max[3];
    uint32_t survivor_prev[3];
    uint32_t survivor_next[3];
    const uint32_t survivor_expected_parent[3] = {
        obsolete_bottom_parent_page_num,
        obsolete_bottom_parent_page_num,
        kept_bottom_parent_page_num
    };
    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_leaf_pages[i] == NULL ||
            !tinydb_nonroot_merge_leaf_valid(
                (const unsigned char*)survivor_leaf_pages[i],
                PAGE_SIZE,
                survivor_expected_parent[i],
                &survivor_min[i],
                &survivor_max[i],
                &survivor_prev[i],
                &survivor_next[i]) ||
            (i > 0u && survivor_max[i - 1u] >= survivor_min[i])) {
            return false;
        }
    }

    uint32_t removed_min = 0u;
    uint32_t removed_max = 0u;
    uint32_t removed_prev = INVALID_PAGE_NUM;
    uint32_t removed_next = INVALID_PAGE_NUM;
    if (!tinydb_nonroot_merge_leaf_valid(removed,
                                         removed_leaf_capacity,
                                         kept_bottom_parent_page_num,
                                         &removed_min,
                                         &removed_max,
                                         &removed_prev,
                                         &removed_next) ||
        removed_min != removed_key || removed_max != removed_key ||
        survivor_max[0] != tinydb_parent_stage_key_at(obsolete, 0u) ||
        survivor_max[1] != tinydb_parent_stage_key_at(left_grand, 0u) ||
        tinydb_parent_stage_key_at(root, 0u) != survivor_max[2] ||
        removed_prev != survivor_leaf_page_nums[1] ||
        removed_next != survivor_leaf_page_nums[2] ||
        survivor_next[0] != survivor_leaf_page_nums[1] ||
        survivor_prev[1] != survivor_leaf_page_nums[0] ||
        survivor_next[1] != removed_leaf_page_num ||
        survivor_prev[2] != removed_leaf_page_num ||
        survivor_max[1] >= removed_key || removed_key >= survivor_min[2]) {
        return false;
    }

    uint32_t right_first_min = 0u;
    uint32_t right_first_max = 0u;
    uint32_t right_first_prev = INVALID_PAGE_NUM;
    uint32_t right_first_next = INVALID_PAGE_NUM;
    uint32_t right_last_min = 0u;
    uint32_t right_last_max = 0u;
    uint32_t right_last_prev = INVALID_PAGE_NUM;
    uint32_t right_last_next = INVALID_PAGE_NUM;
    uint32_t right_left_first_child =
        tinydb_parent_stage_child_at(right_left, 0u);
    uint32_t right_right_last_child =
        tinydb_parent_stage_child_at(right_right, 1u);
    if (right_subtree_leftmost_leaf_page_num != right_left_first_child ||
        right_subtree_rightmost_leaf_page_num != right_right_last_child ||
        !tinydb_nonroot_merge_leaf_valid(right_first,
                                         right_subtree_leftmost_capacity,
                                         right_left_parent_page_num,
                                         &right_first_min,
                                         &right_first_max,
                                         &right_first_prev,
                                         &right_first_next) ||
        !tinydb_nonroot_merge_leaf_valid(right_last,
                                         right_subtree_rightmost_capacity,
                                         right_right_parent_page_num,
                                         &right_last_min,
                                         &right_last_max,
                                         &right_last_prev,
                                         &right_last_next) ||
        survivor_next[2] != right_subtree_leftmost_leaf_page_num ||
        right_first_prev != survivor_leaf_page_nums[2] ||
        survivor_max[2] >= right_first_min ||
        right_last_next != 0u ||
        right_last_max <= tinydb_parent_stage_key_at(right_grand, 0u)) {
        return false;
    }
    (void)right_first_max;
    (void)right_first_next;
    (void)right_last_min;
    (void)right_last_prev;

    uint32_t right_left_subtree_max =
        tinydb_parent_stage_key_at(right_grand, 0u);
    uint32_t right_subtree_max = right_last_max;
    if (right_left_subtree_max <= survivor_max[2] ||
        right_subtree_max <= right_left_subtree_max) {
        return false;
    }

    unsigned char root_scratch[PAGE_SIZE];
    unsigned char kept_scratch[PAGE_SIZE];
    unsigned char right_left_scratch[PAGE_SIZE];
    unsigned char right_right_scratch[PAGE_SIZE];
    unsigned char right_grand_scratch[PAGE_SIZE];
    unsigned char survivor_scratch[3][PAGE_SIZE];
    memcpy(root_scratch, root, PAGE_SIZE);
    memcpy(kept_scratch, kept, PAGE_SIZE);
    memcpy(right_left_scratch, right_left, PAGE_SIZE);
    memcpy(right_right_scratch, right_right, PAGE_SIZE);
    memcpy(right_grand_scratch, right_grand, PAGE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(survivor_scratch[i], survivor_leaf_pages[i], PAGE_SIZE);
    }

    if (!tinydb_stage_leaf_sibling_relink(survivor_scratch[1],
                                          PAGE_SIZE,
                                          true,
                                          removed_leaf_page_num,
                                          survivor_leaf_page_nums[2]) ||
        !tinydb_stage_leaf_sibling_relink(survivor_scratch[2],
                                          PAGE_SIZE,
                                          false,
                                          removed_leaf_page_num,
                                          survivor_leaf_page_nums[1])) {
        return false;
    }
    for (uint32_t i = 0u; i < 3u; i++) {
        tinydb_parent_stage_write_u32(
            survivor_scratch[i] + PARENT_POINTER_OFFSET,
            kept_bottom_parent_page_num);
    }

    if (!tinydb_nonroot_merge_build_three_child_internal(
            kept_scratch,
            right_grand_page_num,
            survivor_leaf_page_nums,
            survivor_max)) {
        return false;
    }

    const uint32_t merged_children[3] = {
        kept_bottom_parent_page_num,
        right_left_parent_page_num,
        right_right_parent_page_num
    };
    const uint32_t merged_maxes[3] = {
        survivor_max[2],
        right_left_subtree_max,
        right_subtree_max
    };
    if (!tinydb_nonroot_merge_build_three_child_internal(
            right_grand_scratch,
            root_page_num,
            merged_children,
            merged_maxes)) {
        return false;
    }

    void* direct_child_pages[3] = {
        kept_scratch,
        right_left_scratch,
        right_right_scratch
    };
    uint32_t promoted_page_num = INVALID_PAGE_NUM;
    if (!tinydb_stage_internal_root_collapse_to_internal(
            root_scratch,
            PAGE_SIZE,
            root_page_num,
            right_grand_scratch,
            PAGE_SIZE,
            right_grand_page_num,
            right_subtree_max,
            left_grand_page_num,
            survivor_max[2],
            direct_child_pages,
            merged_children,
            3u,
            &promoted_page_num) ||
        promoted_page_num != right_grand_page_num) {
        return false;
    }

    if (!tinydb_parent_stage_validate(root_scratch, PAGE_SIZE) ||
        root_scratch[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(root_scratch + PARENT_POINTER_OFFSET) != 0u ||
        tinydb_parent_stage_child_at(root_scratch, 0u) !=
            kept_bottom_parent_page_num ||
        tinydb_parent_stage_key_at(root_scratch, 0u) != survivor_max[2] ||
        tinydb_parent_stage_child_at(root_scratch, 1u) !=
            right_left_parent_page_num ||
        tinydb_parent_stage_key_at(root_scratch, 1u) != right_left_subtree_max ||
        tinydb_parent_stage_child_at(root_scratch, 2u) !=
            right_right_parent_page_num ||
        tinydb_parent_stage_read_u32(kept_scratch + PARENT_POINTER_OFFSET) !=
            root_page_num ||
        tinydb_parent_stage_read_u32(
            right_left_scratch + PARENT_POINTER_OFFSET) != root_page_num ||
        tinydb_parent_stage_read_u32(
            right_right_scratch + PARENT_POINTER_OFFSET) != root_page_num) {
        return false;
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        uint32_t checked_min = 0u;
        uint32_t checked_max = 0u;
        uint32_t checked_prev = INVALID_PAGE_NUM;
        uint32_t checked_next = INVALID_PAGE_NUM;
        if (!tinydb_nonroot_merge_leaf_valid(survivor_scratch[i],
                                              PAGE_SIZE,
                                              kept_bottom_parent_page_num,
                                              &checked_min,
                                              &checked_max,
                                              &checked_prev,
                                              &checked_next) ||
            checked_min != survivor_min[i] || checked_max != survivor_max[i]) {
            return false;
        }
        if (i > 0u && checked_prev != survivor_leaf_page_nums[i - 1u]) {
            return false;
        }
        if (i + 1u < 3u &&
            checked_next != survivor_leaf_page_nums[i + 1u]) {
            return false;
        }
    }
    uint32_t final_next = INVALID_PAGE_NUM;
    if (!tinydb_leaf_page_next(survivor_scratch[2], PAGE_SIZE, &final_next) ||
        final_next != right_subtree_leftmost_leaf_page_num) {
        return false;
    }

    memcpy(root_page, root_scratch, PAGE_USABLE_SIZE);
    memcpy(kept_bottom_parent_page, kept_scratch, PAGE_USABLE_SIZE);
    memcpy(right_left_parent_page, right_left_scratch, PAGE_USABLE_SIZE);
    memcpy(right_right_parent_page, right_right_scratch, PAGE_USABLE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(survivor_leaf_pages[i], survivor_scratch[i], PAGE_USABLE_SIZE);
    }
    return true;
}

#endif /* TINYDB_INTERNAL_MERGE_ROOT_HEIGHT_CASCADE_STAGE_H */
