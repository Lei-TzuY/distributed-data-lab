#ifndef TINYDB_INTERNAL_MAX_DECREASE_CASCADE_STAGE_H
#define TINYDB_INTERNAL_MAX_DECREASE_CASCADE_STAGE_H

#include "slotted_v2_parent_max_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Atomically stage propagation of a lowered subtree maximum through internal
 * ancestors ordered from the changed child's immediate parent upward.
 *
 * TinyDB does not store a separator for an internal node's rightmost child.
 * Therefore a maximum decrease propagates upward only while the changed child
 * is rightmost. The first ancestor where it is not rightmost receives exactly
 * one separator rewrite and terminates propagation. If the rightmost chain
 * reaches the root, no ancestor page needs modification at all.
 *
 * All caller images are copied to private scratch storage first. On failure no
 * caller byte changes; on success only PAGE_USABLE_SIZE is published so Pager
 * checksum trailers remain untouched.
 */
static inline unsigned char* tinydb_max_cascade_page_at(void* pages,
                                                         size_t stride,
                                                         uint32_t index) {
    return (unsigned char*)pages + (size_t)index * stride;
}

static inline const unsigned char* tinydb_max_cascade_page_at_const(
    const void* pages,
    size_t stride,
    uint32_t index) {
    return (const unsigned char*)pages + (size_t)index * stride;
}

static inline bool tinydb_stage_internal_max_decrease_cascade(
    void* ancestor_pages,
    size_t ancestor_stride,
    const uint32_t* ancestor_page_nums,
    uint32_t ancestor_count,
    uint32_t changed_child_page_num,
    uint32_t old_max,
    uint32_t new_max,
    uint32_t* stop_level_out,
    bool* ancestor_changed_out) {
    if (stop_level_out != NULL) *stop_level_out = UINT32_MAX;
    if (ancestor_changed_out != NULL) *ancestor_changed_out = false;
    if (ancestor_pages == NULL || ancestor_page_nums == NULL ||
        ancestor_count == 0u || ancestor_stride < PAGE_SIZE ||
        changed_child_page_num == 0u ||
        changed_child_page_num == INVALID_PAGE_NUM || new_max >= old_max) {
        return false;
    }

    unsigned char* scratch =
        (unsigned char*)malloc((size_t)ancestor_count * PAGE_SIZE);
    if (scratch == NULL) return false;

    bool valid = true;
    for (uint32_t i = 0u; i < ancestor_count; i++) {
        const unsigned char* source = tinydb_max_cascade_page_at_const(
            ancestor_pages, ancestor_stride, i);
        memcpy(scratch + (size_t)i * PAGE_SIZE, source, PAGE_SIZE);
        if (ancestor_page_nums[i] == INVALID_PAGE_NUM) {
            valid = false;
            break;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (ancestor_page_nums[j] == ancestor_page_nums[i]) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
    }

    uint32_t child_page_num = changed_child_page_num;
    uint32_t stopped = UINT32_MAX;
    bool changed = false;
    bool success = false;

    for (uint32_t level = 0u; valid && level < ancestor_count; level++) {
        unsigned char* current = scratch + (size_t)level * PAGE_SIZE;
        if (!tinydb_parent_stage_validate(current, PAGE_SIZE)) break;

        uint32_t num_keys = tinydb_parent_stage_read_u32(
            current + INTERNAL_NODE_NUM_KEYS_OFFSET);
        uint32_t child_index = num_keys + 1u;
        for (uint32_t i = 0u; i <= num_keys; i++) {
            if (tinydb_parent_stage_child_at(current, i) == child_page_num) {
                if (child_index != num_keys + 1u) {
                    child_index = UINT32_MAX;
                    break;
                }
                child_index = i;
            }
        }
        if (child_index == UINT32_MAX || child_index == num_keys + 1u) break;

        bool is_root = current[IS_ROOT_OFFSET] != 0u;
        if (is_root && level + 1u != ancestor_count) break;

        if (child_index < num_keys) {
            bool separator_changed = false;
            uint32_t staged_index = UINT32_MAX;
            if (!tinydb_stage_parent_child_max_decrease(
                    current,
                    PAGE_SIZE,
                    child_page_num,
                    old_max,
                    new_max,
                    &staged_index,
                    &separator_changed) ||
                staged_index != child_index || !separator_changed) {
                break;
            }
            stopped = level;
            changed = true;
            success = true;
            break;
        }

        if (is_root) {
            stopped = level;
            success = true;
            break;
        }

        if (level + 1u >= ancestor_count || ancestor_page_nums[level] == 0u ||
            tinydb_parent_stage_read_u32(current + PARENT_POINTER_OFFSET) !=
                ancestor_page_nums[level + 1u]) {
            break;
        }
        child_page_num = ancestor_page_nums[level];
    }

    if (success) {
        for (uint32_t i = 0u; i < ancestor_count; i++) {
            unsigned char* destination = tinydb_max_cascade_page_at(
                ancestor_pages, ancestor_stride, i);
            memcpy(destination,
                   scratch + (size_t)i * PAGE_SIZE,
                   PAGE_USABLE_SIZE);
        }
        if (stop_level_out != NULL) *stop_level_out = stopped;
        if (ancestor_changed_out != NULL) *ancestor_changed_out = changed;
    }

    free(scratch);
    return success;
}

#endif
