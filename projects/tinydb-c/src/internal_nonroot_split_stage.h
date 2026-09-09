#ifndef INTERNAL_NONROOT_SPLIT_STAGE_H
#define INTERNAL_NONROOT_SPLIT_STAGE_H

#include "internal_root_split_stage.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Split a completely full non-root internal node after one of its direct
 * children has split, without ever materializing an over-capacity page.
 *
 * The original internal page is reused as the left half. A caller-provided new
 * page becomes the right half. The helper merges the old child sequence plus
 * the new split-right child in ordinary arrays, partitions the sequence, and
 * only then publishes PAGE_USABLE_SIZE into the two page images. The caller
 * can subsequently insert the new right internal page into the grandparent
 * using tinydb_slotted_leaf_v2_stage_parent_split(), which is representation-
 * generic despite its historical name.
 *
 * The promoted_left_max output is the true subtree maximum of the rebuilt left
 * internal node: it is the separator that sat immediately to the right of the
 * left partition in the merged sequence. It is intentionally not stored in
 * the left internal page because TinyDB stores no separator for a node's own
 * rightmost child.
 *
 * Page zero is a valid catalog-stable root and therefore may be the
 * grandparent of this non-root internal node. Zero remains forbidden only for
 * newly allocated non-root page identities and child identities where it is
 * used as a sentinel by the surrounding tree representation.
 */
static inline bool tinydb_stage_full_nonroot_after_child_split(
    void* full_parent_page,
    size_t parent_capacity,
    uint32_t full_parent_page_num,
    void* new_right_internal_page,
    size_t right_capacity,
    uint32_t new_right_internal_page_num,
    uint32_t split_left_child_page_num,
    uint32_t split_right_child_page_num,
    uint32_t old_left_max,
    uint32_t new_left_max,
    uint32_t new_right_max,
    uint32_t* promoted_left_max_out,
    uint32_t* left_child_count_out) {
    if (full_parent_page == NULL || new_right_internal_page == NULL ||
        parent_capacity < PAGE_SIZE || right_capacity < PAGE_SIZE ||
        full_parent_page_num == 0u || new_right_internal_page_num == 0u ||
        full_parent_page_num == new_right_internal_page_num ||
        split_left_child_page_num == 0u || split_right_child_page_num == 0u ||
        split_left_child_page_num == split_right_child_page_num ||
        new_left_max >= new_right_max) {
        return false;
    }

    const unsigned char* original = (const unsigned char*)full_parent_page;
    uint32_t grandparent_page_num = tinydb_parent_stage_read_u32(
        original + PARENT_POINTER_OFFSET);
    if (original[NODE_TYPE_OFFSET] != (unsigned char)NODE_INTERNAL ||
        original[IS_ROOT_OFFSET] != 0u ||
        grandparent_page_num == full_parent_page_num ||
        tinydb_parent_stage_read_u32(
            original + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS ||
        !tinydb_parent_stage_validate(original, parent_capacity)) {
        return false;
    }

    const uint32_t old_key_count = INTERNAL_NODE_MAX_KEYS;
    const uint32_t old_child_count = old_key_count + 1u;
    const uint32_t merged_child_count = old_child_count + 1u;
    const uint32_t merged_key_count = merged_child_count - 1u;

    uint32_t left_index = old_child_count;
    for (uint32_t i = 0u; i < old_child_count; i++) {
        uint32_t child = tinydb_parent_stage_child_at(original, i);
        if (child == split_right_child_page_num) return false;
        if (child == split_left_child_page_num) {
            if (left_index != old_child_count) return false;
            left_index = i;
        }
    }
    if (left_index == old_child_count) return false;

    bool left_was_rightmost = left_index == old_key_count;
    if (!left_was_rightmost &&
        (tinydb_parent_stage_key_at(original, left_index) != old_left_max ||
         new_right_max != old_left_max)) {
        return false;
    }

    uint32_t merged_children[INTERNAL_NODE_MAX_KEYS + 2u];
    uint32_t merged_keys[INTERNAL_NODE_MAX_KEYS + 1u];
    for (uint32_t i = 0u; i < merged_child_count; i++) {
        if (i <= left_index) {
            merged_children[i] = tinydb_parent_stage_child_at(original, i);
        } else if (i == left_index + 1u) {
            merged_children[i] = split_right_child_page_num;
        } else {
            merged_children[i] = tinydb_parent_stage_child_at(original, i - 1u);
        }
    }
    merged_children[left_index] = split_left_child_page_num;

    for (uint32_t i = 0u; i < merged_key_count; i++) {
        if (i < left_index) {
            merged_keys[i] = tinydb_parent_stage_key_at(original, i);
        } else if (i == left_index) {
            merged_keys[i] = new_left_max;
        } else if (i == left_index + 1u) {
            merged_keys[i] = new_right_max;
        } else {
            merged_keys[i] = tinydb_parent_stage_key_at(original, i - 1u);
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

    uint32_t promoted_left_max = merged_keys[left_child_count - 1u];
    unsigned char scratch_left[PAGE_SIZE];
    unsigned char scratch_right[PAGE_SIZE];
    memcpy(scratch_left, full_parent_page, PAGE_SIZE);
    memcpy(scratch_right, new_right_internal_page, PAGE_SIZE);

    if (!tinydb_internal_root_stage_build_node(
            scratch_left,
            full_parent_page_num,
            grandparent_page_num,
            merged_children,
            merged_keys,
            left_child_count) ||
        !tinydb_internal_root_stage_build_node(
            scratch_right,
            new_right_internal_page_num,
            grandparent_page_num,
            merged_children + left_child_count,
            merged_keys + left_child_count,
            right_child_count)) {
        return false;
    }

    memcpy(full_parent_page, scratch_left, PAGE_USABLE_SIZE);
    memcpy(new_right_internal_page, scratch_right, PAGE_USABLE_SIZE);
    if (promoted_left_max_out != NULL) {
        *promoted_left_max_out = promoted_left_max;
    }
    if (left_child_count_out != NULL) {
        *left_child_count_out = left_child_count;
    }
    return true;
}

#endif /* INTERNAL_NONROOT_SPLIT_STAGE_H */
