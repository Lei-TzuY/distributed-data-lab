#ifndef INTERNAL_SPLIT_CASCADE_STAGE_H
#define INTERNAL_SPLIT_CASCADE_STAGE_H

#include "internal_nonroot_split_stage.h"
#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Atomically stage upward propagation of one already-split child through an
 * arbitrary chain of internal ancestors.
 *
 * ancestor_pages contains PAGE_SIZE-byte page images ordered from the child's
 * immediate parent upward. ancestor_page_nums and ancestor_old_maxes use the
 * same order. ancestor_old_maxes[i] is the true pre-mutation subtree maximum
 * of ancestor i; it is required only when that ancestor is full and must split.
 *
 * new_pages/new_page_nums provide caller-reserved scratch images/page numbers.
 * A full non-root ancestor consumes one new page. If propagation reaches a
 * full root, two more pages are consumed because the stable root page is
 * rewritten to point at two newly materialized internal children.
 *
 * The helper never publishes a partial cascade: every ancestor/new-page image
 * is first mutated in private scratch storage. Caller buffers are updated only
 * after the complete cascade succeeds. Pager-owned checksum trailers therefore
 * remain untouched on both success and failure.
 *
 * Descendant parent-pointer rewrites are intentionally left to the tree-level
 * publisher, which owns the referenced child pages. This helper is responsible
 * only for the internal page images and separator/child topology.
 */

static inline unsigned char* tinydb_cascade_page_at(void* pages,
                                                     size_t stride,
                                                     uint32_t index) {
    return (unsigned char*)pages + (size_t)index * stride;
}

static inline const unsigned char* tinydb_cascade_page_at_const(
    const void* pages,
    size_t stride,
    uint32_t index) {
    return (const unsigned char*)pages + (size_t)index * stride;
}

static inline bool tinydb_cascade_child_index(const unsigned char* parent,
                                               uint32_t child_page_num,
                                               uint32_t* index_out) {
    if (parent == NULL || index_out == NULL || child_page_num == 0u ||
        !tinydb_parent_stage_validate(parent, PAGE_SIZE)) {
        return false;
    }
    uint32_t keys = tinydb_parent_stage_read_u32(
        parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= keys; i++) {
        if (tinydb_parent_stage_child_at(parent, i) == child_page_num) {
            *index_out = i;
            return true;
        }
    }
    return false;
}

static inline bool tinydb_cascade_page_num_available(
    const uint32_t* ancestor_page_nums,
    uint32_t ancestor_count,
    const uint32_t* new_page_nums,
    uint32_t used_new_pages,
    uint32_t candidate,
    uint32_t split_left_child_page_num,
    uint32_t split_right_child_page_num) {
    if (candidate == 0u || candidate == INVALID_PAGE_NUM ||
        candidate == split_left_child_page_num ||
        candidate == split_right_child_page_num) {
        return false;
    }
    for (uint32_t i = 0u; i < ancestor_count; i++) {
        if (ancestor_page_nums[i] == candidate) return false;
    }
    for (uint32_t i = 0u; i < used_new_pages; i++) {
        if (new_page_nums[i] == candidate) return false;
    }
    return true;
}

static inline bool tinydb_stage_internal_split_cascade(
    void* ancestor_pages,
    size_t ancestor_stride,
    const uint32_t* ancestor_page_nums,
    const uint32_t* ancestor_old_maxes,
    uint32_t ancestor_count,
    void* new_pages,
    size_t new_page_stride,
    const uint32_t* new_page_nums,
    uint32_t new_page_capacity,
    uint32_t split_left_child_page_num,
    uint32_t split_right_child_page_num,
    uint32_t old_left_max,
    uint32_t new_left_max,
    uint32_t new_right_max,
    uint32_t* new_pages_used_out,
    uint32_t* stop_level_out,
    bool* root_grew_out) {
    if (new_pages_used_out != NULL) *new_pages_used_out = 0u;
    if (stop_level_out != NULL) *stop_level_out = 0u;
    if (root_grew_out != NULL) *root_grew_out = false;

    if (ancestor_pages == NULL || ancestor_page_nums == NULL ||
        ancestor_old_maxes == NULL || ancestor_count == 0u ||
        ancestor_stride < PAGE_SIZE || split_left_child_page_num == 0u ||
        split_right_child_page_num == 0u ||
        split_left_child_page_num == split_right_child_page_num ||
        old_left_max == 0u || new_left_max == 0u || new_right_max == 0u ||
        new_left_max >= new_right_max ||
        (new_page_capacity > 0u &&
         (new_pages == NULL || new_page_nums == NULL ||
          new_page_stride < PAGE_SIZE))) {
        return false;
    }

    /* ancestor_count/new_page_capacity are uint32_t and TinyDB's supported
     * build targets use 64-bit size_t, so PAGE_SIZE multiplication cannot
     * overflow size_t. malloc remains the authoritative allocation check. */
    unsigned char* scratch_ancestors =
        (unsigned char*)malloc((size_t)ancestor_count * PAGE_SIZE);
    unsigned char* scratch_new = new_page_capacity > 0u
        ? (unsigned char*)malloc((size_t)new_page_capacity * PAGE_SIZE)
        : NULL;
    if (scratch_ancestors == NULL ||
        (new_page_capacity > 0u && scratch_new == NULL)) {
        free(scratch_ancestors);
        free(scratch_new);
        return false;
    }

    for (uint32_t i = 0u; i < ancestor_count; i++) {
        const unsigned char* source = tinydb_cascade_page_at_const(
            ancestor_pages, ancestor_stride, i);
        memcpy(scratch_ancestors + (size_t)i * PAGE_SIZE,
               source,
               PAGE_SIZE);
        /* Page zero is TinyDB's normal catalog-stable root identity. It is
         * therefore valid only for the final ancestor when that image is the
         * root internal node itself. Zero remains forbidden for all other
         * ancestor identities and for newly allocated pages. */
        bool page_zero_root = ancestor_page_nums[i] == 0u &&
                              i + 1u == ancestor_count &&
                              source[NODE_TYPE_OFFSET] ==
                                  (unsigned char)NODE_INTERNAL &&
                              source[IS_ROOT_OFFSET] != 0u &&
                              tinydb_parent_stage_read_u32(
                                  source + PARENT_POINTER_OFFSET) == 0u;
        if (ancestor_page_nums[i] == INVALID_PAGE_NUM ||
            (ancestor_page_nums[i] == 0u && !page_zero_root)) {
            free(scratch_ancestors);
            free(scratch_new);
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (ancestor_page_nums[j] == ancestor_page_nums[i]) {
                free(scratch_ancestors);
                free(scratch_new);
                return false;
            }
        }
    }
    for (uint32_t i = 0u; i < new_page_capacity; i++) {
        const unsigned char* source = tinydb_cascade_page_at_const(
            new_pages, new_page_stride, i);
        memcpy(scratch_new + (size_t)i * PAGE_SIZE, source, PAGE_SIZE);
    }

    uint32_t used = 0u;
    uint32_t split_left = split_left_child_page_num;
    uint32_t split_right = split_right_child_page_num;
    uint32_t split_old_max = old_left_max;
    uint32_t split_new_left_max = new_left_max;
    uint32_t split_new_right_max = new_right_max;
    uint32_t stopped = 0u;
    bool root_grew = false;
    bool success = false;

    for (uint32_t level = 0u; level < ancestor_count; level++) {
        unsigned char* current =
            scratch_ancestors + (size_t)level * PAGE_SIZE;
        if (current[NODE_TYPE_OFFSET] != (unsigned char)NODE_INTERNAL ||
            !tinydb_parent_stage_validate(current, PAGE_SIZE)) {
            break;
        }

        uint32_t key_count = tinydb_parent_stage_read_u32(
            current + INTERNAL_NODE_NUM_KEYS_OFFSET);
        if (key_count == 0u || key_count > INTERNAL_NODE_MAX_KEYS) break;

        uint32_t child_index = 0u;
        if (!tinydb_cascade_child_index(current, split_left, &child_index)) {
            break;
        }
        for (uint32_t i = 0u; i <= key_count; i++) {
            if (tinydb_parent_stage_child_at(current, i) == split_right) {
                child_index = UINT32_MAX;
                break;
            }
        }
        if (child_index == UINT32_MAX) break;

        bool child_was_rightmost = child_index == key_count;
        bool is_root = current[IS_ROOT_OFFSET] != 0u;

        if (key_count < INTERNAL_NODE_MAX_KEYS) {
            if (!tinydb_slotted_leaf_v2_stage_parent_split(
                    current,
                    PAGE_SIZE,
                    split_left,
                    split_right,
                    split_old_max,
                    split_new_left_max,
                    split_new_right_max,
                    NULL)) {
                break;
            }
            stopped = level;
            success = true;
            break;
        }

        if (is_root) {
            if (used + 2u > new_page_capacity ||
                level + 1u != ancestor_count ||
                !tinydb_cascade_page_num_available(
                    ancestor_page_nums,
                    ancestor_count,
                    new_page_nums,
                    used,
                    new_page_nums[used],
                    split_left,
                    split_right) ||
                !tinydb_cascade_page_num_available(
                    ancestor_page_nums,
                    ancestor_count,
                    new_page_nums,
                    used + 1u,
                    new_page_nums[used + 1u],
                    split_left,
                    split_right)) {
                break;
            }

            unsigned char* root_left =
                scratch_new + (size_t)used * PAGE_SIZE;
            unsigned char* root_right =
                scratch_new + (size_t)(used + 1u) * PAGE_SIZE;
            if (!tinydb_stage_full_root_after_child_split(
                    current,
                    PAGE_SIZE,
                    ancestor_page_nums[level],
                    root_left,
                    PAGE_SIZE,
                    new_page_nums[used],
                    root_right,
                    PAGE_SIZE,
                    new_page_nums[used + 1u],
                    split_left,
                    split_right,
                    split_old_max,
                    split_new_left_max,
                    split_new_right_max,
                    NULL)) {
                break;
            }
            used += 2u;
            stopped = level;
            root_grew = true;
            success = true;
            break;
        }

        if (level + 1u >= ancestor_count ||
            tinydb_parent_stage_read_u32(current + PARENT_POINTER_OFFSET) !=
                ancestor_page_nums[level + 1u] ||
            ancestor_old_maxes[level] == 0u || used >= new_page_capacity ||
            !tinydb_cascade_page_num_available(
                ancestor_page_nums,
                ancestor_count,
                new_page_nums,
                used,
                new_page_nums[used],
                split_left,
                split_right)) {
            break;
        }

        unsigned char* new_right_internal =
            scratch_new + (size_t)used * PAGE_SIZE;
        uint32_t promoted_left_max = 0u;
        if (!tinydb_stage_full_nonroot_after_child_split(
                current,
                PAGE_SIZE,
                ancestor_page_nums[level],
                new_right_internal,
                PAGE_SIZE,
                new_page_nums[used],
                split_left,
                split_right,
                split_old_max,
                split_new_left_max,
                split_new_right_max,
                &promoted_left_max,
                NULL)) {
            break;
        }

        uint32_t old_current_max = ancestor_old_maxes[level];
        uint32_t new_current_max = child_was_rightmost
            ? split_new_right_max
            : old_current_max;
        if (promoted_left_max == 0u || new_current_max == 0u ||
            promoted_left_max >= new_current_max) {
            break;
        }

        split_left = ancestor_page_nums[level];
        split_right = new_page_nums[used];
        split_old_max = old_current_max;
        split_new_left_max = promoted_left_max;
        split_new_right_max = new_current_max;
        used++;
    }

    if (success) {
        for (uint32_t i = 0u; i < ancestor_count; i++) {
            unsigned char* destination = tinydb_cascade_page_at(
                ancestor_pages, ancestor_stride, i);
            memcpy(destination,
                   scratch_ancestors + (size_t)i * PAGE_SIZE,
                   PAGE_USABLE_SIZE);
        }
        for (uint32_t i = 0u; i < used; i++) {
            unsigned char* destination = tinydb_cascade_page_at(
                new_pages, new_page_stride, i);
            memcpy(destination,
                   scratch_new + (size_t)i * PAGE_SIZE,
                   PAGE_USABLE_SIZE);
        }
        if (new_pages_used_out != NULL) *new_pages_used_out = used;
        if (stop_level_out != NULL) *stop_level_out = stopped;
        if (root_grew_out != NULL) *root_grew_out = root_grew;
    }

    free(scratch_ancestors);
    free(scratch_new);
    return success;
}

#endif /* INTERNAL_SPLIT_CASCADE_STAGE_H */
