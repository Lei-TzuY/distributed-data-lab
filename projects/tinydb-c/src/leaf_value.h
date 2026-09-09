#ifndef LEAF_VALUE_H
#define LEAF_VALUE_H

#include "table.h"

#include <stddef.h>

/* Schema-aware callers operate on logical byte strings. The current storage
 * implementation still bridges those bytes into the historical ROW_SIZE leaf
 * carrier. A future slotted-page implementation can replace this module while
 * leaving record encoding and SQL execution unchanged. */

/* The insert adapter temporarily uses Row as the input carrier accepted by the
 * legacy B+ tree API. This predicate makes the required ABI layout explicit:
 * the three Row fields must begin at the same offsets as the canonical
 * serialized legacy slot, and the struct must contain at least ROW_SIZE bytes. */
bool tinydb_leaf_value_legacy_layout_compatible(void);

bool tinydb_leaf_value_insert(Cursor* cursor,
                              uint32_t key,
                              const void* bytes,
                              uint32_t length);

bool tinydb_leaf_value_write(Cursor* cursor,
                             const void* bytes,
                             uint32_t length);

bool tinydb_leaf_value_read(Cursor* cursor,
                            void* output,
                            size_t output_capacity,
                            uint32_t length);

#endif /* LEAF_VALUE_H */
