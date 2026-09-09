#ifndef TINYDB_INTERNAL_MERGE_BORROW_ROOT_CASCADE_LEFT_STAGE_H
#define TINYDB_INTERNAL_MERGE_BORROW_ROOT_CASCADE_LEFT_STAGE_H

#include "internal_merge_borrow_root_cascade_stage.h"

/*
 * Mirror of tinydb_stage_internal_merge_borrow_from_right_grandparent().
 *
 * The right grandparent starts at minimum occupancy with two minimum bottom
 * parents. Removing the singleton rightmost leaf of its left/obsolete bottom
 * parent requires a lower merge that keeps the right bottom parent. That lower
 * merge would leave the right grandparent with one child, so the complete
 * rightmost donor bottom-parent subtree of the left grandparent is moved across
 * the root boundary. The left grandparent must start with at least three
 * children, so losing its rightmost donor remains legal. Because that donor was
 * the left grandparent maximum, the root separator decreases to the left
 * grandparent's new maximum.
 *
 * No caller-visible image is modified until every rebuilt page validates.
 * Removed leaf and obsolete bottom parent are source-only. Donor leaves and the
 * previous subtree's rightmost leaf are source-only because the upper-level
 * parent move preserves the existing leaf chain.
 */
static inline bool tinydb_stage_internal_merge_borrow_from_left_grandparent(
    void* root_page,
    size_t root_capacity,
    uint32_t root_page_num,
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
    if (root_page == NULL || left_grand_page == NULL ||
        right_grand_page == NULL || donor_parent_page == NULL ||
        obsolete_bottom_parent_page == NULL || kept_bottom_parent_page == NULL ||
        removed_leaf_page == NULL || survivor_leaf_pages == NULL ||
        survivor_leaf_page_nums == NULL || donor_leaf_pages == NULL ||
        donor_leaf_page_nums == NULL || previous_subtree_rightmost_leaf_page == NULL ||
        root_capacity < PAGE_SIZE || left_grand_capacity < PAGE_SIZE ||
        right_grand_capacity < PAGE_SIZE || donor_parent_capacity < PAGE_SIZE ||
        obsolete_bottom_capacity < PAGE_SIZE || kept_bottom_capacity < PAGE_SIZE ||
        removed_leaf_capacity < PAGE_SIZE ||
        previous_subtree_rightmost_capacity < PAGE_SIZE ||
        root_page_num == INVALID_PAGE_NUM ||
        left_grand_page_num == 0u || left_grand_page_num == INVALID_PAGE_NUM ||
        right_grand_page_num == 0u || right_grand_page_num == INVALID_PAGE_NUM ||
        donor_parent_page_num == 0u || donor_parent_page_num == INVALID_PAGE_NUM ||
        obsolete_bottom_parent_page_num == 0u ||
        obsolete_bottom_parent_page_num == INVALID_PAGE_NUM ||
        kept_bottom_parent_page_num == 0u ||
        kept_bottom_parent_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num == 0u || removed_leaf_page_num == INVALID_PAGE_NUM ||
        previous_subtree_rightmost_leaf_page_num == 0u ||
        previous_subtree_rightmost_leaf_page_num == INVALID_PAGE_NUM) {
        return false;
    }

    const unsigned char* root = (const unsigned char*)root_page;
    const unsigned char* left_grand = (const unsigned char*)left_grand_page;
    const unsigned char* right_grand = (const unsigned char*)right_grand_page;
    const unsigned char* donor = (const unsigned char*)donor_parent_page;
    const unsigned char* obsolete =
        (const unsigned char*)obsolete_bottom_parent_page;
    const unsigned char* kept = (const unsigned char*)kept_bottom_parent_page;
    const unsigned char* removed = (const unsigned char*)removed_leaf_page;
    const unsigned char* previous_right =
        (const unsigned char*)previous_subtree_rightmost_leaf_page;

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
        tinydb_parent_stage_read_u32(left_grand + INTERNAL_NODE_NUM_KEYS_OFFSET) <
            2u ||
        !tinydb_parent_stage_validate(right_grand, right_grand_capacity) ||
        right_grand[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(right_grand + PARENT_POINTER_OFFSET) !=
            root_page_num ||
        tinydb_parent_stage_read_u32(right_grand + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        tinydb_parent_stage_child_at(right_grand, 0u) !=
            obsolete_bottom_parent_page_num ||
        tinydb_parent_stage_child_at(right_grand, 1u) !=
            kept_bottom_parent_page_num ||
        !tinydb_parent_stage_validate(donor, donor_parent_capacity) ||
        donor[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(donor + PARENT_POINTER_OFFSET) !=
            left_grand_page_num ||
        tinydb_parent_stage_read_u32(donor + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_parent_stage_validate(obsolete, obsolete_bottom_capacity) ||
        obsolete[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(obsolete + PARENT_POINTER_OFFSET) !=
            right_grand_page_num ||
        tinydb_parent_stage_read_u32(obsolete + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        !tinydb_parent_stage_validate(kept, kept_bottom_capacity) ||
        kept[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(kept + PARENT_POINTER_OFFSET) !=
            right_grand_page_num ||
        tinydb_parent_stage_read_u32(kept + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_leaf_format_detect_page(removed, removed_leaf_capacity) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed, removed_leaf_capacity)) {
        return false;
    }

    uint32_t left_key_count = tinydb_parent_stage_read_u32(
        left_grand + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (tinydb_parent_stage_child_at(left_grand, left_key_count) !=
        donor_parent_page_num) {
        return false;
    }
    uint32_t previous_parent_num = tinydb_parent_stage_child_at(
        left_grand, left_key_count - 1u);
    uint32_t old_left_max = tinydb_parent_stage_key_at(root, 0u);

    uint32_t donor_children[2] = {
        tinydb_parent_stage_child_at(donor, 0u),
        tinydb_parent_stage_child_at(donor, 1u)
    };
    if (donor_leaf_page_nums[0] != donor_children[0] ||
        donor_leaf_page_nums[1] != donor_children[1] ||
        donor_leaf_pages[0] == NULL || donor_leaf_pages[1] == NULL ||
        donor_children[0] == donor_children[1]) {
        return false;
    }

    uint32_t donor_min[2];
    uint32_t donor_max[2];
    uint32_t donor_prev[2];
    uint32_t donor_next[2];
    for (uint32_t i = 0u; i < 2u; i++) {
        if (!tinydb_nonroot_merge_leaf_valid(
                (const unsigned char*)donor_leaf_pages[i],
                PAGE_SIZE,
                donor_parent_page_num,
                &donor_min[i],
                &donor_max[i],
                &donor_prev[i],
                &donor_next[i]) ||
            (i > 0u && donor_max[i - 1u] >= donor_min[i])) {
            return false;
        }
    }
    uint32_t donor_subtree_max = donor_max[1];
    if (donor_max[0] != tinydb_parent_stage_key_at(donor, 0u) ||
        donor_subtree_max != old_left_max) {
        return false;
    }

    uint32_t previous_min = 0u;
    uint32_t previous_max = 0u;
    uint32_t previous_prev = INVALID_PAGE_NUM;
    uint32_t previous_next = INVALID_PAGE_NUM;
    if (!tinydb_nonroot_merge_leaf_valid(previous_right,
                                         previous_subtree_rightmost_capacity,
                                         previous_parent_num,
                                         &previous_min,
                                         &previous_max,
                                         &previous_prev,
                                         &previous_next) ||
        previous_subtree_rightmost_leaf_page_num == donor_leaf_page_nums[0] ||
        previous_max != tinydb_parent_stage_key_at(left_grand,
                                                    left_key_count - 1u) ||
        previous_next != donor_leaf_page_nums[0] ||
        donor_prev[0] != previous_subtree_rightmost_leaf_page_num ||
        donor_next[0] != donor_leaf_page_nums[1] ||
        donor_prev[1] != donor_leaf_page_nums[0] ||
        previous_max >= donor_min[0]) {
        return false;
    }
    (void)previous_min;
    (void)previous_prev;

    uint32_t obsolete_left = tinydb_parent_stage_child_at(obsolete, 0u);
    uint32_t obsolete_right = tinydb_parent_stage_child_at(obsolete, 1u);
    uint32_t kept_left = tinydb_parent_stage_child_at(kept, 0u);
    uint32_t kept_right = tinydb_parent_stage_child_at(kept, 1u);
    if (obsolete_right != removed_leaf_page_num ||
        survivor_leaf_page_nums[0] != obsolete_left ||
        survivor_leaf_page_nums[1] != kept_left ||
        survivor_leaf_page_nums[2] != kept_right ||
        tinydb_parent_stage_key_at(obsolete, 0u) >= removed_key) {
        return false;
    }

    uint32_t survivor_min[3];
    uint32_t survivor_max[3];
    uint32_t survivor_prev[3];
    uint32_t survivor_next[3];
    const uint32_t survivor_expected_parent[3] = {
        obsolete_bottom_parent_page_num,
        kept_bottom_parent_page_num,
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
                                         obsolete_bottom_parent_page_num,
                                         &removed_min,
                                         &removed_max,
                                         &removed_prev,
                                         &removed_next) ||
        removed_min != removed_key || removed_max != removed_key ||
        survivor_max[0] != tinydb_parent_stage_key_at(obsolete, 0u) ||
        survivor_max[1] != tinydb_parent_stage_key_at(kept, 0u) ||
        removed_prev != survivor_leaf_page_nums[0] ||
        removed_next != survivor_leaf_page_nums[1] ||
        survivor_next[0] != removed_leaf_page_num ||
        survivor_prev[1] != removed_leaf_page_num ||
        survivor_next[1] != survivor_leaf_page_nums[2] ||
        survivor_prev[2] != survivor_leaf_page_nums[1] ||
        survivor_max[0] >= removed_key || removed_key >= survivor_min[1] ||
        donor_subtree_max >= survivor_min[0] ||
        donor_next[1] != survivor_leaf_page_nums[0] ||
        survivor_prev[0] != donor_leaf_page_nums[1]) {
        return false;
    }

    unsigned char root_scratch[PAGE_SIZE];
    unsigned char left_grand_scratch[PAGE_SIZE];
    unsigned char right_grand_scratch[PAGE_SIZE];
    unsigned char donor_scratch[PAGE_SIZE];
    unsigned char kept_scratch[PAGE_SIZE];
    unsigned char survivor_scratch[3][PAGE_SIZE];
    memcpy(root_scratch, root, PAGE_SIZE);
    memcpy(left_grand_scratch, left_grand, PAGE_SIZE);
    memcpy(right_grand_scratch, right_grand, PAGE_SIZE);
    memcpy(donor_scratch, donor, PAGE_SIZE);
    memcpy(kept_scratch, kept, PAGE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(survivor_scratch[i], survivor_leaf_pages[i], PAGE_SIZE);
    }

    if (!tinydb_stage_leaf_sibling_relink(survivor_scratch[0],
                                          PAGE_SIZE,
                                          true,
                                          removed_leaf_page_num,
                                          survivor_leaf_page_nums[1]) ||
        !tinydb_stage_leaf_sibling_relink(survivor_scratch[1],
                                          PAGE_SIZE,
                                          false,
                                          removed_leaf_page_num,
                                          survivor_leaf_page_nums[0])) {
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

    uint32_t removed_donor_index = UINT32_MAX;
    bool left_grand_max_changed = false;
    uint32_t left_grand_new_max = 0u;
    if (!tinydb_stage_internal_child_remove(left_grand_scratch,
                                            PAGE_SIZE,
                                            donor_parent_page_num,
                                            donor_subtree_max,
                                            &removed_donor_index,
                                            &left_grand_max_changed,
                                            &left_grand_new_max) ||
        removed_donor_index != left_key_count || !left_grand_max_changed ||
        left_grand_new_max != previous_max ||
        left_grand_new_max >= donor_subtree_max) {
        return false;
    }

    if (!tinydb_internal_borrow_build_two_child_parent(
            right_grand_scratch,
            root_page_num,
            donor_parent_page_num,
            donor_subtree_max,
            kept_bottom_parent_page_num)) {
        return false;
    }
    tinydb_parent_stage_write_u32(donor_scratch + PARENT_POINTER_OFFSET,
                                  right_grand_page_num);
    if (!tinydb_parent_stage_validate(donor_scratch, PAGE_SIZE) ||
        tinydb_parent_stage_read_u32(donor_scratch + PARENT_POINTER_OFFSET) !=
            right_grand_page_num) {
        return false;
    }

    tinydb_parent_stage_write_u32(
        tinydb_parent_stage_cell(root_scratch, 0u) + INTERNAL_NODE_CHILD_SIZE,
        left_grand_new_max);
    if (!tinydb_parent_stage_validate(root_scratch, PAGE_SIZE) ||
        tinydb_parent_stage_child_at(root_scratch, 0u) != left_grand_page_num ||
        tinydb_parent_stage_child_at(root_scratch, 1u) != right_grand_page_num ||
        tinydb_parent_stage_key_at(root_scratch, 0u) != left_grand_new_max ||
        tinydb_parent_stage_key_at(root_scratch, 0u) >= old_left_max) {
        return false;
    }

    if (!tinydb_parent_stage_validate(left_grand_scratch, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(right_grand_scratch, PAGE_SIZE) ||
        tinydb_parent_stage_child_at(right_grand_scratch, 0u) != donor_parent_page_num ||
        tinydb_parent_stage_key_at(right_grand_scratch, 0u) != donor_subtree_max ||
        tinydb_parent_stage_child_at(right_grand_scratch, 1u) !=
            kept_bottom_parent_page_num) {
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
    uint32_t final_prev = INVALID_PAGE_NUM;
    if (!tinydb_leaf_page_prev(survivor_scratch[0], PAGE_SIZE, &final_prev) ||
        final_prev != donor_leaf_page_nums[1]) {
        return false;
    }

    memcpy(root_page, root_scratch, PAGE_USABLE_SIZE);
    memcpy(left_grand_page, left_grand_scratch, PAGE_USABLE_SIZE);
    memcpy(right_grand_page, right_grand_scratch, PAGE_USABLE_SIZE);
    memcpy(donor_parent_page, donor_scratch, PAGE_USABLE_SIZE);
    memcpy(kept_bottom_parent_page, kept_scratch, PAGE_USABLE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(survivor_leaf_pages[i], survivor_scratch[i], PAGE_USABLE_SIZE);
    }
    return true;
}

#endif /* TINYDB_INTERNAL_MERGE_BORROW_ROOT_CASCADE_LEFT_STAGE_H */
