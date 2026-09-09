#ifndef TINYDB_RECORD_PAYLOAD_RECURSIVE_CHAIN_H
#define TINYDB_RECORD_PAYLOAD_RECURSIVE_CHAIN_H

#include "record.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Execute a payload-native V2 leaf overflow through three or more consecutive
 * full internal ancestors. The topology transform itself is arbitrary-depth;
 * the live path is bounded only by the current atomic publication batch
 * capacity. Topologies that need more page images remain fail-closed before
 * page claims, generic-index epoch publication, or live page mutation.
 */
bool tinydb_record_payload_try_bounded_recursive_overflow(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size);

#endif /* TINYDB_RECORD_PAYLOAD_RECURSIVE_CHAIN_H */
