#include "leaf_page_access.h"

#include "slotted_leaf_v2.h"

#include <string.h>

static uint16_t read_u16_le(const unsigned char* bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32_le(const unsigned char* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool v1_count(const void* page, uint32_t* count) {
    uint32_t value = 0u;
    memcpy(&value,
           (const unsigned char*)page + LEAF_NODE_NUM_CELLS_OFFSET,
           sizeof(value));
    if (value > LEAF_NODE_MAX_CELLS) return false;
    if (count != NULL) *count = value;
    return true;
}

static const unsigned char* v1_cell(const void* page, uint32_t cell_index) {
    return (const unsigned char*)page + LEAF_NODE_HEADER_SIZE +
           cell_index * LEAF_NODE_CELL_SIZE;
}

static bool v2_slot_at(const void* page,
                       size_t page_capacity,
                       uint32_t cell_index,
                       TinyDBSlottedLeafV2Slot* slot) {
    if (!tinydb_slotted_leaf_v2_validate(page, page_capacity)) return false;
    uint16_t count = tinydb_slotted_leaf_v2_count(page, page_capacity);
    if (cell_index >= count || cell_index > UINT16_MAX) return false;

    const unsigned char* bytes = (const unsigned char*)page +
        TINYDB_SLOTTED_V2_HEADER_SIZE +
        cell_index * TINYDB_SLOTTED_V2_SLOT_SIZE;
    TinyDBSlottedLeafV2Slot current;
    current.key = read_u32_le(bytes + TINYDB_SLOTTED_V2_SLOT_KEY_OFFSET);
    current.value_offset = read_u16_le(
        bytes + TINYDB_SLOTTED_V2_SLOT_VALUE_OFFSET);
    current.value_length = read_u16_le(
        bytes + TINYDB_SLOTTED_V2_SLOT_LENGTH_OFFSET);
    if (slot != NULL) *slot = current;
    return true;
}

bool tinydb_leaf_page_count(const void* page,
                            size_t page_capacity,
                            uint32_t* count) {
    if (count == NULL) return false;
    TinyDBLeafPageFormat format =
        tinydb_leaf_format_detect_page(page, page_capacity);
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        return v1_count(page, count);
    }
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        *count = tinydb_slotted_leaf_v2_count(page, page_capacity);
        return true;
    }
    return false;
}

bool tinydb_leaf_page_key_at(const void* page,
                             size_t page_capacity,
                             uint32_t cell_index,
                             uint32_t* key) {
    if (key == NULL) return false;
    TinyDBLeafPageFormat format =
        tinydb_leaf_format_detect_page(page, page_capacity);
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        uint32_t count = 0u;
        if (!v1_count(page, &count) || cell_index >= count) return false;
        memcpy(key,
               v1_cell(page, cell_index) + LEAF_NODE_KEY_OFFSET,
               sizeof(*key));
        return true;
    }
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        TinyDBSlottedLeafV2Slot slot;
        if (!v2_slot_at(page, page_capacity, cell_index, &slot)) return false;
        *key = slot.key;
        return true;
    }
    return false;
}

bool tinydb_leaf_page_value_at(const void* page,
                               size_t page_capacity,
                               uint32_t cell_index,
                               const void** value,
                               uint32_t* value_length) {
    if (value == NULL || value_length == NULL) return false;
    TinyDBLeafPageFormat format =
        tinydb_leaf_format_detect_page(page, page_capacity);
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        uint32_t count = 0u;
        if (!v1_count(page, &count) || cell_index >= count) return false;
        *value = v1_cell(page, cell_index) + LEAF_NODE_VALUE_OFFSET;
        *value_length = ROW_SIZE;
        return true;
    }
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        TinyDBSlottedLeafV2Slot slot;
        if (!v2_slot_at(page, page_capacity, cell_index, &slot)) return false;
        *value = (const unsigned char*)page + slot.value_offset;
        *value_length = slot.value_length;
        return true;
    }
    return false;
}

bool tinydb_leaf_page_lower_bound(const void* page,
                                  size_t page_capacity,
                                  uint32_t key,
                                  uint32_t* cell_index,
                                  bool* exact_match) {
    if (cell_index == NULL || exact_match == NULL) return false;
    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(page, page_capacity, &count)) return false;

    uint32_t low = 0u;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        uint32_t middle_key = 0u;
        if (!tinydb_leaf_page_key_at(page,
                                     page_capacity,
                                     middle,
                                     &middle_key)) {
            return false;
        }
        if (middle_key < key) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }

    *cell_index = low;
    *exact_match = false;
    if (low < count) {
        uint32_t found_key = 0u;
        if (!tinydb_leaf_page_key_at(page,
                                     page_capacity,
                                     low,
                                     &found_key)) {
            return false;
        }
        *exact_match = found_key == key;
    }
    return true;
}

static bool sibling_page(const void* page,
                         size_t page_capacity,
                         bool next,
                         uint32_t* page_num) {
    if (page_num == NULL) return false;
    TinyDBLeafPageFormat format =
        tinydb_leaf_format_detect_page(page, page_capacity);
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        size_t offset = next ? LEAF_NODE_NEXT_LEAF_OFFSET
                             : LEAF_NODE_PREV_LEAF_OFFSET;
        memcpy(page_num,
               (const unsigned char*)page + offset,
               sizeof(*page_num));
        return true;
    }
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        size_t offset = next ? TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET
                             : TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET;
        *page_num = read_u32_le((const unsigned char*)page + offset);
        return true;
    }
    return false;
}

bool tinydb_leaf_page_next(const void* page,
                           size_t page_capacity,
                           uint32_t* page_num) {
    return sibling_page(page, page_capacity, true, page_num);
}

bool tinydb_leaf_page_prev(const void* page,
                           size_t page_capacity,
                           uint32_t* page_num) {
    return sibling_page(page, page_capacity, false, page_num);
}

bool tinydb_leaf_page_is_fixed_v1(const void* page, size_t page_capacity) {
    return tinydb_leaf_format_detect_page(page, page_capacity) ==
           TINYDB_LEAF_PAGE_FORMAT_FIXED_V1;
}
