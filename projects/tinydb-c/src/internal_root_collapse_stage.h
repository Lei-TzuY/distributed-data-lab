#ifndef TINYDB_INTERNAL_ROOT_COLLAPSE_STAGE_H
#define TINYDB_INTERNAL_ROOT_COLLAPSE_STAGE_H

#include "leaf_page_access.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Stage the height-two -> height-one collapse of a V2 tree without touching
 * Pager allocation state.
 *
 * The input root must be a valid one-key internal root with exactly two leaf
 * children. One child has already become empty and is being removed by the
 * caller. The surviving child image must already describe the only remaining
 * leaf in the chain (prev = next = 0), still be non-root, and still point at
 * the stable root page as its parent.
 *
 * TinyDB keeps catalog root page numbers stable, so collapse copies the
 * surviving V2 leaf image into the existing root page rather than changing the
 * schema's root_page_num. The surviving child page itself is deliberately not
 * modified; after an atomic Pager/WAL publication the caller may reclaim both
 * the removed empty page and the promoted child page.
 *
 * Only PAGE_USABLE_SIZE is copied on success. Pager-owned checksum trailer
 * bytes in the stable root page are preserved. Every validation happens in
 * scratch memory, so failure leaves both caller images byte-for-byte unchanged.
 */
static inline bool tinydb_stage_internal_root_collapse_to_v2_leaf(
    void* root_page,
    size_t root_capacity,
    uint32_t root_page_num,
    const void* surviving_leaf_page,
    size_t surviving_capacity,
    uint32_t surviving_leaf_page_num,
    uint32_t removed_child_page_num,
    uint32_t removed_child_max,
    uint32_t* promoted_page_num_out) {
    if (promoted_page_num_out != NULL) {
        *promoted_page_num_out = INVALID_PAGE_NUM;
    }
    if (root_page == NULL || surviving_leaf_page == NULL ||
        root_capacity < PAGE_SIZE || surviving_capacity < PAGE_SIZE ||
        surviving_leaf_page_num == 0u ||
        surviving_leaf_page_num == INVALID_PAGE_NUM ||
        removed_child_page_num == 0u ||
        removed_child_page_num == INVALID_PAGE_NUM ||
        surviving_leaf_page_num == removed_child_page_num ||
        surviving_leaf_page_num == root_page_num ||
        removed_child_page_num == root_page_num) {
        return false;
    }

    const unsigned char* original_root = (const unsigned char*)root_page;
    const unsigned char* surviving =
        (const unsigned char*)surviving_leaf_page;
    if (original_root[NODE_TYPE_OFFSET] != (unsigned char)NODE_INTERNAL ||
        original_root[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(original_root + PARENT_POINTER_OFFSET) !=
            0u ||
        !tinydb_parent_stage_validate(original_root, root_capacity) ||
        tinydb_parent_stage_read_u32(
            original_root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_slotted_leaf_v2_validate(surviving, surviving_capacity) ||
        surviving[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(surviving + PARENT_POINTER_OFFSET) !=
            root_page_num) {
        return false;
    }

    uint32_t previous_page_num = INVALID_PAGE_NUM;
    uint32_t next_page_num = INVALID_PAGE_NUM;
    uint32_t survivor_count = 0u;
    if (!tinydb_leaf_page_prev(surviving,
                               surviving_capacity,
                               &previous_page_num) ||
        !tinydb_leaf_page_next(surviving,
                               surviving_capacity,
                               &next_page_num) ||
        previous_page_num != 0u || next_page_num != 0u ||
        !tinydb_leaf_page_count(surviving,
                                surviving_capacity,
                                &survivor_count) ||
        survivor_count == 0u) {
        return false;
    }

    uint32_t survivor_min = 0u;
    uint32_t survivor_max = 0u;
    if (!tinydb_leaf_page_key_at(surviving,
                                 surviving_capacity,
                                 0u,
                                 &survivor_min) ||
        !tinydb_leaf_page_key_at(surviving,
                                 surviving_capacity,
                                 survivor_count - 1u,
                                 &survivor_max) ||
        survivor_min > survivor_max) {
        return false;
    }

    uint32_t left_child = tinydb_parent_stage_child_at(original_root, 0u);
    uint32_t right_child = tinydb_parent_stage_child_at(original_root, 1u);
    uint32_t separator = tinydb_parent_stage_key_at(original_root, 0u);
    bool removed_left = left_child == removed_child_page_num &&
                        right_child == surviving_leaf_page_num;
    bool removed_right = right_child == removed_child_page_num &&
                         left_child == surviving_leaf_page_num;
    if (removed_left == removed_right) return false;

    if (removed_left) {
        if (separator != removed_child_max ||
            removed_child_max >= survivor_min) {
            return false;
        }
    } else {
        if (separator != survivor_max || removed_child_max <= survivor_max) {
            return false;
        }
    }

    unsigned char scratch[PAGE_SIZE];
    memcpy(scratch, original_root, PAGE_SIZE);
    memcpy(scratch, surviving, PAGE_USABLE_SIZE);
    scratch[IS_ROOT_OFFSET] = 1u;
    tinydb_parent_stage_write_u32(scratch + PARENT_POINTER_OFFSET, 0u);

    if (!tinydb_slotted_leaf_v2_validate(scratch, PAGE_SIZE) ||
        scratch[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(scratch + PARENT_POINTER_OFFSET) != 0u) {
        return false;
    }

    uint32_t staged_previous = INVALID_PAGE_NUM;
    uint32_t staged_next = INVALID_PAGE_NUM;
    uint32_t staged_count = 0u;
    uint32_t staged_min = 0u;
    uint32_t staged_max = 0u;
    if (!tinydb_leaf_page_prev(scratch, PAGE_SIZE, &staged_previous) ||
        !tinydb_leaf_page_next(scratch, PAGE_SIZE, &staged_next) ||
        staged_previous != 0u || staged_next != 0u ||
        !tinydb_leaf_page_count(scratch, PAGE_SIZE, &staged_count) ||
        staged_count != survivor_count ||
        !tinydb_leaf_page_key_at(scratch, PAGE_SIZE, 0u, &staged_min) ||
        !tinydb_leaf_page_key_at(scratch,
                                 PAGE_SIZE,
                                 staged_count - 1u,
                                 &staged_max) ||
        staged_min != survivor_min || staged_max != survivor_max) {
        return false;
    }

    memcpy(root_page, scratch, PAGE_USABLE_SIZE);
    if (promoted_page_num_out != NULL) {
        *promoted_page_num_out = surviving_leaf_page_num;
    }
    return true;
}

/* Validate one direct child of an internal node before/after a parent-pointer
 * rewrite. The common-header parent pointer is shared by internal nodes, fixed
 * V1 leaves, and slotted V2 leaves. */
static inline bool tinydb_root_collapse_direct_child_valid(
    const unsigned char* page,
    size_t page_capacity,
    uint32_t expected_parent_page_num) {
    if (page == NULL || page_capacity < PAGE_SIZE ||
        page[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(page + PARENT_POINTER_OFFSET) !=
            expected_parent_page_num) {
        return false;
    }

    NodeType type = get_node_type((void*)page);
    if (type == NODE_INTERNAL) {
        return tinydb_parent_stage_validate(page, page_capacity);
    }
    if (type != NODE_LEAF) return false;

    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page,
                                                                 page_capacity);
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return tinydb_slotted_leaf_v2_validate(page, page_capacity);
    }
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        uint32_t count = 0u;
        return tinydb_leaf_page_is_fixed_v1(page, page_capacity) &&
               tinydb_leaf_page_count(page, page_capacity, &count) &&
               count > 0u;
    }
    return false;
}

/*
 * Stage root contraction when the surviving child is itself an internal node.
 *
 * Copying that internal node into the stable root page is not sufficient: all
 * of its direct children still name the old internal page as their parent. The
 * caller therefore supplies every direct child image and page number in exact
 * child order. This helper stages the promoted root and all parent-pointer
 * rewrites in scratch storage, validates the complete image set, and only then
 * publishes PAGE_USABLE_SIZE into the caller buffers.
 *
 * The surviving internal source image is immutable. A future live Pager/WAL
 * route may reclaim its old page only after atomically publishing the staged
 * root and descendant images. No allocator state is touched here.
 */
static inline bool tinydb_stage_internal_root_collapse_to_internal(
    void* root_page,
    size_t root_capacity,
    uint32_t root_page_num,
    const void* surviving_internal_page,
    size_t surviving_capacity,
    uint32_t surviving_internal_page_num,
    uint32_t surviving_subtree_max,
    uint32_t removed_child_page_num,
    uint32_t removed_child_max,
    void* const* direct_child_pages,
    const uint32_t* direct_child_page_nums,
    uint32_t direct_child_count,
    uint32_t* promoted_page_num_out) {
    if (promoted_page_num_out != NULL) {
        *promoted_page_num_out = INVALID_PAGE_NUM;
    }
    if (root_page == NULL || surviving_internal_page == NULL ||
        direct_child_pages == NULL || direct_child_page_nums == NULL ||
        root_capacity < PAGE_SIZE || surviving_capacity < PAGE_SIZE ||
        surviving_internal_page_num == 0u ||
        surviving_internal_page_num == INVALID_PAGE_NUM ||
        removed_child_page_num == 0u ||
        removed_child_page_num == INVALID_PAGE_NUM ||
        surviving_internal_page_num == removed_child_page_num ||
        surviving_internal_page_num == root_page_num ||
        removed_child_page_num == root_page_num) {
        return false;
    }

    const unsigned char* original_root = (const unsigned char*)root_page;
    const unsigned char* surviving =
        (const unsigned char*)surviving_internal_page;
    if (!tinydb_parent_stage_validate(original_root, root_capacity) ||
        original_root[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(original_root + PARENT_POINTER_OFFSET) !=
            0u ||
        tinydb_parent_stage_read_u32(
            original_root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_parent_stage_validate(surviving, surviving_capacity) ||
        surviving[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(surviving + PARENT_POINTER_OFFSET) !=
            root_page_num) {
        return false;
    }

    uint32_t survivor_key_count = tinydb_parent_stage_read_u32(
        surviving + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (survivor_key_count == 0u ||
        survivor_key_count > INTERNAL_NODE_MAX_KEYS ||
        direct_child_count != survivor_key_count + 1u ||
        direct_child_count == 0u ||
        direct_child_count > INTERNAL_NODE_MAX_KEYS + 1u) {
        return false;
    }

    uint32_t root_left = tinydb_parent_stage_child_at(original_root, 0u);
    uint32_t root_right = tinydb_parent_stage_child_at(original_root, 1u);
    uint32_t root_separator = tinydb_parent_stage_key_at(original_root, 0u);
    bool removed_left = root_left == removed_child_page_num &&
                        root_right == surviving_internal_page_num;
    bool removed_right = root_right == removed_child_page_num &&
                         root_left == surviving_internal_page_num;
    if (removed_left == removed_right) return false;
    if ((removed_left &&
         (root_separator != removed_child_max ||
          removed_child_max >= surviving_subtree_max)) ||
        (removed_right &&
         (root_separator != surviving_subtree_max ||
          removed_child_max <= surviving_subtree_max))) {
        return false;
    }

    for (uint32_t i = 0u; i < direct_child_count; i++) {
        if (direct_child_pages[i] == NULL ||
            direct_child_pages[i] == root_page ||
            direct_child_pages[i] == surviving_internal_page ||
            direct_child_page_nums[i] == 0u ||
            direct_child_page_nums[i] == INVALID_PAGE_NUM ||
            direct_child_page_nums[i] == root_page_num ||
            direct_child_page_nums[i] == surviving_internal_page_num ||
            direct_child_page_nums[i] == removed_child_page_num ||
            direct_child_page_nums[i] !=
                tinydb_parent_stage_child_at(surviving, i) ||
            !tinydb_root_collapse_direct_child_valid(
                (const unsigned char*)direct_child_pages[i],
                PAGE_SIZE,
                surviving_internal_page_num)) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (direct_child_page_nums[i] == direct_child_page_nums[j] ||
                direct_child_pages[i] == direct_child_pages[j]) {
                return false;
            }
        }
    }

    if (direct_child_count != 0u &&
        (size_t)PAGE_SIZE > SIZE_MAX / (size_t)direct_child_count) {
        return false;
    }
    size_t scratch_bytes = (size_t)direct_child_count * (size_t)PAGE_SIZE;
    unsigned char* child_scratch = (unsigned char*)malloc(scratch_bytes);
    if (child_scratch == NULL) return false;

    unsigned char root_scratch[PAGE_SIZE];
    memcpy(root_scratch, original_root, PAGE_SIZE);
    memcpy(root_scratch, surviving, PAGE_USABLE_SIZE);
    root_scratch[IS_ROOT_OFFSET] = 1u;
    tinydb_parent_stage_write_u32(root_scratch + PARENT_POINTER_OFFSET, 0u);

    bool ok = tinydb_parent_stage_validate(root_scratch, PAGE_SIZE) &&
              root_scratch[IS_ROOT_OFFSET] != 0u &&
              tinydb_parent_stage_read_u32(
                  root_scratch + PARENT_POINTER_OFFSET) == 0u &&
              tinydb_parent_stage_read_u32(
                  root_scratch + INTERNAL_NODE_NUM_KEYS_OFFSET) ==
                  survivor_key_count;

    for (uint32_t i = 0u; ok && i < direct_child_count; i++) {
        unsigned char* staged_child =
            child_scratch + (size_t)i * PAGE_SIZE;
        memcpy(staged_child, direct_child_pages[i], PAGE_SIZE);
        tinydb_parent_stage_write_u32(staged_child + PARENT_POINTER_OFFSET,
                                      root_page_num);
        ok = tinydb_root_collapse_direct_child_valid(staged_child,
                                                     PAGE_SIZE,
                                                     root_page_num) &&
             tinydb_parent_stage_child_at(root_scratch, i) ==
                 direct_child_page_nums[i];
    }

    if (ok) {
        memcpy(root_page, root_scratch, PAGE_USABLE_SIZE);
        for (uint32_t i = 0u; i < direct_child_count; i++) {
            memcpy(direct_child_pages[i],
                   child_scratch + (size_t)i * PAGE_SIZE,
                   PAGE_USABLE_SIZE);
        }
        if (promoted_page_num_out != NULL) {
            *promoted_page_num_out = surviving_internal_page_num;
        }
    }

    free(child_scratch);
    return ok;
}

#endif /* TINYDB_INTERNAL_ROOT_COLLAPSE_STAGE_H */
