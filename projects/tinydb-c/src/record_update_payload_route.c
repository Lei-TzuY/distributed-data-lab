#include "record.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool tinydb_record_update_mixed_legacy_base(Table* table,
                                            const TableSchema* schema,
                                            uint32_t id,
                                            const TinyDBValue* values,
                                            uint32_t value_count,
                                            char* message,
                                            size_t message_size);

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static void preserve_value_update_rejection_contract(char* message,
                                                     size_t message_size) {
    if (message == NULL || message_size == 0u ||
        strstr(message,
               "payload-native UPDATE requires an existing compact V2 row") == NULL) {
        return;
    }
    set_message(message,
                message_size,
                "unable to update logical leaf value: slotted V2 rows are read-only unless already stored as compact envelope V2 and the replacement fits without splitting");
}

/*
 * Keep the public value-based UPDATE API, but make the generic-table route use
 * the same schema-sized payload mutation seam as wide V2 callers. This removes
 * the historical TinyDBRecord narrowing step from normal generic UPDATEs and
 * ensures validation/capacity checks happen before the generic-index epoch is
 * published.
 *
 * The legacy users table remains delegated to the historical mixed route. That
 * path owns compatibility behavior around the old users secondary indexes and
 * must not be silently replaced by the generic payload route.
 */
bool tinydb_record_update(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || schema == NULL || values == NULL) {
        set_message(message,
                    message_size,
                    "table, schema, and values are required before generic UPDATE");
        return false;
    }

    if (ci_equal(schema->name, "users")) {
        return tinydb_record_update_mixed_legacy_base(table,
                                                      schema,
                                                      id,
                                                      values,
                                                      value_count,
                                                      message,
                                                      message_size);
    }

    TinyDBRecordPayload payload;
    if (!tinydb_record_payload_encode_values(schema,
                                             values,
                                             value_count,
                                             &payload,
                                             message,
                                             message_size)) {
        return false;
    }

    bool updated = tinydb_record_payload_update(table,
                                                schema,
                                                id,
                                                &payload,
                                                message,
                                                message_size);
    if (!updated) preserve_value_update_rejection_contract(message, message_size);
    return updated;
}
