#ifndef SLOTTED_LEAF_V2_PARENT_STAGE_H
#define SLOTTED_LEAF_V2_PARENT_STAGE_H

#include "table.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Stage the parent-page portion of a non-root slotted-leaf split.
 *
 * This helper is deliberately page-local and allocation-free from the B+ tree
 * point of view: it never fetches pages or marks them dirty.  The caller gives
 * it the already-split left/right leaf identities and key bounds.  We rebuild
 * the parent in scratch memory, validate the resulting child/separator layout,
 * and publish PAGE_USABLE_SIZE only after the complete parent mutation is
 * known to be valid.  Pager-owned checksum bytes are therefore untouched.
 *
 * Parent representation follows TinyDB's existing invariant:
 *   - every non-rightmost child is stored in a cell together with that child's
 *     maximum key;
 *   - the rightmost child is stored in INTERNAL_NODE_RIGHT_CHILD and has no
 *     separator entry.
 *
 * For an interior child split the original parent separator must equal
 * old_left_max and the new right half must preserve that upper boundary.  This
 * fail-closed restriction prevents a staged split from silently crossing into
 * the next child's key range.  A split of the parent's rightmost child may
 * grow the overall upper boundary because no following sibling range exists.
 *
 * Parent overflow is intentionally rejected.  Recursive internal-node split
 * remains a later tree-level operation that must be coordinated with Pager/WAL
 * allocation and recovery.
 */

static inline uint32_t tinydb_parent_stage_read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static inline void tinydb_parent_stage_write_u32(unsigned char* p,
                                                  uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static inline unsigned char* tinydb_parent_stage_cell(unsigned char* page,
                                                       uint32_t index) {
    return page + INTERNAL_NODE_HEADER_SIZE +
           index * INTERNAL_NODE_CELL_SIZE;
}

static inline const unsigned char* tinydb_parent_stage_cell_const(
    const unsigned char* page,
    uint32_t index) {
    return page + INTERNAL_NODE_HEADER_SIZE +
           index * INTERNAL_NODE_CELL_SIZE;
}

static inline uint32_t tinydb_parent_stage_child_at(const unsigned char* page,
                                                     uint32_t index) {
    uint32_t num_keys = tinydb_parent_stage_read_u32(
        page + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (index == num_keys) {
        return tinydb_parent_stage_read_u32(
            page + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
    }
    return tinydb_parent_stage_read_u32(
        tinydb_parent_stage_cell_const(page, index));
}

static inline uint32_t tinydb_parent_stage_key_at(const unsigned char* page,
                                                   uint32_t index) {
    return tinydb_parent_stage_read_u32(
        tinydb_parent_stage_cell_const(page, index) + INTERNAL_NODE_CHILD_SIZE);
}

static inline bool tinydb_parent_stage_validate(const unsigned char* page,
                                                 size_t page_capacity) {
    if (page == NULL || page_capacity < PAGE_SIZE ||
        page[NODE_TYPE_OFFSET] != (unsigned char)NODE_INTERNAL) {
        return false;
    }

    uint32_t num_keys = tinydb_parent_stage_read_u32(
        page + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (num_keys == 0u || num_keys > INTERNAL_NODE_MAX_KEYS) return false;

    uint32_t previous_key = 0u;
    for (uint32_t i = 0u; i < num_keys; i++) {
        uint32_t child = tinydb_parent_stage_child_at(page, i);
        uint32_t key = tinydb_parent_stage_key_at(page, i);
        if (child == 0u || (i > 0u && key <= previous_key)) return false;
        previous_key = key;

        for (uint32_t j = 0u; j < i; j++) {
            if (child == tinydb_parent_stage_child_at(page, j)) return false;
        }
    }

    uint32_t right_child = tinydb_parent_stage_child_at(page, num_keys);
    if (right_child == 0u) return false;
    for (uint32_t i = 0u; i < num_keys; i++) {
        if (right_child == tinydb_parent_stage_child_at(page, i)) return false;
    }
    return true;
}

static inline bool tinydb_slotted_leaf_v2_stage_parent_split(
    void* parent_page,
    size_t parent_capacity,
    uint32_t left_page_num,
    uint32_t right_page_num,
    uint32_t old_left_max,
    uint32_t new_left_max,
    uint32_t new_right_max,
    uint32_t* inserted_child_index_out) {
    if (parent_page == NULL || parent_capacity < PAGE_SIZE ||
        left_page_num == 0u || right_page_num == 0u ||
        left_page_num == right_page_num || new_left_max >= new_right_max ||
        !tinydb_parent_stage_validate((const unsigned char*)parent_page,
                                      parent_capacity)) {
        return false;
    }

    const unsigned char* original = (const unsigned char*)parent_page;
    uint32_t num_keys = tinydb_parent_stage_read_u32(
        original + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (num_keys >= INTERNAL_NODE_MAX_KEYS) return false;

    uint32_t left_index = num_keys + 1u;
    for (uint32_t i = 0u; i <= num_keys; i++) {
        uint32_t child = tinydb_parent_stage_child_at(original, i);
        if (child == right_page_num) return false;
        if (child == left_page_num) {
            if (left_index != num_keys + 1u) return false;
            left_index = i;
        }
    }
    if (left_index == num_keys + 1u) return false;

    bool left_was_rightmost = left_index == num_keys;
    if (!left_was_rightmost) {
        if (tinydb_parent_stage_key_at(original, left_index) != old_left_max ||
            new_right_max != old_left_max) {
            return false;
        }
    }

    unsigned char* scratch = (unsigned char*)malloc(PAGE_SIZE);
    if (scratch == NULL) return false;
    memcpy(scratch, original, PAGE_SIZE);

    bool ok = true;
    uint32_t inserted_index = left_index + 1u;
    if (left_was_rightmost) {
        unsigned char* new_cell = tinydb_parent_stage_cell(scratch, num_keys);
        tinydb_parent_stage_write_u32(new_cell, left_page_num);
        tinydb_parent_stage_write_u32(new_cell + INTERNAL_NODE_CHILD_SIZE,
                                      new_left_max);
        tinydb_parent_stage_write_u32(
            scratch + INTERNAL_NODE_RIGHT_CHILD_OFFSET, right_page_num);
        inserted_index = num_keys + 1u;
    } else {
        for (uint32_t i = num_keys; i > left_index + 1u; i--) {
            memcpy(tinydb_parent_stage_cell(scratch, i),
                   tinydb_parent_stage_cell_const(scratch, i - 1u),
                   INTERNAL_NODE_CELL_SIZE);
        }

        tinydb_parent_stage_write_u32(
            tinydb_parent_stage_cell(scratch, left_index) +
                INTERNAL_NODE_CHILD_SIZE,
            new_left_max);
        unsigned char* right_cell =
            tinydb_parent_stage_cell(scratch, left_index + 1u);
        tinydb_parent_stage_write_u32(right_cell, right_page_num);
        tinydb_parent_stage_write_u32(right_cell + INTERNAL_NODE_CHILD_SIZE,
                                      new_right_max);
    }

    tinydb_parent_stage_write_u32(scratch + INTERNAL_NODE_NUM_KEYS_OFFSET,
                                  num_keys + 1u);

    if (ok) {
        ok = tinydb_parent_stage_validate(scratch, PAGE_SIZE) &&
             tinydb_parent_stage_child_at(scratch, left_index) == left_page_num &&
             tinydb_parent_stage_key_at(scratch, left_index) == new_left_max &&
             tinydb_parent_stage_child_at(scratch, inserted_index) ==
                 right_page_num;
    }

    if (ok && !left_was_rightmost) {
        ok = tinydb_parent_stage_key_at(scratch, inserted_index) ==
             new_right_max;
    }

    if (ok) {
        memcpy(parent_page, scratch, PAGE_USABLE_SIZE);
        if (inserted_child_index_out != NULL) {
            *inserted_child_index_out = inserted_index;
        }
    }

    free(scratch);
    return ok;
}

#endif /* SLOTTED_LEAF_V2_PARENT_STAGE_H */
