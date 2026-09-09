#include "slotted_leaf_v2.h"

#include <stdlib.h>
#include <string.h>

static bool valid_page(const void* page, size_t page_capacity) {
    return page != NULL && page_capacity >= PAGE_SIZE;
}

static uint16_t read_u16_le(const unsigned char* bytes) {
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32_le(const unsigned char* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void write_u16_le(unsigned char* bytes, uint16_t value) {
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
}

static void write_u32_le(unsigned char* bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
}

static unsigned char* slot_ptr(void* page, uint16_t slot_index) {
    return (unsigned char*)page + TINYDB_SLOTTED_V2_HEADER_SIZE +
           (uint32_t)slot_index * TINYDB_SLOTTED_V2_SLOT_SIZE;
}

static const unsigned char* const_slot_ptr(const void* page, uint16_t slot_index) {
    return (const unsigned char*)page + TINYDB_SLOTTED_V2_HEADER_SIZE +
           (uint32_t)slot_index * TINYDB_SLOTTED_V2_SLOT_SIZE;
}

static uint16_t raw_count(const void* page) {
    return read_u16_le((const unsigned char*)page +
                       TINYDB_SLOTTED_V2_NUM_SLOTS_OFFSET);
}

static uint16_t raw_free_start(const void* page) {
    return read_u16_le((const unsigned char*)page +
                       TINYDB_SLOTTED_V2_FREE_START_OFFSET);
}

static uint16_t raw_free_end(const void* page) {
    return read_u16_le((const unsigned char*)page +
                       TINYDB_SLOTTED_V2_FREE_END_OFFSET);
}

static void set_count(void* page, uint16_t value) {
    write_u16_le((unsigned char*)page + TINYDB_SLOTTED_V2_NUM_SLOTS_OFFSET,
                 value);
}

static void set_free_start(void* page, uint16_t value) {
    write_u16_le((unsigned char*)page + TINYDB_SLOTTED_V2_FREE_START_OFFSET,
                 value);
}

static void set_free_end(void* page, uint16_t value) {
    write_u16_le((unsigned char*)page + TINYDB_SLOTTED_V2_FREE_END_OFFSET,
                 value);
}

static TinyDBSlottedLeafV2Slot read_slot(const void* page, uint16_t slot_index) {
    const unsigned char* bytes = const_slot_ptr(page, slot_index);
    TinyDBSlottedLeafV2Slot slot;
    slot.key = read_u32_le(bytes + TINYDB_SLOTTED_V2_SLOT_KEY_OFFSET);
    slot.value_offset = read_u16_le(bytes + TINYDB_SLOTTED_V2_SLOT_VALUE_OFFSET);
    slot.value_length = read_u16_le(bytes + TINYDB_SLOTTED_V2_SLOT_LENGTH_OFFSET);
    return slot;
}

static void write_slot(void* page,
                       uint16_t slot_index,
                       const TinyDBSlottedLeafV2Slot* slot) {
    unsigned char* bytes = slot_ptr(page, slot_index);
    write_u32_le(bytes + TINYDB_SLOTTED_V2_SLOT_KEY_OFFSET, slot->key);
    write_u16_le(bytes + TINYDB_SLOTTED_V2_SLOT_VALUE_OFFSET,
                 slot->value_offset);
    write_u16_le(bytes + TINYDB_SLOTTED_V2_SLOT_LENGTH_OFFSET,
                 slot->value_length);
}

bool tinydb_slotted_leaf_v2_init(void* page, size_t page_capacity) {
    if (!valid_page(page, page_capacity) ||
        PAGE_USABLE_SIZE > UINT16_MAX ||
        TINYDB_SLOTTED_V2_HEADER_SIZE > PAGE_USABLE_SIZE) {
        return false;
    }

    unsigned char* bytes = (unsigned char*)page;
    /* PAGE_CHECKSUM_SIZE remains owned by Pager and is deliberately untouched. */
    memset(bytes, 0, PAGE_USABLE_SIZE);
    bytes[NODE_TYPE_OFFSET] = (unsigned char)NODE_LEAF;
    bytes[IS_ROOT_OFFSET] = 0u;
    write_u32_le(bytes + PARENT_POINTER_OFFSET, 0u);
    write_u32_le(bytes + TINYDB_SLOTTED_V2_MAGIC_OFFSET,
                 TINYDB_SLOTTED_LEAF_V2_MAGIC);
    write_u16_le(bytes + TINYDB_SLOTTED_V2_VERSION_OFFSET,
                 TINYDB_SLOTTED_LEAF_V2_VERSION);
    set_count(page, 0u);
    set_free_start(page, (uint16_t)TINYDB_SLOTTED_V2_HEADER_SIZE);
    set_free_end(page, (uint16_t)PAGE_USABLE_SIZE);
    return true;
}

bool tinydb_slotted_leaf_v2_validate(const void* page, size_t page_capacity) {
    if (!valid_page(page, page_capacity)) return false;

    const unsigned char* bytes = (const unsigned char*)page;
    if (bytes[NODE_TYPE_OFFSET] != (unsigned char)NODE_LEAF ||
        read_u32_le(bytes + TINYDB_SLOTTED_V2_MAGIC_OFFSET) !=
            TINYDB_SLOTTED_LEAF_V2_MAGIC ||
        read_u16_le(bytes + TINYDB_SLOTTED_V2_VERSION_OFFSET) !=
            TINYDB_SLOTTED_LEAF_V2_VERSION) {
        return false;
    }

    uint16_t count = raw_count(page);
    uint32_t expected_free_start = TINYDB_SLOTTED_V2_HEADER_SIZE +
        (uint32_t)count * TINYDB_SLOTTED_V2_SLOT_SIZE;
    uint16_t free_start = raw_free_start(page);
    uint16_t free_end = raw_free_end(page);
    if (expected_free_start > PAGE_USABLE_SIZE ||
        free_start != expected_free_start ||
        free_start > free_end ||
        free_end > PAGE_USABLE_SIZE) {
        return false;
    }

    uint32_t total_payload = 0u;
    uint32_t previous_key = 0u;
    for (uint16_t i = 0u; i < count; i++) {
        TinyDBSlottedLeafV2Slot current = read_slot(page, i);
        if (current.value_length == 0u ||
            current.value_offset < free_end ||
            current.value_offset > PAGE_USABLE_SIZE ||
            current.value_length > PAGE_USABLE_SIZE - current.value_offset) {
            return false;
        }
        if (i > 0u && current.key <= previous_key) return false;
        previous_key = current.key;
        total_payload += current.value_length;

        for (uint16_t j = 0u; j < i; j++) {
            TinyDBSlottedLeafV2Slot earlier = read_slot(page, j);
            uint32_t current_end =
                (uint32_t)current.value_offset + current.value_length;
            uint32_t earlier_end =
                (uint32_t)earlier.value_offset + earlier.value_length;
            if ((uint32_t)current.value_offset < earlier_end &&
                (uint32_t)earlier.value_offset < current_end) {
                return false;
            }
        }
    }

    return total_payload == PAGE_USABLE_SIZE - free_end;
}

uint16_t tinydb_slotted_leaf_v2_count(const void* page, size_t page_capacity) {
    if (!tinydb_slotted_leaf_v2_validate(page, page_capacity)) return 0u;
    return raw_count(page);
}

uint32_t tinydb_slotted_leaf_v2_free_bytes(const void* page,
                                           size_t page_capacity) {
    if (!tinydb_slotted_leaf_v2_validate(page, page_capacity)) return 0u;
    return (uint32_t)raw_free_end(page) - raw_free_start(page);
}

static uint16_t lower_bound_key(const void* page,
                                uint16_t count,
                                uint32_t key) {
    uint16_t low = 0u;
    uint16_t high = count;
    while (low < high) {
        uint16_t middle = (uint16_t)(low + (uint16_t)((high - low) / 2u));
        TinyDBSlottedLeafV2Slot slot = read_slot(page, middle);
        if (slot.key < key) {
            low = (uint16_t)(middle + 1u);
        } else {
            high = middle;
        }
    }
    return low;
}

bool tinydb_slotted_leaf_v2_find(const void* page,
                                 size_t page_capacity,
                                 uint32_t key,
                                 TinyDBSlottedLeafV2Slot* slot,
                                 uint16_t* slot_index) {
    if (!tinydb_slotted_leaf_v2_validate(page, page_capacity)) return false;
    uint16_t count = raw_count(page);
    uint16_t index = lower_bound_key(page, count, key);
    if (index >= count) return false;
    TinyDBSlottedLeafV2Slot found = read_slot(page, index);
    if (found.key != key) return false;
    if (slot != NULL) *slot = found;
    if (slot_index != NULL) *slot_index = index;
    return true;
}

bool tinydb_slotted_leaf_v2_insert(void* page,
                                   size_t page_capacity,
                                   uint32_t key,
                                   const void* value,
                                   uint16_t value_length) {
    if (value == NULL || value_length == 0u ||
        !tinydb_slotted_leaf_v2_validate(page, page_capacity)) {
        return false;
    }

    uint16_t count = raw_count(page);
    uint16_t index = lower_bound_key(page, count, key);
    if (index < count && read_slot(page, index).key == key) return false;

    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + value_length;
    if (required > tinydb_slotted_leaf_v2_free_bytes(page, page_capacity)) {
        return false;
    }

    uint16_t free_end = raw_free_end(page);
    uint16_t value_offset = (uint16_t)(free_end - value_length);
    unsigned char* insertion = slot_ptr(page, index);
    size_t move_size = (size_t)(count - index) * TINYDB_SLOTTED_V2_SLOT_SIZE;
    if (move_size > 0u) {
        memmove(insertion + TINYDB_SLOTTED_V2_SLOT_SIZE,
                insertion,
                move_size);
    }

    memcpy((unsigned char*)page + value_offset, value, value_length);
    TinyDBSlottedLeafV2Slot new_slot;
    new_slot.key = key;
    new_slot.value_offset = value_offset;
    new_slot.value_length = value_length;
    write_slot(page, index, &new_slot);

    set_count(page, (uint16_t)(count + 1u));
    set_free_start(page,
                   (uint16_t)(TINYDB_SLOTTED_V2_HEADER_SIZE +
                              (uint32_t)(count + 1u) *
                                  TINYDB_SLOTTED_V2_SLOT_SIZE));
    set_free_end(page, value_offset);
    return tinydb_slotted_leaf_v2_validate(page, page_capacity);
}

bool tinydb_slotted_leaf_v2_read(const void* page,
                                 size_t page_capacity,
                                 uint32_t key,
                                 void* output,
                                 size_t output_capacity,
                                 uint16_t* value_length) {
    TinyDBSlottedLeafV2Slot slot;
    if (output == NULL ||
        !tinydb_slotted_leaf_v2_find(page,
                                     page_capacity,
                                     key,
                                     &slot,
                                     NULL) ||
        output_capacity < slot.value_length) {
        return false;
    }
    memcpy(output,
           (const unsigned char*)page + slot.value_offset,
           slot.value_length);
    if (value_length != NULL) *value_length = slot.value_length;
    return true;
}

static void copy_page_identity(void* destination, const void* source) {
    unsigned char* dst = (unsigned char*)destination;
    const unsigned char* src = (const unsigned char*)source;
    dst[IS_ROOT_OFFSET] = src[IS_ROOT_OFFSET];
    memcpy(dst + PARENT_POINTER_OFFSET,
           src + PARENT_POINTER_OFFSET,
           PARENT_POINTER_SIZE);
    memcpy(dst + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
           src + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
           TINYDB_SLOTTED_V2_NEXT_LEAF_SIZE);
    memcpy(dst + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
           src + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
           TINYDB_SLOTTED_V2_PREV_LEAF_SIZE);
    memcpy(dst + TINYDB_SLOTTED_V2_FLAGS_OFFSET,
           src + TINYDB_SLOTTED_V2_FLAGS_OFFSET,
           TINYDB_SLOTTED_V2_FLAGS_SIZE);
}

static bool rebuild_page(void* page,
                         size_t page_capacity,
                         bool replace,
                         uint32_t replace_key,
                         const void* replacement,
                         uint16_t replacement_length,
                         bool omit,
                         uint32_t omit_key) {
    if (!tinydb_slotted_leaf_v2_validate(page, page_capacity)) return false;

    unsigned char* scratch = (unsigned char*)malloc(PAGE_SIZE);
    if (scratch == NULL) return false;
    memset(scratch, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(scratch, PAGE_SIZE)) {
        free(scratch);
        return false;
    }
    copy_page_identity(scratch, page);

    uint16_t count = raw_count(page);
    bool replaced = false;
    bool omitted = false;
    bool ok = true;
    for (uint16_t i = 0u; i < count && ok; i++) {
        TinyDBSlottedLeafV2Slot slot = read_slot(page, i);
        if (omit && slot.key == omit_key) {
            omitted = true;
            continue;
        }

        const void* value = (const unsigned char*)page + slot.value_offset;
        uint16_t length = slot.value_length;
        if (replace && slot.key == replace_key) {
            value = replacement;
            length = replacement_length;
            replaced = true;
        }
        ok = tinydb_slotted_leaf_v2_insert(scratch,
                                           PAGE_SIZE,
                                           slot.key,
                                           value,
                                           length);
    }

    if (ok && replace && !replaced) ok = false;
    if (ok && omit && !omitted) ok = false;
    if (ok) {
        memcpy(page, scratch, PAGE_USABLE_SIZE);
        ok = tinydb_slotted_leaf_v2_validate(page, page_capacity);
    }
    free(scratch);
    return ok;
}

bool tinydb_slotted_leaf_v2_update(void* page,
                                   size_t page_capacity,
                                   uint32_t key,
                                   const void* value,
                                   uint16_t value_length) {
    if (value == NULL || value_length == 0u ||
        !tinydb_slotted_leaf_v2_validate(page, page_capacity)) {
        return false;
    }

    TinyDBSlottedLeafV2Slot old_slot;
    if (!tinydb_slotted_leaf_v2_find(page,
                                     page_capacity,
                                     key,
                                     &old_slot,
                                     NULL)) {
        return false;
    }
    uint32_t available = tinydb_slotted_leaf_v2_free_bytes(page, page_capacity) +
                         old_slot.value_length;
    if (value_length > available) return false;

    return rebuild_page(page,
                        page_capacity,
                        true,
                        key,
                        value,
                        value_length,
                        false,
                        0u);
}

bool tinydb_slotted_leaf_v2_delete(void* page,
                                   size_t page_capacity,
                                   uint32_t key) {
    if (!tinydb_slotted_leaf_v2_validate(page, page_capacity)) return false;
    return rebuild_page(page,
                        page_capacity,
                        false,
                        0u,
                        NULL,
                        0u,
                        true,
                        key);
}

bool tinydb_slotted_leaf_v2_compact(void* page, size_t page_capacity) {
    return rebuild_page(page,
                        page_capacity,
                        false,
                        0u,
                        NULL,
                        0u,
                        false,
                        0u);
}
