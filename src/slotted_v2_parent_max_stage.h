#ifndef TINYDB_SLOTTED_V2_PARENT_MAX_STAGE_H
#define TINYDB_SLOTTED_V2_PARENT_MAX_STAGE_H

#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Stage the parent-side consequence of lowering one child's maximum key.
 * TinyDB stores a separator only for non-rightmost children, and that separator
 * is exactly the child's current maximum.  A rightmost child therefore needs
 * no local parent-page rewrite; callers must separately decide whether the
 * parent's own maximum must propagate farther upward.
 *
 * The parent image is changed only after the complete ordering/topology check
 * succeeds.  PAGE_USABLE_SIZE is copied so the Pager-owned checksum trailer is
 * never overwritten by this staging primitive.
 */
static inline bool tinydb_stage_parent_child_max_decrease(
    void* parent_page,
    size_t parent_capacity,
    uint32_t child_page_num,
    uint32_t old_child_max,
    uint32_t new_child_max,
    uint32_t* child_index_out,
    bool* separator_changed_out) {
    if (separator_changed_out != NULL) *separator_changed_out = false;
    if (parent_page == NULL || parent_capacity < PAGE_SIZE ||
        child_page_num == 0u || new_child_max >= old_child_max ||
        !tinydb_parent_stage_validate((const unsigned char*)parent_page,
                                      parent_capacity)) {
        return false;
    }

    const unsigned char* original = (const unsigned char*)parent_page;
    uint32_t num_keys = tinydb_parent_stage_read_u32(
        original + INTERNAL_NODE_NUM_KEYS_OFFSET);
    uint32_t child_index = num_keys + 1u;
    for (uint32_t i = 0u; i <= num_keys; i++) {
        if (tinydb_parent_stage_child_at(original, i) == child_page_num) {
            if (child_index != num_keys + 1u) return false;
            child_index = i;
        }
    }
    if (child_index == num_keys + 1u) return false;
    if (child_index_out != NULL) *child_index_out = child_index;

    if (child_index == num_keys) {
        return true;
    }

    if (tinydb_parent_stage_key_at(original, child_index) != old_child_max) {
        return false;
    }
    if (child_index > 0u &&
        new_child_max <= tinydb_parent_stage_key_at(original,
                                                    child_index - 1u)) {
        return false;
    }
    if (child_index + 1u < num_keys &&
        new_child_max >= tinydb_parent_stage_key_at(original,
                                                    child_index + 1u)) {
        return false;
    }

    unsigned char scratch[PAGE_SIZE];
    memcpy(scratch, original, PAGE_SIZE);
    tinydb_parent_stage_write_u32(
        tinydb_parent_stage_cell(scratch, child_index) +
            INTERNAL_NODE_CHILD_SIZE,
        new_child_max);
    if (!tinydb_parent_stage_validate(scratch, PAGE_SIZE) ||
        tinydb_parent_stage_child_at(scratch, child_index) != child_page_num ||
        tinydb_parent_stage_key_at(scratch, child_index) != new_child_max) {
        return false;
    }

    memcpy(parent_page, scratch, PAGE_USABLE_SIZE);
    if (separator_changed_out != NULL) *separator_changed_out = true;
    return true;
}

#endif
