#include "record_payload_try_find.h"

#include "leaf_format.h"
#include "leaf_page_access.h"
#include "pager_try_pin.h"
#include "row_envelope.h"

#include <stdio.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message == NULL || message_size == 0u) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

bool tinydb_record_payload_schema_supported(const TableSchema* schema,
                                            char* message,
                                            size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (schema == NULL) {
        set_message(message, message_size, "schema is required");
        return false;
    }
    if (schema->num_columns == 0u || schema->num_columns > MAX_COLUMNS_PER_TABLE) {
        set_message(message, message_size, "schema has an invalid column count");
        return false;
    }
    if (schema->row_size == 0u || schema->row_size > TINYDB_RECORD_PAYLOAD_MAX) {
        set_message(message,
                    message_size,
                    "schema row exceeds the schema-aware logical payload capacity");
        return false;
    }
    if (schema->columns[0].type != COL_TYPE_INT ||
        schema->columns[0].offset != 0u ||
        schema->columns[0].size != sizeof(uint32_t)) {
        set_message(message,
                    message_size,
                    "generic payload records require an INT primary key as the first column");
        return false;
    }

    uint32_t expected_offset = 0u;
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        if (column->size == 0u || column->offset != expected_offset ||
            column->offset > schema->row_size ||
            column->size > schema->row_size - column->offset) {
            set_message(message,
                        message_size,
                        "schema columns must form a contiguous serialized payload layout");
            return false;
        }
        if (column->type == COL_TYPE_INT) {
            if (column->size != sizeof(uint32_t)) {
                set_message(message,
                            message_size,
                            "INT columns must use a 4-byte serialized representation");
                return false;
            }
        } else if (column->type != COL_TYPE_VARCHAR) {
            set_message(message, message_size, "schema contains an unsupported column type");
            return false;
        }
        if (expected_offset > UINT32_MAX - column->size) {
            set_message(message, message_size, "schema serialized offsets overflow");
            return false;
        }
        expected_offset += column->size;
    }
    if (expected_offset != schema->row_size) {
        set_message(message,
                    message_size,
                    "schema row size must match the serialized payload layout");
        return false;
    }
    return true;
}

static bool try_find_valid_page(const Table* table, uint32_t page_num) {
    return table != NULL && table->pager != NULL &&
           page_num != INVALID_PAGE_NUM && page_num < table->pager->num_pages;
}

static bool try_find_internal_child(Table* table,
                                    uint32_t page_num,
                                    void* node,
                                    uint32_t key,
                                    uint32_t* child_page) {
    if (table == NULL || node == NULL || child_page == NULL ||
        get_node_type(node) != NODE_INTERNAL) {
        return false;
    }

    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys == 0u || num_keys > INTERNAL_NODE_MAX_KEYS) return false;

    uint32_t right_child = *internal_node_right_child(node);
    if (!try_find_valid_page(table, right_child) || right_child == page_num) {
        return false;
    }

    uint32_t previous_key = 0u;
    for (uint32_t i = 0u; i < num_keys; i++) {
        uint32_t separator = *internal_node_key(node, i);
        if (i > 0u && separator <= previous_key) return false;
        previous_key = separator;

        uint32_t candidate = *internal_node_child(node, i);
        if (!try_find_valid_page(table, candidate) ||
            candidate == page_num || candidate == right_child) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (*internal_node_child(node, j) == candidate) return false;
        }
    }

    uint32_t min_index = 0u;
    uint32_t max_index = num_keys;
    while (min_index < max_index) {
        uint32_t index = min_index + (max_index - min_index) / 2u;
        if (*internal_node_key(node, index) >= key) {
            max_index = index;
        } else {
            min_index = index + 1u;
        }
    }

    *child_page = min_index == num_keys
        ? right_child
        : *internal_node_child(node, min_index);
    return try_find_valid_page(table, *child_page) && *child_page != page_num;
}

static bool try_find_decode_leaf(const TableSchema* schema,
                                 const void* page,
                                 uint32_t id,
                                 TinyDBRecordPayload* payload,
                                 char* message,
                                 size_t message_size) {
    uint32_t cell_index = 0u;
    bool exact_match = false;
    if (!tinydb_leaf_page_lower_bound(page,
                                      PAGE_SIZE,
                                      id,
                                      &cell_index,
                                      &exact_match)) {
        set_message(message, message_size, "leaf page has an invalid searchable layout");
        return false;
    }
    if (!exact_match) {
        set_message(message, message_size, "primary key not found");
        return false;
    }

    const void* value = NULL;
    uint32_t stored_length = 0u;
    if (!tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cell_index,
                                   &value,
                                   &stored_length) ||
        value == NULL) {
        set_message(message, message_size, "row payload is invalid");
        return false;
    }

    TinyDBRecordPayload local;
    memset(&local, 0, sizeof(local));
    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        if (stored_length < schema->row_size) {
            set_message(message,
                        message_size,
                        "fixed V1 row is shorter than the logical schema payload");
            return false;
        }
        local.length = schema->row_size;
        memcpy(local.bytes, value, local.length);
    } else if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        if (stored_length == schema->row_size) {
            local.length = stored_length;
            memcpy(local.bytes, value, local.length);
        } else if (!tinydb_row_envelope_decode(schema,
                                               value,
                                               stored_length,
                                               &local)) {
            set_message(message,
                        message_size,
                        "slotted V2 row envelope is invalid for the logical schema");
            return false;
        }
    } else {
        set_message(message, message_size, "leaf page uses an unsupported format");
        return false;
    }

    *payload = local;
    return true;
}

bool tinydb_record_payload_try_find(Table* table,
                                    const TableSchema* schema,
                                    uint32_t id,
                                    TinyDBRecordPayload* payload,
                                    char* message,
                                    size_t message_size) {
    if (payload != NULL) memset(payload, 0, sizeof(*payload));
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || table->pager == NULL || schema == NULL || payload == NULL) {
        set_message(message, message_size, "table, schema, and payload are required");
        return false;
    }
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }
    if (!try_find_valid_page(table, schema->root_page_num)) {
        set_message(message, message_size, "schema root page is invalid");
        return false;
    }

    uint32_t page_num = schema->root_page_num;
    for (uint32_t depth = 0u; depth <= table->pager->num_pages; depth++) {
        PagerPageHandle handle;
        PagerTryPinStatus status = pager_try_pin_existing_page_handle(table->pager,
                                                                      page_num,
                                                                      &handle);
        if (status != PAGER_TRY_PIN_OK) {
            if (message != NULL && message_size > 0u) {
                snprintf(message,
                         message_size,
                         "could not acquire B+ tree page %u: %s",
                         page_num,
                         pager_try_pin_status_string(status));
            }
            return false;
        }
        if (!pager_page_handle_acquire_read(&handle)) {
            (void)pager_release_page_handle(&handle);
            set_message(message, message_size, "could not acquire B+ tree page read lock");
            return false;
        }

        NodeType type = get_node_type(handle.data);
        if (type == NODE_INTERNAL) {
            uint32_t child_page = INVALID_PAGE_NUM;
            bool valid = try_find_internal_child(table,
                                                 page_num,
                                                 handle.data,
                                                 id,
                                                 &child_page);
            if (!pager_page_handle_release_read(&handle)) {
                set_message(message, message_size, "could not release B+ tree page read lock");
                return false;
            }
            if (!pager_release_page_handle(&handle)) {
                set_message(message, message_size, "could not release B+ tree page pin");
                return false;
            }
            if (!valid) {
                set_message(message, message_size, "internal B+ tree routing metadata is invalid");
                return false;
            }
            page_num = child_page;
            continue;
        }

        TinyDBRecordPayload local;
        memset(&local, 0, sizeof(local));
        bool decoded = false;
        if (type == NODE_LEAF) {
            decoded = try_find_decode_leaf(schema,
                                           handle.data,
                                           id,
                                           &local,
                                           message,
                                           message_size);
        } else {
            set_message(message, message_size, "B+ tree page has an invalid node type");
        }

        if (!pager_page_handle_release_read(&handle)) {
            set_message(message, message_size, "could not release B+ tree page read lock");
            return false;
        }
        if (!pager_release_page_handle(&handle)) {
            set_message(message, message_size, "could not release B+ tree page pin");
            return false;
        }
        if (!decoded) return false;

        *payload = local;
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    set_message(message, message_size, "B+ tree traversal exceeded the allocated page count");
    return false;
}

bool tinydb_record_payload_encode_values(const TableSchema* schema,
                                         const TinyDBValue* values,
                                         uint32_t value_count,
                                         TinyDBRecordPayload* payload,
                                         char* message,
                                         size_t message_size) {
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }
    if (values == NULL || payload == NULL || value_count != schema->num_columns) {
        set_message(message,
                    message_size,
                    "record value count does not match the payload schema");
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        const TinyDBValue* value = &values[i];
        unsigned char* destination = payload->bytes + column->offset;
        if (value->type != column->type) {
            set_message(message,
                        message_size,
                        "record value type does not match payload column schema");
            memset(payload, 0, sizeof(*payload));
            return false;
        }
        if (column->type == COL_TYPE_INT) {
            memcpy(destination, &value->int_value, sizeof(value->int_value));
        } else {
            size_t length = strlen(value->text);
            if (length >= column->size) {
                set_message(message,
                            message_size,
                            "VARCHAR value exceeds its serialized column capacity");
                memset(payload, 0, sizeof(*payload));
                return false;
            }
            memcpy(destination, value->text, length);
            destination[length] = '\0';
        }
    }
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_payload_decode_values(const TableSchema* schema,
                                         const TinyDBRecordPayload* payload,
                                         TinyDBValue* values,
                                         uint32_t value_capacity,
                                         uint32_t* value_count,
                                         char* message,
                                         size_t message_size) {
    if (value_count != NULL) *value_count = 0u;
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }
    if (payload == NULL || values == NULL || value_capacity < schema->num_columns ||
        payload->length != schema->row_size) {
        set_message(message, message_size, "payload decode buffer or length is invalid");
        return false;
    }

    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        const unsigned char* source = payload->bytes + column->offset;
        TinyDBValue* value = &values[i];
        memset(value, 0, sizeof(*value));
        value->type = column->type;
        if (column->type == COL_TYPE_INT) {
            memcpy(&value->int_value, source, sizeof(value->int_value));
        } else {
            uint32_t length = 0u;
            while (length < column->size && source[length] != '\0') length++;
            if (length == column->size || length > TINYDB_RECORD_TEXT_MAX) {
                set_message(message,
                            message_size,
                            "serialized VARCHAR does not fit the logical value carrier");
                return false;
            }
            memcpy(value->text, source, length);
            value->text[length] = '\0';
        }
    }
    if (value_count != NULL) *value_count = schema->num_columns;
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

static bool validate_legacy_schema(const TableSchema* schema,
                                   char* message,
                                   size_t message_size) {
    if (!tinydb_schema_supports_records(schema, message, message_size)) {
        return false;
    }
    if (schema->row_size > ROW_SIZE) {
        set_message(message,
                    message_size,
                    "logical record does not fit the legacy fixed record carrier");
        return false;
    }
    return true;
}

bool tinydb_record_payload_from_record(const TableSchema* schema,
                                       const TinyDBRecord* record,
                                       TinyDBRecordPayload* payload,
                                       char* message,
                                       size_t message_size) {
    if (!validate_legacy_schema(schema, message, message_size)) return false;
    if (record == NULL || payload == NULL) {
        set_message(message, message_size, "record and payload are required");
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    memcpy(payload->bytes, record->bytes, payload->length);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_payload_to_record(const TableSchema* schema,
                                     const TinyDBRecordPayload* payload,
                                     TinyDBRecord* record,
                                     char* message,
                                     size_t message_size) {
    if (!validate_legacy_schema(schema, message, message_size)) return false;
    if (payload == NULL || record == NULL) {
        set_message(message, message_size, "payload and record are required");
        return false;
    }
    if (payload->length != schema->row_size || payload->length > ROW_SIZE) {
        set_message(message,
                    message_size,
                    "payload length does not match the legacy schema row size");
        return false;
    }

    memset(record, 0, sizeof(*record));
    memcpy(record->bytes, payload->bytes, payload->length);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_payload_pack_fixed_slot(const TinyDBRecordPayload* payload,
                                           void* slot,
                                           size_t slot_size,
                                           char* message,
                                           size_t message_size) {
    if (payload == NULL || slot == NULL) {
        set_message(message, message_size, "payload and fixed slot are required");
        return false;
    }
    if (slot_size != ROW_SIZE) {
        set_message(message,
                    message_size,
                    "legacy fixed leaf slot must be exactly ROW_SIZE bytes");
        return false;
    }
    if (payload->length == 0u || payload->length > ROW_SIZE) {
        set_message(message, message_size, "payload length is outside fixed-slot bounds");
        return false;
    }

    memset(slot, 0, slot_size);
    memcpy(slot, payload->bytes, payload->length);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_payload_unpack_fixed_slot(const TableSchema* schema,
                                             const void* slot,
                                             size_t slot_size,
                                             TinyDBRecordPayload* payload,
                                             char* message,
                                             size_t message_size) {
    if (!validate_legacy_schema(schema, message, message_size)) return false;
    if (slot == NULL || payload == NULL) {
        set_message(message, message_size, "fixed slot and payload are required");
        return false;
    }
    if (slot_size != ROW_SIZE) {
        set_message(message,
                    message_size,
                    "legacy fixed leaf slot must be exactly ROW_SIZE bytes");
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    memcpy(payload->bytes, slot, payload->length);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}
