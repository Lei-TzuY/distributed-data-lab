#ifndef SLOTTED_LEAF_V2_ROOT_SPLIT_STAGE_H
#define SLOTTED_LEAF_V2_ROOT_SPLIT_STAGE_H

#include "leaf_page_access.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_leaf_v2_split.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Stage the first structural growth of a V2 tree: a full root leaf becomes a
 * one-key internal root while its rows are redistributed into two newly
 * allocated non-root V2 leaves.
 *
 * The stable root page number is preserved.  This is important because catalog
 * metadata stores that page number and callers should not need to rewrite the
 * schema merely because the tree grows from height one to height two.
 *
 * All three caller images are updated only after the complete transformation
 * validates.  PAGE_USABLE_SIZE is copied on success so Pager-owned checksum
 * trailers remain byte-for-byte untouched.
 */
static inline void tinydb_root_split_stage_write_u32(unsigned char* p,
                                                      uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static inline bool tinydb_slotted_leaf_v2_stage_root_split(
    void* root_page,
    size_t root_capacity,
    uint32_t root_page_num,
    void* left_child_page,
    size_t left_capacity,
    uint32_t left_child_page_num,
    void* right_child_page,
    size_t right_capacity,
    uint32_t right_child_page_num,
    uint32_t* separator_out) {
    if (root_page == NULL || left_child_page == NULL ||
        right_child_page == NULL || root_capacity < PAGE_SIZE ||
        left_capacity < PAGE_SIZE || right_capacity < PAGE_SIZE ||
        left_child_page_num == 0u || right_child_page_num == 0u ||
        left_child_page_num == right_child_page_num ||
        left_child_page_num == root_page_num ||
        right_child_page_num == root_page_num ||
        !tinydb_slotted_leaf_v2_validate(root_page, root_capacity) ||
        ((const unsigned char*)root_page)[IS_ROOT_OFFSET] == 0u) {
        return false;
    }

    const unsigned char* original = (const unsigned char*)root_page;
    uint32_t prev = tinydb_slotted_split_read_u32(
        original + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET);
    uint32_t next = tinydb_slotted_split_read_u32(
        original + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET);
    if (prev != 0u || next != 0u ||
        tinydb_slotted_leaf_v2_count(original, PAGE_SIZE) < 2u) {
        return false;
    }

    unsigned char scratch_root[PAGE_SIZE];
    unsigned char scratch_left[PAGE_SIZE];
    unsigned char scratch_right[PAGE_SIZE];
    memcpy(scratch_root, root_page, PAGE_SIZE);
    memcpy(scratch_left, root_page, PAGE_SIZE);
    memcpy(scratch_right, right_child_page, PAGE_SIZE);

    scratch_left[IS_ROOT_OFFSET] = 0u;
    tinydb_root_split_stage_write_u32(
        scratch_left + PARENT_POINTER_OFFSET,
        root_page_num);

    if (!tinydb_slotted_leaf_v2_split_nonroot(
            scratch_left,
            PAGE_SIZE,
            left_child_page_num,
            scratch_right,
            PAGE_SIZE,
            right_child_page_num,
            NULL)) {
        return false;
    }

    uint16_t left_count = tinydb_slotted_leaf_v2_count(scratch_left, PAGE_SIZE);
    uint16_t right_count =
        tinydb_slotted_leaf_v2_count(scratch_right, PAGE_SIZE);
    uint32_t separator = 0u;
    if (left_count == 0u || right_count == 0u ||
        !tinydb_leaf_page_key_at(scratch_left,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &separator)) {
        return false;
    }

    memset(scratch_root, 0, PAGE_USABLE_SIZE);
    scratch_root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    scratch_root[IS_ROOT_OFFSET] = 1u;
    tinydb_root_split_stage_write_u32(
        scratch_root + PARENT_POINTER_OFFSET,
        0u);
    tinydb_root_split_stage_write_u32(
        scratch_root + INTERNAL_NODE_NUM_KEYS_OFFSET,
        1u);
    unsigned char* root_cell = scratch_root + INTERNAL_NODE_HEADER_SIZE;
    tinydb_root_split_stage_write_u32(root_cell, left_child_page_num);
    tinydb_root_split_stage_write_u32(
        root_cell + INTERNAL_NODE_CHILD_SIZE,
        separator);
    tinydb_root_split_stage_write_u32(
        scratch_root + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
        right_child_page_num);

    if (!tinydb_parent_stage_validate(scratch_root, PAGE_SIZE) ||
        tinydb_parent_stage_child_at(scratch_root, 0u) != left_child_page_num ||
        tinydb_parent_stage_child_at(scratch_root, 1u) !=
            right_child_page_num ||
        tinydb_parent_stage_key_at(scratch_root, 0u) != separator ||
        tinydb_slotted_split_read_u32(
            scratch_left + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) != 0u ||
        tinydb_slotted_split_read_u32(
            scratch_left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) !=
            right_child_page_num ||
        tinydb_slotted_split_read_u32(
            scratch_right + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) !=
            left_child_page_num ||
        tinydb_slotted_split_read_u32(
            scratch_right + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != 0u ||
        tinydb_slotted_split_read_u32(
            scratch_left + PARENT_POINTER_OFFSET) != root_page_num ||
        tinydb_slotted_split_read_u32(
            scratch_right + PARENT_POINTER_OFFSET) != root_page_num) {
        return false;
    }

    memcpy(root_page, scratch_root, PAGE_USABLE_SIZE);
    memcpy(left_child_page, scratch_left, PAGE_USABLE_SIZE);
    memcpy(right_child_page, scratch_right, PAGE_USABLE_SIZE);
    if (separator_out != NULL) *separator_out = separator;
    return true;
}

#endif /* SLOTTED_LEAF_V2_ROOT_SPLIT_STAGE_H */
