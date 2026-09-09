#ifndef SLOTTED_LEAF_V2_SPLIT_TXN_H
#define SLOTTED_LEAF_V2_SPLIT_TXN_H

#include "slotted_leaf_v2_split.h"

#include <stdlib.h>
#include <string.h>

/*
 * Atomically split a non-tail V2 leaf and repair the old next sibling's
 * backlink in the same caller-visible operation.
 *
 * Parent separator insertion is intentionally still outside this primitive;
 * the tree-level caller must perform that step before committing the pages.
 * This helper closes the smaller local-topology gap left by
 * tinydb_slotted_leaf_v2_split_nonroot(): either left, right and old_next all
 * become mutually consistent, or none of their usable bytes change.
 */
static inline bool tinydb_slotted_leaf_v2_split_nonroot_with_next(
    void* left_page,
    size_t left_capacity,
    uint32_t left_page_num,
    void* right_page,
    size_t right_capacity,
    uint32_t right_page_num,
    void* old_next_page,
    size_t old_next_capacity,
    uint32_t old_next_page_num,
    uint16_t* split_index_out) {
    if (left_page == NULL || right_page == NULL || old_next_page == NULL ||
        left_page == right_page || left_page == old_next_page ||
        right_page == old_next_page || left_capacity < PAGE_SIZE ||
        right_capacity < PAGE_SIZE || old_next_capacity < PAGE_SIZE ||
        left_page_num == right_page_num || left_page_num == old_next_page_num ||
        right_page_num == old_next_page_num ||
        !tinydb_slotted_leaf_v2_validate(left_page, left_capacity) ||
        !tinydb_slotted_leaf_v2_validate(old_next_page, old_next_capacity) ||
        ((const unsigned char*)left_page)[IS_ROOT_OFFSET] != 0u) {
        return false;
    }

    const unsigned char* left = (const unsigned char*)left_page;
    const unsigned char* old_next = (const unsigned char*)old_next_page;
    uint32_t recorded_next = tinydb_slotted_split_read_u32(
        left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET);
    uint32_t recorded_prev = tinydb_slotted_split_read_u32(
        old_next + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET);
    if (recorded_next != old_next_page_num || recorded_prev != left_page_num) {
        return false;
    }

    unsigned char* scratch_left = (unsigned char*)malloc(PAGE_SIZE);
    unsigned char* scratch_right = (unsigned char*)malloc(PAGE_SIZE);
    unsigned char* scratch_next = (unsigned char*)malloc(PAGE_SIZE);
    if (scratch_left == NULL || scratch_right == NULL || scratch_next == NULL) {
        free(scratch_left);
        free(scratch_right);
        free(scratch_next);
        return false;
    }

    memcpy(scratch_left, left_page, PAGE_SIZE);
    memcpy(scratch_right, right_page, PAGE_SIZE);
    memcpy(scratch_next, old_next_page, PAGE_SIZE);

    uint16_t split_index = 0u;
    bool ok = tinydb_slotted_leaf_v2_split_nonroot(scratch_left,
                                                    PAGE_SIZE,
                                                    left_page_num,
                                                    scratch_right,
                                                    PAGE_SIZE,
                                                    right_page_num,
                                                    &split_index);
    if (ok) {
        tinydb_slotted_split_write_u32(
            scratch_next + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            right_page_num);
        ok = tinydb_slotted_leaf_v2_validate(scratch_next, PAGE_SIZE) &&
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
        /* Pager checksum trailers remain caller-owned on all three pages. */
        memcpy(left_page, scratch_left, PAGE_USABLE_SIZE);
        memcpy(right_page, scratch_right, PAGE_USABLE_SIZE);
        memcpy(old_next_page, scratch_next, PAGE_USABLE_SIZE);
        if (split_index_out != NULL) *split_index_out = split_index;
    }

    free(scratch_left);
    free(scratch_right);
    free(scratch_next);
    return ok;
}

#endif /* SLOTTED_LEAF_V2_SPLIT_TXN_H */
