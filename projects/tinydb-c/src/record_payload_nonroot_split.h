#ifndef TINYDB_RECORD_PAYLOAD_NONROOT_SPLIT_H
#define TINYDB_RECORD_PAYLOAD_NONROOT_SPLIT_H

#include "record.h"

#include <stddef.h>

/* Internal schema-sized V2 INSERT helper for a full non-root leaf whose
 * existing internal parent still has room for one more child. The caller
 * supplies an already encoded compact-V2 envelope so no TinyDBRecord narrowing
 * occurs. Parent overflow remains deliberately outside this helper. */
bool tinydb_record_payload_try_nonroot_split(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size);

#endif /* TINYDB_RECORD_PAYLOAD_NONROOT_SPLIT_H */
