#ifndef TINYDB_RECORD_PAYLOAD_ROOT_SPLIT_H
#define TINYDB_RECORD_PAYLOAD_ROOT_SPLIT_H

#include "record.h"

#include <stddef.h>

/* Internal payload-native root-leaf overflow helper. The caller supplies an
 * already encoded compact-V2 envelope so wide logical rows never need to pass
 * through TinyDBRecord. `applicable` becomes true only when the current schema
 * root is a valid slotted-V2 root leaf whose pending row requires a split. */
bool tinydb_record_payload_try_root_split(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size);

#endif /* TINYDB_RECORD_PAYLOAD_ROOT_SPLIT_H */
