#ifndef SLOTTED_LEAF_V2_H
#define SLOTTED_LEAF_V2_H

#include "table.h"

#include <stddef.h>

#define TINYDB_SLOTTED_LEAF_V2_MAGIC 0x32464C54u /* TLF2 */
#define TINYDB_SLOTTED_LEAF_V2_VERSION 2u

/* V2 keeps the historical common B+ tree header prefix so root/parent
 * semantics can eventually be shared, then adds an explicit magic/version and
 * free-space bounds. The slot directory grows upward while variable-length
 * payload bytes grow downward from PAGE_USABLE_SIZE.
 *
 * This codec is deliberately in-memory only today. No production page is
 * identified as or written in V2 format until a database migration/version
 * discriminator is implemented. */
#define TINYDB_SLOTTED_V2_MAGIC_OFFSET        COMMON_NODE_HEADER_SIZE
#define TINYDB_SLOTTED_V2_MAGIC_SIZE          4u
#define TINYDB_SLOTTED_V2_VERSION_OFFSET      (TINYDB_SLOTTED_V2_MAGIC_OFFSET + TINYDB_SLOTTED_V2_MAGIC_SIZE)
#define TINYDB_SLOTTED_V2_VERSION_SIZE        2u
#define TINYDB_SLOTTED_V2_NUM_SLOTS_OFFSET    (TINYDB_SLOTTED_V2_VERSION_OFFSET + TINYDB_SLOTTED_V2_VERSION_SIZE)
#define TINYDB_SLOTTED_V2_NUM_SLOTS_SIZE      2u
#define TINYDB_SLOTTED_V2_FREE_START_OFFSET   (TINYDB_SLOTTED_V2_NUM_SLOTS_OFFSET + TINYDB_SLOTTED_V2_NUM_SLOTS_SIZE)
#define TINYDB_SLOTTED_V2_FREE_START_SIZE     2u
#define TINYDB_SLOTTED_V2_FREE_END_OFFSET     (TINYDB_SLOTTED_V2_FREE_START_OFFSET + TINYDB_SLOTTED_V2_FREE_START_SIZE)
#define TINYDB_SLOTTED_V2_FREE_END_SIZE       2u
#define TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET    (TINYDB_SLOTTED_V2_FREE_END_OFFSET + TINYDB_SLOTTED_V2_FREE_END_SIZE)
#define TINYDB_SLOTTED_V2_NEXT_LEAF_SIZE      4u
#define TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET    (TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET + TINYDB_SLOTTED_V2_NEXT_LEAF_SIZE)
#define TINYDB_SLOTTED_V2_PREV_LEAF_SIZE      4u
#define TINYDB_SLOTTED_V2_FLAGS_OFFSET        (TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET + TINYDB_SLOTTED_V2_PREV_LEAF_SIZE)
#define TINYDB_SLOTTED_V2_FLAGS_SIZE          2u
#define TINYDB_SLOTTED_V2_HEADER_SIZE         (TINYDB_SLOTTED_V2_FLAGS_OFFSET + TINYDB_SLOTTED_V2_FLAGS_SIZE)

#define TINYDB_SLOTTED_V2_SLOT_KEY_OFFSET     0u
#define TINYDB_SLOTTED_V2_SLOT_KEY_SIZE       4u
#define TINYDB_SLOTTED_V2_SLOT_VALUE_OFFSET   4u
#define TINYDB_SLOTTED_V2_SLOT_VALUE_SIZE     2u
#define TINYDB_SLOTTED_V2_SLOT_LENGTH_OFFSET  6u
#define TINYDB_SLOTTED_V2_SLOT_LENGTH_SIZE    2u
#define TINYDB_SLOTTED_V2_SLOT_SIZE           8u

typedef struct {
    uint32_t key;
    uint16_t value_offset;
    uint16_t value_length;
} TinyDBSlottedLeafV2Slot;

bool tinydb_slotted_leaf_v2_init(void* page, size_t page_capacity);
bool tinydb_slotted_leaf_v2_validate(const void* page, size_t page_capacity);

uint16_t tinydb_slotted_leaf_v2_count(const void* page, size_t page_capacity);
uint32_t tinydb_slotted_leaf_v2_free_bytes(const void* page, size_t page_capacity);

bool tinydb_slotted_leaf_v2_find(const void* page,
                                 size_t page_capacity,
                                 uint32_t key,
                                 TinyDBSlottedLeafV2Slot* slot,
                                 uint16_t* slot_index);

bool tinydb_slotted_leaf_v2_insert(void* page,
                                   size_t page_capacity,
                                   uint32_t key,
                                   const void* value,
                                   uint16_t value_length);

bool tinydb_slotted_leaf_v2_read(const void* page,
                                 size_t page_capacity,
                                 uint32_t key,
                                 void* output,
                                 size_t output_capacity,
                                 uint16_t* value_length);

bool tinydb_slotted_leaf_v2_update(void* page,
                                   size_t page_capacity,
                                   uint32_t key,
                                   const void* value,
                                   uint16_t value_length);

bool tinydb_slotted_leaf_v2_delete(void* page,
                                   size_t page_capacity,
                                   uint32_t key);

bool tinydb_slotted_leaf_v2_compact(void* page, size_t page_capacity);

#endif /* SLOTTED_LEAF_V2_H */
