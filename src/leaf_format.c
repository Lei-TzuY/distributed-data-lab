#include "leaf_format.h"
#include "slotted_leaf_v2.h"

#include <string.h>

static const TinyDBLeafFormatDescriptor CURRENT_FORMAT = {
    TINYDB_LEAF_FORMAT_FIXED_V1,
    false,
    PAGE_SIZE,
    PAGE_USABLE_SIZE,
    LEAF_NODE_HEADER_SIZE,
    LEAF_NODE_KEY_SIZE,
    LEAF_NODE_VALUE_SIZE,
    LEAF_NODE_CELL_SIZE,
    LEAF_NODE_MAX_CELLS
};

const TinyDBLeafFormatDescriptor* tinydb_leaf_format_current(void) {
    return &CURRENT_FORMAT;
}

bool tinydb_leaf_format_validate_current(void) {
    const TinyDBLeafFormatDescriptor* format = tinydb_leaf_format_current();
    if (format->format_version != TINYDB_LEAF_FORMAT_FIXED_V1 ||
        format->variable_length_values ||
        format->page_size != PAGE_SIZE ||
        format->usable_size != PAGE_USABLE_SIZE ||
        format->header_size != LEAF_NODE_HEADER_SIZE ||
        format->key_size != LEAF_NODE_KEY_SIZE ||
        format->value_capacity != LEAF_NODE_VALUE_SIZE ||
        format->cell_size != LEAF_NODE_CELL_SIZE ||
        format->max_cells != LEAF_NODE_MAX_CELLS) {
        return false;
    }
    if (format->usable_size < format->header_size ||
        format->cell_size != format->key_size + format->value_capacity ||
        format->cell_size == 0u) {
        return false;
    }
    return format->max_cells ==
        (format->usable_size - format->header_size) / format->cell_size;
}

bool tinydb_leaf_format_can_store_value(uint32_t length) {
    const TinyDBLeafFormatDescriptor* format = tinydb_leaf_format_current();
    return tinydb_leaf_format_validate_current() &&
           length > 0u &&
           length <= format->value_capacity;
}

static uint32_t read_u32_le(const unsigned char* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

bool tinydb_leaf_format_v2_marker_disjoint_from_v1(void) {
    if (TINYDB_SLOTTED_V2_MAGIC_OFFSET != LEAF_NODE_NUM_CELLS_OFFSET ||
        TINYDB_SLOTTED_V2_MAGIC_SIZE != LEAF_NODE_NUM_CELLS_SIZE) {
        return false;
    }

    unsigned char marker[TINYDB_SLOTTED_V2_MAGIC_SIZE];
    marker[0] = (unsigned char)(TINYDB_SLOTTED_LEAF_V2_MAGIC & 0xffu);
    marker[1] = (unsigned char)((TINYDB_SLOTTED_LEAF_V2_MAGIC >> 8) & 0xffu);
    marker[2] = (unsigned char)((TINYDB_SLOTTED_LEAF_V2_MAGIC >> 16) & 0xffu);
    marker[3] = (unsigned char)((TINYDB_SLOTTED_LEAF_V2_MAGIC >> 24) & 0xffu);

    for (uint32_t count = 0u; count <= LEAF_NODE_MAX_CELLS; count++) {
        unsigned char encoded_count[LEAF_NODE_NUM_CELLS_SIZE];
        memcpy(encoded_count, &count, sizeof(count));
        if (memcmp(encoded_count, marker, sizeof(marker)) == 0) return false;
    }
    return true;
}

TinyDBLeafPageFormat tinydb_leaf_format_detect_page(const void* page,
                                                    size_t page_capacity) {
    if (page == NULL || page_capacity < PAGE_SIZE ||
        !tinydb_leaf_format_v2_marker_disjoint_from_v1()) {
        return TINYDB_LEAF_PAGE_FORMAT_UNKNOWN;
    }

    const unsigned char* bytes = (const unsigned char*)page;
    if (bytes[NODE_TYPE_OFFSET] != (unsigned char)NODE_LEAF) {
        return TINYDB_LEAF_PAGE_FORMAT_UNKNOWN;
    }

    uint32_t marker = read_u32_le(bytes + TINYDB_SLOTTED_V2_MAGIC_OFFSET);
    if (marker == TINYDB_SLOTTED_LEAF_V2_MAGIC) {
        return tinydb_slotted_leaf_v2_validate(page, page_capacity)
            ? TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2
            : TINYDB_LEAF_PAGE_FORMAT_UNKNOWN;
    }

    uint32_t legacy_count = 0u;
    memcpy(&legacy_count,
           bytes + LEAF_NODE_NUM_CELLS_OFFSET,
           sizeof(legacy_count));
    if (!tinydb_leaf_format_validate_current() ||
        legacy_count > LEAF_NODE_MAX_CELLS ||
        LEAF_NODE_HEADER_SIZE + legacy_count * LEAF_NODE_CELL_SIZE >
            PAGE_USABLE_SIZE) {
        return TINYDB_LEAF_PAGE_FORMAT_UNKNOWN;
    }
    return TINYDB_LEAF_PAGE_FORMAT_FIXED_V1;
}
