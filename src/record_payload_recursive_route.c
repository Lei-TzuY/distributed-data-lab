#include "record_payload_nonroot_split.h"
#include "record_payload_recursive_chain.h"

#include <stdio.h>
#include <string.h>

#define TINYDB_PAYLOAD_FULL_GRANDPARENT_ESCALATION \
    "payload-native recursive internal overflow beyond the grandparent remains fail-closed"

bool tinydb_record_payload_try_nonroot_split_recursive_preflight_base(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size);

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text != NULL ? text : "");
    }
}

bool tinydb_record_payload_try_nonroot_split(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size) {
    bool base_applicable = false;
    char base_message[TINYDB_RECORD_MESSAGE_MAX];
    base_message[0] = '\0';
    if (tinydb_record_payload_try_nonroot_split_recursive_preflight_base(
            table,
            schema,
            key,
            envelope,
            envelope_length,
            &base_applicable,
            base_message,
            sizeof(base_message))) {
        if (applicable != NULL) *applicable = true;
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    if (!base_applicable ||
        strcmp(base_message, TINYDB_PAYLOAD_FULL_GRANDPARENT_ESCALATION) != 0) {
        if (applicable != NULL) *applicable = base_applicable;
        set_message(message, message_size, base_message);
        return false;
    }

    bool recursive_applicable = false;
    char recursive_message[TINYDB_RECORD_MESSAGE_MAX];
    recursive_message[0] = '\0';
    if (tinydb_record_payload_try_bounded_recursive_overflow(
            table,
            schema,
            key,
            envelope,
            envelope_length,
            &recursive_applicable,
            recursive_message,
            sizeof(recursive_message))) {
        if (applicable != NULL) *applicable = true;
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    if (applicable != NULL) {
        *applicable = base_applicable || recursive_applicable;
    }
    set_message(message,
                message_size,
                recursive_message[0] != '\0'
                    ? recursive_message
                    : base_message);
    return false;
}
