#ifndef INTERNAL_ROOT_SPLIT_STAGE_H
#define INTERNAL_ROOT_SPLIT_STAGE_H

#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Stage the parent-overflow case for a leaf split when the leaf's parent is
 * already the full root internal node.
 *
 * TinyDB internal separators are the maximum key of every non-rightmost child.
 * A full root has INTERNAL_NODE_MAX_KEYS keys and therefore
 * INTERNAL_NODE_MAX_KEYS + 1 children. Splitting one child adds one more child;
 * rather than ever writing an over-capacity root page, this helper builds the
 * merged child/key sequence in ordinary C arrays, partitions it into two new
 * non-root internal pages, and rewrites the stable root page as a one-key root
 * pointing at those two internal pages. Page zero is a valid stable root page;
 * only newly allocated child/internal pages must be nonzero.
 *
 * Only PAGE_USABLE_SIZE is published into caller images. Pager-owned checksum
 * trailers are preserved byte-for-byte. The caller remains responsible for
 * updating each descendant child's common-header parent pointer and for
 * publishing/dirtying all staged pages atomically through Pager/WAL.
 */

static inline void tinydb_internal_root_stage_write_u32(unsigned char* p,
                                                         uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static inline bool tinydb_internal_root_stage_build_node(
    unsigned char* page,
    uint32_t page_num,
    uint32_t parent_page_num,
    const uint32_t* children,
    const uint32_t* keys,
    uint32_t child_count) {
    if (page == NULL || children == NULL || keys == NULL ||
        page_num == 0u || parent_page_num == page_num || child_count < 2u ||
        child_count - 1u > INTERNAL_NODE_MAX_KEYS) {
        return false;
    }

    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 0u;
    tinydb_internal_root_stage_write_u32(page + PARENT_POINTER_OFFSET,
                                         parent_page_num);
    tinydb_internal_root_stage_write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET,
                                         child_count - 1u);
    tinydb_internal_root_stage_write_u32(
        page + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
        children[child_count - 1u]);

    uint32_t previous_key = 0u;
    for (uint32_t i = 0u; i + 1u < child_count; i++) {
        if (children[i] == 0u || keys[i] == 0u ||
            (i > 0u && keys[i] <= previous_key)) {
            return false;
        }
        unsigned char* cell =
            page + INTERNAL_NODE_HEADER_SIZE + i * INTERNAL_NODE_CELL_SIZE;
        tinydb_internal_root_stage_write_u32(cell, children[i]);
        tinydb_internal_root_stage_write_u32(cell + INTERNAL_NODE_CHILD_SIZE,
                                             keys[i]);
        previous_key = keys[i];
    }
    if (children[child_count - 1u] == 0u) return false;
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static inline bool tinydb_stage_full_root_after_child_split(
    void* root_page,
    size_t root_capacity,
    uint32_t root_page_num,
    void* new_left_internal_page,
    size_t left_capacity,
    uint32_t new_left_internal_page_num,
    void* new_right_internal_page,
    size_t right_capacity,
    uint32_t new_right_internal_page_num,
    uint32_t split_left_child_page_num,
    uint32_t split_right_child_page_num,
    uint32_t old_left_max,
    uint32_t new_left_max,
    uint32_t new_right_max,
    uint32_t* left_child_count_out) {
    if (root_page == NULL || new_left_internal_page == NULL ||
        new_right_internal_page == NULL || root_capacity < PAGE_SIZE ||
        left_capacity < PAGE_SIZE || right_capacity < PAGE_SIZE ||
        new_left_internal_page_num == 0u ||
        new_right_internal_page_num == 0u ||
        root_page_num == new_left_internal_page_num ||
        root_page_num == new_right_internal_page_num ||
        new_left_internal_page_num == new_right_internal_page_num ||
        split_left_child_page_num == 0u || split_right_child_page_num == 0u ||
        split_left_child_page_num == split_right_child_page_num ||
        new_left_max >= new_right_max) {
        return false;
    }

    const unsigned char* original_root = (const unsigned char*)root_page;
    if (original_root[NODE_TYPE_OFFSET] != (unsigned char)NODE_INTERNAL ||
        original_root[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(
            original_root + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS ||
        !tinydb_parent_stage_validate(original_root, root_capacity)) {
        return false;
    }

    const uint32_t old_key_count = INTERNAL_NODE_MAX_KEYS;
    const uint32_t old_child_count = old_key_count + 1u;
    const uint32_t merged_child_count = old_child_count + 1u;
    const uint32_t merged_key_count = merged_child_count - 1u;

    uint32_t left_index = old_child_count;
    for (uint32_t i = 0u; i < old_child_count; i++) {
        uint32_t child = tinydb_parent_stage_child_at(original_root, i);
        if (child == split_right_child_page_num) return false;
        if (child == split_left_child_page_num) {
            if (left_index != old_child_count) return false;
            left_index = i;
        }
    }
    if (left_index == old_child_count) return false;

    bool left_was_rightmost = left_index == old_key_count;
    if (!left_was_rightmost) {
        if (tinydb_parent_stage_key_at(original_root, left_index) !=
                old_left_max ||
            new_right_max != old_left_max) {
            return false;
        }
    }

    uint32_t merged_children[INTERNAL_NODE_MAX_KEYS + 2u];
    uint32_t merged_keys[INTERNAL_NODE_MAX_KEYS + 1u];
    for (uint32_t i = 0u; i < merged_child_count; i++) {
        if (i <= left_index) {
            merged_children[i] = tinydb_parent_stage_child_at(original_root, i);
        } else if (i == left_index + 1u) {
            merged_children[i] = split_right_child_page_num;
        } else {
            merged_children[i] =
                tinydb_parent_stage_child_at(original_root, i - 1u);
        }
    }
    merged_children[left_index] = split_left_child_page_num;

    for (uint32_t i = 0u; i < merged_key_count; i++) {
        if (i < left_index) {
            merged_keys[i] = tinydb_parent_stage_key_at(original_root, i);
        } else if (i == left_index) {
            merged_keys[i] = new_left_max;
        } else if (i == left_index + 1u) {
            merged_keys[i] = new_right_max;
        } else {
            merged_keys[i] = tinydb_parent_stage_key_at(original_root, i - 1u);
        }
    }

    for (uint32_t i = 1u; i < merged_key_count; i++) {
        if (merged_keys[i] <= merged_keys[i - 1u]) return false;
    }

    uint32_t left_child_count = merged_child_count / 2u;
    uint32_t right_child_count = merged_child_count - left_child_count;
    if (left_child_count < 2u || right_child_count < 2u ||
        left_child_count - 1u > INTERNAL_NODE_MAX_KEYS ||
        right_child_count - 1u > INTERNAL_NODE_MAX_KEYS) {
        return false;
    }

    unsigned char scratch_root[PAGE_SIZE];
    unsigned char scratch_left[PAGE_SIZE];
    unsigned char scratch_right[PAGE_SIZE];
    memcpy(scratch_root, root_page, PAGE_SIZE);
    memcpy(scratch_left, new_left_internal_page, PAGE_SIZE);
    memcpy(scratch_right, new_right_internal_page, PAGE_SIZE);

    if (!tinydb_internal_root_stage_build_node(
            scratch_left,
            new_left_internal_page_num,
            root_page_num,
            merged_children,
            merged_keys,
            left_child_count) ||
        !tinydb_internal_root_stage_build_node(
            scratch_right,
            new_right_internal_page_num,
            root_page_num,
            merged_children + left_child_count,
            merged_keys + left_child_count,
            right_child_count)) {
        return false;
    }

    uint32_t root_separator = merged_keys[left_child_count - 1u];
    memset(scratch_root, 0, PAGE_USABLE_SIZE);
    scratch_root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    scratch_root[IS_ROOT_OFFSET] = 1u;
    tinydb_internal_root_stage_write_u32(scratch_root + PARENT_POINTER_OFFSET, 0u);
    tinydb_internal_root_stage_write_u32(
        scratch_root + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    unsigned char* root_cell = scratch_root + INTERNAL_NODE_HEADER_SIZE;
    tinydb_internal_root_stage_write_u32(root_cell,
                                         new_left_internal_page_num);
    tinydb_internal_root_stage_write_u32(root_cell + INTERNAL_NODE_CHILD_SIZE,
                                         root_separator);
    tinydb_internal_root_stage_write_u32(
        scratch_root + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
        new_right_internal_page_num);

    if (!tinydb_parent_stage_validate(scratch_root, PAGE_SIZE) ||
        tinydb_parent_stage_child_at(scratch_root, 0u) !=
            new_left_internal_page_num ||
        tinydb_parent_stage_child_at(scratch_root, 1u) !=
            new_right_internal_page_num ||
        tinydb_parent_stage_key_at(scratch_root, 0u) != root_separator) {
        return false;
    }

    memcpy(root_page, scratch_root, PAGE_USABLE_SIZE);
    memcpy(new_left_internal_page, scratch_left, PAGE_USABLE_SIZE);
    memcpy(new_right_internal_page, scratch_right, PAGE_USABLE_SIZE);
    if (left_child_count_out != NULL) {
        *left_child_count_out = left_child_count;
    }
    return true;
}

#endif /* INTERNAL_ROOT_SPLIT_STAGE_H */
