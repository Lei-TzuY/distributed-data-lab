#ifndef TINYDB_RECORD_PAYLOAD_TRY_SCAN_H
#define TINYDB_RECORD_PAYLOAD_TRY_SCAN_H

#include "record_payload.h"

/*
 * Backpressure-safe payload-native scans.
 *
 * These additive companions avoid the historical cursor/get_page() path. They
 * acquire one existing B+ tree page at a time with Pager try-pin ownership,
 * release each frame before moving to another page, and therefore can make
 * progress with exactly one replaceable buffer-pool frame.
 *
 * On traversal/backpressure failure, scan_complete is false and the function
 * returns zero with bounded diagnostic detail when a message buffer is
 * supplied. A visitor that returns false requests a successful early stop, as
 * with the historical payload scan APIs. If a later page fails after earlier
 * visitor calls, visitor side effects from that accepted prefix are not rolled
 * back; callers requiring atomic publication should stage their own results.
 */
uint32_t tinydb_record_payload_try_scan(Table* table,
                                        const TableSchema* schema,
                                        TinyDBRecordPayloadVisitor visitor,
                                        void* context,
                                        bool* scan_complete,
                                        char* message,
                                        size_t message_size);

uint32_t tinydb_record_payload_try_scan_range(Table* table,
                                              const TableSchema* schema,
                                              uint32_t min_id,
                                              uint32_t max_id,
                                              TinyDBRecordPayloadVisitor visitor,
                                              void* context,
                                              bool* scan_complete,
                                              char* message,
                                              size_t message_size);

#endif /* TINYDB_RECORD_PAYLOAD_TRY_SCAN_H */
