#ifndef TINYDB_LEAF_SIBLING_RELINK_STAGE_H
#define TINYDB_LEAF_SIBLING_RELINK_STAGE_H

#include "leaf_format.h"
#include "leaf_page_access.h"
#include "slotted_leaf_v2.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Stage one sibling-pointer replacement on either a legacy fixed V1 leaf or a
 * slotted V2 leaf. The caller supplies the expected old sibling so stale or
 * inconsistent topology fails closed. A zero new sibling is valid and means
 * list boundary; INVALID_PAGE_NUM is never a valid link.
 *
 * Only PAGE_USABLE_SIZE is published after the staged image re-validates, so
 * the Pager-owned checksum trailer is preserved byte-for-byte. On any failure
 * the caller image is untouched.
 */
static inline void tinydb_leaf_relink_write_u32_le(unsigned char* p,
                                                    uint32_t value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static inline bool tinydb_stage_leaf_sibling_relink(
    void* leaf_page,
    size_t leaf_capacity,
    bool replace_next,
    uint32_t expected_old_sibling,
    uint32_t new_sibling) {
    if (leaf_page == NULL || leaf_capacity < PAGE_SIZE ||
        expected_old_sibling == 0u ||
        expected_old_sibling == INVALID_PAGE_NUM ||
        new_sibling == INVALID_PAGE_NUM ||
        new_sibling == expected_old_sibling) {
        return false;
    }

    TinyDBLeafPageFormat format =
        tinydb_leaf_format_detect_page(leaf_page, leaf_capacity);
    if (format != TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 &&
        format != TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return false;
    }

    uint32_t current = 0u;
    bool read_ok = replace_next
        ? tinydb_leaf_page_next(leaf_page, leaf_capacity, &current)
        : tinydb_leaf_page_prev(leaf_page, leaf_capacity, &current);
    if (!read_ok || current != expected_old_sibling) return false;

    unsigned char scratch[PAGE_SIZE];
    memcpy(scratch, leaf_page, PAGE_SIZE);
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        size_t offset = replace_next ? LEAF_NODE_NEXT_LEAF_OFFSET
                                     : LEAF_NODE_PREV_LEAF_OFFSET;
        memcpy(scratch + offset, &new_sibling, sizeof(new_sibling));
    } else {
        size_t offset = replace_next ? TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET
                                     : TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET;
        tinydb_leaf_relink_write_u32_le(scratch + offset, new_sibling);
    }

    TinyDBLeafPageFormat staged_format =
        tinydb_leaf_format_detect_page(scratch, PAGE_SIZE);
    if (staged_format != format) return false;

    uint32_t checked = INVALID_PAGE_NUM;
    bool checked_ok = replace_next
        ? tinydb_leaf_page_next(scratch, PAGE_SIZE, &checked)
        : tinydb_leaf_page_prev(scratch, PAGE_SIZE, &checked);
    if (!checked_ok || checked != new_sibling) return false;

    memcpy(leaf_page, scratch, PAGE_USABLE_SIZE);
    return true;
}

#endif
