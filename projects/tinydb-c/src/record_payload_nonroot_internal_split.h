#ifndef RECORD_PAYLOAD_NONROOT_INTERNAL_SPLIT_H
#define RECORD_PAYLOAD_NONROOT_INTERNAL_SPLIT_H

#include "record_payload.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Split a full non-root internal parent after one of its direct V2 leaf
 * children overflows. The already-encoded payload envelope is inserted into
 * the staged split leaves and the new right internal half is inserted into an
 * existing non-full grandparent. If the grandparent is also full, this bounded
 * seam reports the topology as applicable but remains fail-closed so recursive
 * ancestor overflow can be handled by a later layer. */
bool tinydb_record_payload_try_full_nonroot_parent_split(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size);

#endif /* RECORD_PAYLOAD_NONROOT_INTERNAL_SPLIT_H */
