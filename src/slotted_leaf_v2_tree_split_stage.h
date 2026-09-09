#ifndef SLOTTED_LEAF_V2_TREE_SPLIT_STAGE_H
#define SLOTTED_LEAF_V2_TREE_SPLIT_STAGE_H

#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_leaf_v2_split_txn.h"

#include <stdlib.h>
#include <string.h>

/*
 * Stage the complete in-memory page set for a non-tail, non-root V2 leaf split:
 *
 *   left + new right + old next sibling + existing parent
 *
 * Every mutation is performed against PAGE_SIZE scratch copies first.  The
 * caller-visible pages are published only after the leaf redistribution,
 * reciprocal sibling repair, parent separator/child insertion, and final
 * structural cross-checks all succeed.  Pager-owned checksum trailers are not
 * copied back and therefore remain untouched.
 *
 * This deliberately stops short of Pager allocation, dirty tracking and WAL
 * publication.  It provides the atomic page-image boundary that those layers
 * can consume next.  Parent overflow remains fail-closed because recursive
 * internal-node split needs its own allocator/WAL transaction.
 */
static inline bool tinydb_slotted_leaf_v2_stage_tree_split_nonroot_with_next(
    void* left_page,
    size_t left_capacity,
    uint32_t left_page_num,
    void* right_page,
    size_t right_capacity,
    uint32_t right_page_num,
    void* old_next_page,
    size_t old_next_capacity,
    uint32_t old_next_page_num,
    void* parent_page,
    size_t parent_capacity,
    uint16_t* split_index_out,
    uint32_t* inserted_child_index_out) {
    if (left_page == NULL || right_page == NULL || old_next_page == NULL ||
        parent_page == NULL || left_page == right_page ||
        left_page == old_next_page || left_page == parent_page ||
        right_page == old_next_page || right_page == parent_page ||
        old_next_page == parent_page || left_capacity < PAGE_SIZE ||
        right_capacity < PAGE_SIZE || old_next_capacity < PAGE_SIZE ||
        parent_capacity < PAGE_SIZE || left_page_num == 0u ||
        right_page_num == 0u || old_next_page_num == 0u ||
        left_page_num == right_page_num ||
        left_page_num == old_next_page_num ||
        right_page_num == old_next_page_num ||
        !tinydb_slotted_leaf_v2_validate(left_page, left_capacity) ||
        !tinydb_slotted_leaf_v2_validate(old_next_page, old_next_capacity) ||
        !tinydb_parent_stage_validate((const unsigned char*)parent_page,
                                      parent_capacity)) {
        return false;
    }

    const unsigned char* original_left = (const unsigned char*)left_page;
    if (original_left[IS_ROOT_OFFSET] != 0u) return false;

    uint16_t original_count =
        tinydb_slotted_leaf_v2_count(left_page, left_capacity);
    if (original_count < 2u) return false;
    uint32_t old_left_max =
        tinydb_slotted_split_slot_key(left_page, (uint16_t)(original_count - 1u));

    unsigned char* scratch_left = (unsigned char*)malloc(PAGE_SIZE);
    unsigned char* scratch_right = (unsigned char*)malloc(PAGE_SIZE);
    unsigned char* scratch_next = (unsigned char*)malloc(PAGE_SIZE);
    unsigned char* scratch_parent = (unsigned char*)malloc(PAGE_SIZE);
    if (scratch_left == NULL || scratch_right == NULL || scratch_next == NULL ||
        scratch_parent == NULL) {
        free(scratch_left);
        free(scratch_right);
        free(scratch_next);
        free(scratch_parent);
        return false;
    }

    memcpy(scratch_left, left_page, PAGE_SIZE);
    memcpy(scratch_right, right_page, PAGE_SIZE);
    memcpy(scratch_next, old_next_page, PAGE_SIZE);
    memcpy(scratch_parent, parent_page, PAGE_SIZE);

    uint16_t split_index = 0u;
    bool ok = tinydb_slotted_leaf_v2_split_nonroot_with_next(
        scratch_left,
        PAGE_SIZE,
        left_page_num,
        scratch_right,
        PAGE_SIZE,
        right_page_num,
        scratch_next,
        PAGE_SIZE,
        old_next_page_num,
        &split_index);

    uint32_t new_left_max = 0u;
    uint32_t new_right_max = 0u;
    uint32_t inserted_child_index = 0u;
    if (ok) {
        uint16_t left_count =
            tinydb_slotted_leaf_v2_count(scratch_left, PAGE_SIZE);
        uint16_t right_count =
            tinydb_slotted_leaf_v2_count(scratch_right, PAGE_SIZE);
        if (left_count == 0u || right_count == 0u) {
            ok = false;
        } else {
            new_left_max = tinydb_slotted_split_slot_key(
                scratch_left, (uint16_t)(left_count - 1u));
            new_right_max = tinydb_slotted_split_slot_key(
                scratch_right, (uint16_t)(right_count - 1u));
            ok = new_left_max < new_right_max && new_right_max == old_left_max;
        }
    }

    if (ok) {
        ok = tinydb_slotted_leaf_v2_stage_parent_split(
            scratch_parent,
            PAGE_SIZE,
            left_page_num,
            right_page_num,
            old_left_max,
            new_left_max,
            new_right_max,
            &inserted_child_index);
    }

    if (ok) {
        uint32_t parent_keys = tinydb_parent_stage_read_u32(
            scratch_parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
        ok = tinydb_slotted_leaf_v2_validate(scratch_left, PAGE_SIZE) &&
             tinydb_slotted_leaf_v2_validate(scratch_right, PAGE_SIZE) &&
             tinydb_slotted_leaf_v2_validate(scratch_next, PAGE_SIZE) &&
             tinydb_parent_stage_validate(scratch_parent, PAGE_SIZE) &&
             inserted_child_index <= parent_keys &&
             tinydb_parent_stage_child_at(scratch_parent,
                                          inserted_child_index) ==
                 right_page_num &&
             tinydb_slotted_split_read_u32(
                 scratch_left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) ==
                 right_page_num &&
             tinydb_slotted_split_read_u32(
                 scratch_right + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) ==
                 left_page_num &&
             tinydb_slotted_split_read_u32(
                 scratch_right + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) ==
                 old_next_page_num &&
             tinydb_slotted_split_read_u32(
                 scratch_next + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) ==
                 right_page_num;
    }

    if (ok) {
        memcpy(left_page, scratch_left, PAGE_USABLE_SIZE);
        memcpy(right_page, scratch_right, PAGE_USABLE_SIZE);
        memcpy(old_next_page, scratch_next, PAGE_USABLE_SIZE);
        memcpy(parent_page, scratch_parent, PAGE_USABLE_SIZE);
        if (split_index_out != NULL) *split_index_out = split_index;
        if (inserted_child_index_out != NULL) {
            *inserted_child_index_out = inserted_child_index;
        }
    }

    free(scratch_left);
    free(scratch_right);
    free(scratch_next);
    free(scratch_parent);
    return ok;
}

#endif /* SLOTTED_LEAF_V2_TREE_SPLIT_STAGE_H */
