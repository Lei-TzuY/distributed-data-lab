#ifndef GENERIC_INDEX_PAYLOAD_SCAN_SHIM_H
#define GENERIC_INDEX_PAYLOAD_SCAN_SHIM_H

#include "record_payload.h"

#include <stdio.h>

/*
 * generic_index_candidates.c historically rebuilds its persistent candidate
 * snapshot through TinyDBRecord. That carrier is intentionally fixed at
 * ROW_SIZE and therefore cannot represent schema-sized V2 rows. Keep the
 * candidate builder itself format-agnostic by adapting just the two record
 * primitives it consumes: scan and decode.
 *
 * The payload visitor is deliberately bridged back to TinyDBRecordVisitor as
 * an opaque pointer. The pointer is never dereferenced as TinyDBRecord: the
 * decode shim below converts it straight back to TinyDBRecordPayload before
 * decoding. This lets the existing candidate-entry construction and ordering
 * code remain shared between narrow and wide schemas.
 */
typedef struct {
    TinyDBRecordVisitor visitor;
    void* context;
} TinyDBGenericIndexPayloadScanBridge;

static bool tinydb_generic_index_payload_scan_bridge(
    const TableSchema* schema,
    const TinyDBRecordPayload* payload,
    void* raw_context) {
    TinyDBGenericIndexPayloadScanBridge* bridge =
        (TinyDBGenericIndexPayloadScanBridge*)raw_context;
    if (bridge == NULL || bridge->visitor == NULL) return false;
    return bridge->visitor(schema,
                           (const TinyDBRecord*)(const void*)payload,
                           bridge->context);
}

static uint32_t tinydb_generic_index_payload_compatible_scan(
    Table* table,
    const TableSchema* schema,
    TinyDBRecordVisitor visitor,
    void* context) {
    if (schema == NULL || schema->row_size <= ROW_SIZE) {
        return tinydb_record_scan(table, schema, visitor, context);
    }

    TinyDBGenericIndexPayloadScanBridge bridge;
    bridge.visitor = visitor;
    bridge.context = context;

    bool scan_complete = false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    TinyDBRecordPayloadVisitor payload_visitor =
        visitor == NULL ? NULL : tinydb_generic_index_payload_scan_bridge;
    void* payload_context = visitor == NULL ? NULL : &bridge;
    uint32_t count = tinydb_record_payload_scan(table,
                                                schema,
                                                payload_visitor,
                                                payload_context,
                                                &scan_complete,
                                                message,
                                                sizeof(message));

    /* ensure_snapshot() already treats a visitor/decode failure as fatal. On
     * a traversal failure, feed the existing builder a NULL sentinel so it
     * marks the same failure bit instead of accepting a truncated snapshot as
     * a valid empty/partial index image. */
    if (!scan_complete && visitor != NULL) {
        (void)visitor(schema, NULL, context);
    }
    return count;
}

static bool tinydb_generic_index_payload_compatible_decode(
    const TableSchema* schema,
    const TinyDBRecord* record,
    TinyDBValue* values,
    uint32_t value_capacity,
    uint32_t* value_count,
    char* message,
    size_t message_size) {
    if (schema == NULL || schema->row_size <= ROW_SIZE) {
        return tinydb_record_decode(schema,
                                    record,
                                    values,
                                    value_capacity,
                                    value_count,
                                    message,
                                    message_size);
    }
    if (record == NULL) {
        if (message != NULL && message_size > 0) {
            snprintf(message,
                     message_size,
                     "%s",
                     "payload candidate scan did not complete");
        }
        return false;
    }
    return tinydb_record_payload_decode_values(
        schema,
        (const TinyDBRecordPayload*)(const void*)record,
        values,
        value_capacity,
        value_count,
        message,
        message_size);
}

/* This header is injected only after generic_index_candidates.h from
 * generic_index_epoch.h. The macros therefore affect the implementation below
 * the include boundary without changing record.h's public ABI. */
#define tinydb_record_scan tinydb_generic_index_payload_compatible_scan
#define tinydb_record_decode tinydb_generic_index_payload_compatible_decode

#endif /* GENERIC_INDEX_PAYLOAD_SCAN_SHIM_H */
