#ifndef TINYDB_INTERNAL_CHILD_REMOVE_STAGE_H
#define TINYDB_INTERNAL_CHILD_REMOVE_STAGE_H

#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Remove one direct child from an internal page without allowing the parent to
 * underflow below two remaining children. TinyDB stores max(child[i]) for each
 * non-rightmost child and stores the final child separately.
 *
 * Removing a non-rightmost child removes its matching separator and does not
 * change the parent's subtree maximum. Removing the rightmost child promotes
 * the old penultimate child to rightmost; its formerly stored separator is the
 * parent's new subtree maximum and is returned for ancestor propagation.
 *
 * The caller page is updated only after the rebuilt image validates. Only
 * PAGE_USABLE_SIZE is copied so Pager-owned checksum trailers remain intact.
 */
static inline bool tinydb_stage_internal_child_remove(
    void* parent_page,
    size_t parent_capacity,
    uint32_t removed_child_page_num,
    uint32_t removed_child_max,
    uint32_t* removed_index_out,
    bool* parent_max_changed_out,
    uint32_t* new_parent_max_out) {
    if (removed_index_out != NULL) *removed_index_out = UINT32_MAX;
    if (parent_max_changed_out != NULL) *parent_max_changed_out = false;
    if (new_parent_max_out != NULL) *new_parent_max_out = 0u;
    if (parent_page == NULL || parent_capacity < PAGE_SIZE ||
        removed_child_page_num == 0u ||
        removed_child_page_num == INVALID_PAGE_NUM ||
        !tinydb_parent_stage_validate((const unsigned char*)parent_page,
                                      parent_capacity)) {
        return false;
    }

    const unsigned char* original = (const unsigned char*)parent_page;
    uint32_t old_key_count = tinydb_parent_stage_read_u32(
        original + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (old_key_count < 2u || old_key_count > INTERNAL_NODE_MAX_KEYS) {
        return false;
    }
    uint32_t old_child_count = old_key_count + 1u;
    uint32_t remove_index = old_child_count;
    for (uint32_t i = 0u; i < old_child_count; i++) {
        if (tinydb_parent_stage_child_at(original, i) == removed_child_page_num) {
            if (remove_index != old_child_count) return false;
            remove_index = i;
        }
    }
    if (remove_index == old_child_count) return false;

    bool removed_rightmost = remove_index == old_key_count;
    if (!removed_rightmost &&
        tinydb_parent_stage_key_at(original, remove_index) !=
            removed_child_max) {
        return false;
    }

    uint32_t new_child_count = old_child_count - 1u;
    uint32_t new_key_count = new_child_count - 1u;
    uint32_t children[INTERNAL_NODE_MAX_KEYS + 1u];
    uint32_t keys[INTERNAL_NODE_MAX_KEYS];

    uint32_t child_out = 0u;
    for (uint32_t i = 0u; i < old_child_count; i++) {
        if (i == remove_index) continue;
        children[child_out++] = tinydb_parent_stage_child_at(original, i);
    }
    if (child_out != new_child_count) return false;

    uint32_t key_out = 0u;
    for (uint32_t i = 0u; i < old_key_count; i++) {
        if ((!removed_rightmost && i == remove_index) ||
            (removed_rightmost && i + 1u == old_key_count)) {
            continue;
        }
        keys[key_out++] = tinydb_parent_stage_key_at(original, i);
    }
    if (key_out != new_key_count) return false;

    unsigned char scratch[PAGE_SIZE];
    memcpy(scratch, original, PAGE_SIZE);
    unsigned char is_root = original[IS_ROOT_OFFSET];
    uint32_t parent_page_num = tinydb_parent_stage_read_u32(
        original + PARENT_POINTER_OFFSET);
    memset(scratch, 0, PAGE_USABLE_SIZE);
    scratch[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    scratch[IS_ROOT_OFFSET] = is_root;
    tinydb_parent_stage_write_u32(scratch + PARENT_POINTER_OFFSET,
                                  parent_page_num);
    tinydb_parent_stage_write_u32(scratch + INTERNAL_NODE_NUM_KEYS_OFFSET,
                                  new_key_count);
    tinydb_parent_stage_write_u32(scratch + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
                                  children[new_child_count - 1u]);

    for (uint32_t i = 0u; i < new_key_count; i++) {
        unsigned char* cell = tinydb_parent_stage_cell(scratch, i);
        tinydb_parent_stage_write_u32(cell, children[i]);
        tinydb_parent_stage_write_u32(cell + INTERNAL_NODE_CHILD_SIZE,
                                      keys[i]);
    }
    if (!tinydb_parent_stage_validate(scratch, PAGE_SIZE)) return false;

    uint32_t new_parent_max = removed_rightmost
        ? tinydb_parent_stage_key_at(original, old_key_count - 1u)
        : 0u;
    if (removed_rightmost &&
        (new_parent_max == 0u || new_parent_max >= removed_child_max)) {
        return false;
    }

    memcpy(parent_page, scratch, PAGE_USABLE_SIZE);
    if (removed_index_out != NULL) *removed_index_out = remove_index;
    if (parent_max_changed_out != NULL) {
        *parent_max_changed_out = removed_rightmost;
    }
    if (new_parent_max_out != NULL) *new_parent_max_out = new_parent_max;
    return true;
}

#endif
