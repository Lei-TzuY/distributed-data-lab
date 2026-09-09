#ifndef SLOTTED_LEAF_V2_SPLIT_H
#define SLOTTED_LEAF_V2_SPLIT_H

#include "slotted_leaf_v2.h"

#include <stdlib.h>
#include <string.h>

/*
 * Split an existing non-root V2 leaf into two V2 leaves without changing any
 * caller-visible bytes unless the complete redistribution succeeds.
 *
 * The split point minimizes the difference in physical bytes used by each
 * side (slot directory bytes plus payload bytes), rather than merely halving
 * the row count.  This matters once VARCHAR payloads become truly variable
 * length.
 *
 * Tree-level parent separator insertion and old-next-sibling backlink repair
 * remain the caller's responsibility.  This primitive prepares the local
 * sibling chain as:
 *
 *   old_prev <-> left(left_page_num) <-> right(right_page_num) -> old_next
 *
 * Root leaves are deliberately rejected because splitting a root also
 * requires creating an internal root atomically.
 */

static inline uint16_t tinydb_slotted_split_read_u16(const unsigned char* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t tinydb_slotted_split_read_u32(const unsigned char* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void tinydb_slotted_split_write_u32(unsigned char* p,
                                                   uint32_t value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static inline const unsigned char* tinydb_slotted_split_slot(
    const void* page,
    uint16_t index) {
    return (const unsigned char*)page + TINYDB_SLOTTED_V2_HEADER_SIZE +
           (uint32_t)index * TINYDB_SLOTTED_V2_SLOT_SIZE;
}

static inline uint32_t tinydb_slotted_split_slot_key(const void* page,
                                                      uint16_t index) {
    const unsigned char* slot = tinydb_slotted_split_slot(page, index);
    return tinydb_slotted_split_read_u32(
        slot + TINYDB_SLOTTED_V2_SLOT_KEY_OFFSET);
}

static inline uint16_t tinydb_slotted_split_slot_offset(const void* page,
                                                         uint16_t index) {
    const unsigned char* slot = tinydb_slotted_split_slot(page, index);
    return tinydb_slotted_split_read_u16(
        slot + TINYDB_SLOTTED_V2_SLOT_VALUE_OFFSET);
}

static inline uint16_t tinydb_slotted_split_slot_length(const void* page,
                                                         uint16_t index) {
    const unsigned char* slot = tinydb_slotted_split_slot(page, index);
    return tinydb_slotted_split_read_u16(
        slot + TINYDB_SLOTTED_V2_SLOT_LENGTH_OFFSET);
}

static inline uint16_t tinydb_slotted_leaf_v2_choose_split_index(
    const void* page,
    size_t page_capacity) {
    if (!tinydb_slotted_leaf_v2_validate(page, page_capacity)) return 0u;

    uint16_t count = tinydb_slotted_leaf_v2_count(page, page_capacity);
    if (count < 2u) return 0u;

    uint32_t total = 0u;
    for (uint16_t i = 0u; i < count; i++) {
        total += TINYDB_SLOTTED_V2_SLOT_SIZE +
                 tinydb_slotted_split_slot_length(page, i);
    }

    uint16_t best_index = 1u;
    uint32_t prefix = 0u;
    uint32_t best_delta = UINT32_MAX;
    for (uint16_t i = 1u; i < count; i++) {
        prefix += TINYDB_SLOTTED_V2_SLOT_SIZE +
                  tinydb_slotted_split_slot_length(page, (uint16_t)(i - 1u));
        uint32_t suffix = total - prefix;
        uint32_t delta = prefix > suffix ? prefix - suffix : suffix - prefix;
        if (delta < best_delta) {
            best_delta = delta;
            best_index = i;
        }
    }
    return best_index;
}

static inline void tinydb_slotted_split_copy_common_identity(
    unsigned char* destination,
    const unsigned char* source) {
    destination[IS_ROOT_OFFSET] = 0u;
    memcpy(destination + PARENT_POINTER_OFFSET,
           source + PARENT_POINTER_OFFSET,
           PARENT_POINTER_SIZE);
    memcpy(destination + TINYDB_SLOTTED_V2_FLAGS_OFFSET,
           source + TINYDB_SLOTTED_V2_FLAGS_OFFSET,
           TINYDB_SLOTTED_V2_FLAGS_SIZE);
}

static inline bool tinydb_slotted_leaf_v2_split_nonroot(
    void* left_page,
    size_t left_capacity,
    uint32_t left_page_num,
    void* right_page,
    size_t right_capacity,
    uint32_t right_page_num,
    uint16_t* split_index_out) {
    if (left_page == NULL || right_page == NULL || left_page == right_page ||
        left_capacity < PAGE_SIZE || right_capacity < PAGE_SIZE ||
        left_page_num == right_page_num ||
        !tinydb_slotted_leaf_v2_validate(left_page, left_capacity) ||
        ((const unsigned char*)left_page)[IS_ROOT_OFFSET] != 0u) {
        return false;
    }

    uint16_t split_index =
        tinydb_slotted_leaf_v2_choose_split_index(left_page, left_capacity);
    uint16_t count = tinydb_slotted_leaf_v2_count(left_page, left_capacity);
    if (split_index == 0u || split_index >= count) return false;

    unsigned char* scratch_left = (unsigned char*)malloc(PAGE_SIZE);
    unsigned char* scratch_right = (unsigned char*)malloc(PAGE_SIZE);
    if (scratch_left == NULL || scratch_right == NULL) {
        free(scratch_left);
        free(scratch_right);
        return false;
    }
    memset(scratch_left, 0, PAGE_SIZE);
    memset(scratch_right, 0, PAGE_SIZE);

    bool ok = tinydb_slotted_leaf_v2_init(scratch_left, PAGE_SIZE) &&
              tinydb_slotted_leaf_v2_init(scratch_right, PAGE_SIZE);
    const unsigned char* source = (const unsigned char*)left_page;
    if (ok) {
        tinydb_slotted_split_copy_common_identity(scratch_left, source);
        tinydb_slotted_split_copy_common_identity(scratch_right, source);

        memcpy(scratch_left + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
               source + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
               TINYDB_SLOTTED_V2_PREV_LEAF_SIZE);
        tinydb_slotted_split_write_u32(
            scratch_left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
            right_page_num);
        tinydb_slotted_split_write_u32(
            scratch_right + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            left_page_num);
        memcpy(scratch_right + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
               source + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
               TINYDB_SLOTTED_V2_NEXT_LEAF_SIZE);
    }

    for (uint16_t i = 0u; i < count && ok; i++) {
        uint32_t key = tinydb_slotted_split_slot_key(source, i);
        uint16_t offset = tinydb_slotted_split_slot_offset(source, i);
        uint16_t length = tinydb_slotted_split_slot_length(source, i);
        void* destination = i < split_index ? (void*)scratch_left
                                            : (void*)scratch_right;
        ok = tinydb_slotted_leaf_v2_insert(destination,
                                           PAGE_SIZE,
                                           key,
                                           source + offset,
                                           length);
    }

    if (ok) {
        ok = tinydb_slotted_leaf_v2_validate(scratch_left, PAGE_SIZE) &&
             tinydb_slotted_leaf_v2_validate(scratch_right, PAGE_SIZE) &&
             tinydb_slotted_leaf_v2_count(scratch_left, PAGE_SIZE) > 0u &&
             tinydb_slotted_leaf_v2_count(scratch_right, PAGE_SIZE) > 0u;
    }

    if (ok) {
        /* Leave both Pager-owned checksum trailers byte-for-byte untouched. */
        memcpy(left_page, scratch_left, PAGE_USABLE_SIZE);
        memcpy(right_page, scratch_right, PAGE_USABLE_SIZE);
        if (split_index_out != NULL) *split_index_out = split_index;
    }

    free(scratch_left);
    free(scratch_right);
    return ok;
}

#endif /* SLOTTED_LEAF_V2_SPLIT_H */
