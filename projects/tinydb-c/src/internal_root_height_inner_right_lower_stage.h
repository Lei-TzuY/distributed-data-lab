#ifndef TINYDB_INTERNAL_ROOT_HEIGHT_INNER_RIGHT_LOWER_STAGE_H
#define TINYDB_INTERNAL_ROOT_HEIGHT_INNER_RIGHT_LOWER_STAGE_H

#include "internal_nonroot_merge_stage.h"

/*
 * Stage the inner-right lower merge needed by height-4 root contraction:
 *
 *   kept [A, B] + obsolete [C, removed] + next -> kept [A, B, C] + next
 *
 * Unlike the outer-right variant, removed is not the chain tail. The operation
 * therefore repairs both sides of the cross-subtree boundary C<->next, reparents
 * A/B/C to kept, and rebuilds kept under new_parent_page_num. The next leaf keeps
 * its own parent. All writes are scratch-only until every relationship validates.
 */
static inline bool tinydb_stage_root_height_inner_right_lower_merge(
    void* kept_parent_page,
    size_t kept_capacity,
    uint32_t kept_parent_page_num,
    const void* obsolete_parent_page,
    size_t obsolete_capacity,
    uint32_t obsolete_parent_page_num,
    uint32_t old_parent_page_num,
    uint32_t new_parent_page_num,
    const void* removed_leaf_page,
    size_t removed_capacity,
    uint32_t removed_leaf_page_num,
    uint32_t removed_key,
    void* const survivor_leaf_pages[3],
    const uint32_t survivor_leaf_page_nums[3],
    void* next_leaf_page,
    size_t next_leaf_capacity,
    uint32_t next_leaf_page_num,
    uint32_t next_leaf_parent_page_num) {
    if (kept_parent_page == NULL || obsolete_parent_page == NULL ||
        removed_leaf_page == NULL || survivor_leaf_pages == NULL ||
        survivor_leaf_page_nums == NULL || next_leaf_page == NULL ||
        kept_capacity < PAGE_SIZE || obsolete_capacity < PAGE_SIZE ||
        removed_capacity < PAGE_SIZE || next_leaf_capacity < PAGE_SIZE ||
        kept_parent_page_num == 0u || kept_parent_page_num == INVALID_PAGE_NUM ||
        obsolete_parent_page_num == 0u ||
        obsolete_parent_page_num == INVALID_PAGE_NUM ||
        old_parent_page_num == 0u || old_parent_page_num == INVALID_PAGE_NUM ||
        new_parent_page_num == 0u || new_parent_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num == 0u || removed_leaf_page_num == INVALID_PAGE_NUM ||
        next_leaf_page_num == 0u || next_leaf_page_num == INVALID_PAGE_NUM ||
        next_leaf_parent_page_num == 0u ||
        next_leaf_parent_page_num == INVALID_PAGE_NUM ||
        kept_parent_page_num == obsolete_parent_page_num ||
        removed_leaf_page_num == next_leaf_page_num ||
        removed_leaf_page_num == kept_parent_page_num ||
        removed_leaf_page_num == obsolete_parent_page_num) {
        return false;
    }

    const unsigned char* kept = (const unsigned char*)kept_parent_page;
    const unsigned char* obsolete = (const unsigned char*)obsolete_parent_page;
    const unsigned char* removed = (const unsigned char*)removed_leaf_page;
    const unsigned char* next_leaf = (const unsigned char*)next_leaf_page;
    if (!tinydb_parent_stage_validate(kept, kept_capacity) ||
        kept[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(kept + PARENT_POINTER_OFFSET) !=
            old_parent_page_num ||
        tinydb_parent_stage_read_u32(kept + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_parent_stage_validate(obsolete, obsolete_capacity) ||
        obsolete[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(obsolete + PARENT_POINTER_OFFSET) !=
            old_parent_page_num ||
        tinydb_parent_stage_read_u32(obsolete + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(obsolete, 1u) != removed_leaf_page_num ||
        tinydb_leaf_format_detect_page(removed, removed_capacity) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed, removed_capacity) ||
        tinydb_leaf_format_detect_page(next_leaf, next_leaf_capacity) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(next_leaf, next_leaf_capacity)) {
        return false;
    }

    const uint32_t expected[3] = {
        tinydb_parent_stage_child_at(kept, 0u),
        tinydb_parent_stage_child_at(kept, 1u),
        tinydb_parent_stage_child_at(obsolete, 0u)
    };
    const uint32_t expected_parent[3] = {
        kept_parent_page_num, kept_parent_page_num, obsolete_parent_page_num
    };
    uint32_t mins[3], maxes[3], prevs[3], nexts[3];
    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_leaf_pages[i] == NULL ||
            survivor_leaf_page_nums[i] != expected[i] ||
            !tinydb_nonroot_merge_leaf_valid(
                (const unsigned char*)survivor_leaf_pages[i], PAGE_SIZE,
                expected_parent[i], &mins[i], &maxes[i], &prevs[i], &nexts[i]) ||
            (i > 0u && maxes[i - 1u] >= mins[i])) {
            return false;
        }
    }

    uint32_t removed_min = 0u, removed_max = 0u;
    uint32_t removed_prev = INVALID_PAGE_NUM, removed_next = INVALID_PAGE_NUM;
    uint32_t next_min = 0u, next_max = 0u;
    uint32_t next_prev = INVALID_PAGE_NUM, next_next = INVALID_PAGE_NUM;
    if (!tinydb_nonroot_merge_leaf_valid(removed, removed_capacity,
                                         obsolete_parent_page_num,
                                         &removed_min, &removed_max,
                                         &removed_prev, &removed_next) ||
        !tinydb_nonroot_merge_leaf_valid(next_leaf, next_leaf_capacity,
                                         next_leaf_parent_page_num,
                                         &next_min, &next_max,
                                         &next_prev, &next_next) ||
        removed_min != removed_key || removed_max != removed_key ||
        removed_prev != survivor_leaf_page_nums[2] ||
        removed_next != next_leaf_page_num ||
        next_prev != removed_leaf_page_num ||
        nexts[0] != survivor_leaf_page_nums[1] ||
        prevs[1] != survivor_leaf_page_nums[0] ||
        nexts[1] != survivor_leaf_page_nums[2] ||
        prevs[2] != survivor_leaf_page_nums[1] ||
        nexts[2] != removed_leaf_page_num ||
        maxes[0] != tinydb_parent_stage_key_at(kept, 0u) ||
        maxes[2] != tinydb_parent_stage_key_at(obsolete, 0u) ||
        maxes[2] >= removed_key || removed_key >= next_min) {
        return false;
    }
    (void)next_max;
    (void)next_next;

    unsigned char kept_scratch[PAGE_SIZE];
    unsigned char leaf_scratch[3][PAGE_SIZE];
    unsigned char next_scratch[PAGE_SIZE];
    memcpy(kept_scratch, kept, PAGE_SIZE);
    memcpy(next_scratch, next_leaf, PAGE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(leaf_scratch[i], survivor_leaf_pages[i], PAGE_SIZE);
    }

    if (!tinydb_stage_leaf_sibling_relink(
            leaf_scratch[2], PAGE_SIZE, true,
            removed_leaf_page_num, next_leaf_page_num) ||
        !tinydb_stage_leaf_sibling_relink(
            next_scratch, PAGE_SIZE, false,
            removed_leaf_page_num, survivor_leaf_page_nums[2])) {
        return false;
    }
    for (uint32_t i = 0u; i < 3u; i++) {
        tinydb_parent_stage_write_u32(leaf_scratch[i] + PARENT_POINTER_OFFSET,
                                      kept_parent_page_num);
    }
    if (!tinydb_nonroot_merge_build_three_child_internal(
            kept_scratch, new_parent_page_num, survivor_leaf_page_nums, maxes)) {
        return false;
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        uint32_t checked_min = 0u, checked_max = 0u;
        uint32_t checked_prev = INVALID_PAGE_NUM, checked_next = INVALID_PAGE_NUM;
        if (!tinydb_nonroot_merge_leaf_valid(
                leaf_scratch[i], PAGE_SIZE, kept_parent_page_num,
                &checked_min, &checked_max, &checked_prev, &checked_next) ||
            checked_min != mins[i] || checked_max != maxes[i]) {
            return false;
        }
    }

    uint32_t c_next = INVALID_PAGE_NUM;
    uint32_t checked_next_prev = INVALID_PAGE_NUM;
    uint32_t checked_next_parent = tinydb_parent_stage_read_u32(
        next_scratch + PARENT_POINTER_OFFSET);
    if (!tinydb_leaf_page_next(leaf_scratch[2], PAGE_SIZE, &c_next) ||
        !tinydb_leaf_page_prev(next_scratch, PAGE_SIZE, &checked_next_prev) ||
        c_next != next_leaf_page_num ||
        checked_next_prev != survivor_leaf_page_nums[2] ||
        checked_next_parent != next_leaf_parent_page_num ||
        !tinydb_slotted_leaf_v2_validate(next_scratch, PAGE_SIZE)) {
        return false;
    }

    memcpy(kept_parent_page, kept_scratch, PAGE_USABLE_SIZE);
    for (uint32_t i = 0u; i < 3u; i++) {
        memcpy(survivor_leaf_pages[i], leaf_scratch[i], PAGE_USABLE_SIZE);
    }
    memcpy(next_leaf_page, next_scratch, PAGE_USABLE_SIZE);
    return true;
}

#endif /* TINYDB_INTERNAL_ROOT_HEIGHT_INNER_RIGHT_LOWER_STAGE_H */
