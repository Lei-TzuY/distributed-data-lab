#ifndef TINYDB_RECORD_PAYLOAD_TRY_FIND_H
#define TINYDB_RECORD_PAYLOAD_TRY_FIND_H

#include "record_payload.h"

/*
 * Backpressure-safe payload-native primary-key lookup.
 *
 * Unlike the historical tinydb_record_payload_find() cursor path, this API
 * never calls process-fatal get_page(). It acquires one existing B+ tree page
 * at a time through pager_try_pin_existing_page_handle(), releases each parent
 * before descending, and publishes the decoded payload only after the leaf
 * read lock and pin have both been released successfully.
 *
 * On failure, payload is zeroed when non-NULL and message receives bounded
 * detail when a diagnostic buffer is supplied. A missing key is reported as a
 * normal false result with a "primary key not found" diagnostic.
 */
bool tinydb_record_payload_try_find(Table* table,
                                    const TableSchema* schema,
                                    uint32_t id,
                                    TinyDBRecordPayload* payload,
                                    char* message,
                                    size_t message_size);

#endif /* TINYDB_RECORD_PAYLOAD_TRY_FIND_H */
