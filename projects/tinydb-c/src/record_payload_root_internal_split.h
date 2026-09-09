#ifndef RECORD_PAYLOAD_ROOT_INTERNAL_SPLIT_H
#define RECORD_PAYLOAD_ROOT_INTERNAL_SPLIT_H

#include "record.h"
#include "slotted_leaf_v2_split.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Handle the first internal-parent overflow reachable from payload-native
 * INSERT: a full stable root whose V2 leaf child must split. The stable root
 * page is rewritten as a one-key root over two newly allocated internal pages;
 * the pending already-encoded payload is inserted only after the complete
 * leaf/internal topology has been staged and validated. */
bool tinydb_record_payload_try_full_root_parent_split(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size);

#endif /* RECORD_PAYLOAD_ROOT_INTERNAL_SPLIT_H */
