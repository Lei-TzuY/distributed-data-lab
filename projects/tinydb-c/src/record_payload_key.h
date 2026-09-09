#ifndef RECORD_PAYLOAD_KEY_H
#define RECORD_PAYLOAD_KEY_H

#include "record_payload.h"

#include <stdio.h>
#include <string.h>

/* Extract the generic INT primary key from a schema-sized logical payload.
 * Keep this validation independent of the historical TinyDBRecord carrier so
 * payload-native INSERT/split paths can share one key-decoding contract. */
static inline bool tinydb_record_payload_primary_key(
    const TableSchema* schema,
    const TinyDBRecordPayload* payload,
    uint32_t* key_out,
    char* message,
    size_t message_size) {
    if (key_out != NULL) *key_out = 0u;
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }
    if (payload == NULL || key_out == NULL ||
        payload->length != schema->row_size ||
        payload->length < sizeof(uint32_t)) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "%s",
                     "payload length does not match the schema-sized row layout");
        }
        return false;
    }

    memcpy(key_out, payload->bytes, sizeof(*key_out));
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

#endif /* RECORD_PAYLOAD_KEY_H */