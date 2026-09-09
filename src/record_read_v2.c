#include "generic_index_epoch.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "record_payload.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

static void begin_root_scope(Table* table,
                             const TableSchema* schema,
                             uint32_t* previous_root) {
    *previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
}

static void end_root_scope(Table* table, uint32_t previous_root) {
    table->root_page_num = previous_root;
}

static bool cursor_key(Cursor* cursor, uint32_t* key) {
    if (key != NULL) *key = 0u;
    if (cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL || key == NULL ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= cursor->table->pager->num_pages) {
        return false;
    }
    void* page = get_page(cursor->table->pager, cursor->page_num);
    if (get_node_type(page) != NODE_LEAF) return false;
    return tinydb_leaf_page_key_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   key);
}

static bool cursor_key_equals(Cursor* cursor, uint32_t key) {
    uint32_t found = 0u;
    return cursor_key(cursor, &found) && found == key;
}

static bool raw_value_to_payload(const TableSchema* schema,
                                 const void* value,
                                 uint32_t stored_length,
                                 TinyDBLeafPageFormat format,
                                 TinyDBRecordPayload* payload) {
    /* Callers validate the schema contract appropriate to their API before
     * reaching this physical decoder: payload-native callers use the full
     * schema-sized validator while legacy callers retain their historical
     * compatibility validator. Keep this helper layout-focused so tightening
     * the new API cannot accidentally reject an otherwise supported legacy
     * catalog layout. */
    if (schema == NULL || value == NULL || payload == NULL ||
        schema->row_size == 0u ||
        schema->row_size > sizeof(payload->bytes)) {
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        /* A fixed V1 slot can only contain schemas that physically fit it.
         * Wide schemas therefore fail closed here rather than being truncated. */
        if (stored_length < schema->row_size) return false;
        payload->length = schema->row_size;
        memcpy(payload->bytes, value, payload->length);
        return true;
    }
    if (format != TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) return false;

    /* V2 accepts both migration-era raw logical payloads and compact row
     * envelopes. Neither path is constrained by the historical ROW_SIZE. */
    if (stored_length == schema->row_size) {
        payload->length = stored_length;
        memcpy(payload->bytes, value, payload->length);
        return true;
    }
    return tinydb_row_envelope_decode(schema,
                                      value,
                                      stored_length,
                                      payload);
}

static bool cursor_to_payload(const TableSchema* schema,
                              Cursor* cursor,
                              TinyDBRecordPayload* payload) {
    if (schema == NULL || cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL || payload == NULL ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= cursor->table->pager->num_pages) {
        return false;
    }

    void* page = get_page(cursor->table->pager, cursor->page_num);
    if (get_node_type(page) != NODE_LEAF) return false;

    const void* value = NULL;
    uint32_t stored_length = 0u;
    if (!tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &value,
                                   &stored_length) ||
        value == NULL) {
        return false;
    }

    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    return raw_value_to_payload(schema,
                                value,
                                stored_length,
                                format,
                                payload);
}

static bool cursor_to_record(const TableSchema* schema,
                             Cursor* cursor,
                             TinyDBRecord* record) {
    if (record == NULL) return false;

    TinyDBRecordPayload payload;
    if (!cursor_to_payload(schema, cursor, &payload)) return false;

    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_to_record(schema,
                                           &payload,
                                           record,
                                           message,
                                           sizeof(message));
}

bool tinydb_record_payload_find(Table* table,
                                const TableSchema* schema,
                                uint32_t id,
                                TinyDBRecordPayload* payload,
                                char* message,
                                size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || schema == NULL || payload == NULL) {
        set_message(message, message_size, "table, schema, and payload are required");
        return false;
    }
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, id);
    bool found = cursor_key_equals(cursor, id) &&
                 cursor_to_payload(schema, cursor, payload);
    free(cursor);
    end_root_scope(table, previous_root);
    if (!found) {
        set_message(message, message_size, "primary key not found or row payload is invalid");
    }
    return found;
}

uint32_t tinydb_record_payload_scan(Table* table,
                                    const TableSchema* schema,
                                    TinyDBRecordPayloadVisitor visitor,
                                    void* context,
                                    bool* scan_complete,
                                    char* message,
                                    size_t message_size) {
    if (scan_complete != NULL) *scan_complete = false;
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || schema == NULL) {
        set_message(message, message_size, "table and schema are required");
        return 0u;
    }
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return 0u;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_start(table);
    if (cursor == NULL) {
        end_root_scope(table, previous_root);
        set_message(message, message_size, "unable to start leaf scan");
        return 0u;
    }

    uint32_t count = 0u;
    bool complete = true;
    while (!cursor->end_of_table) {
        TinyDBRecordPayload payload;
        if (!cursor_to_payload(schema, cursor, &payload)) {
            complete = false;
            set_message(message, message_size, "unable to decode logical row payload during scan");
            break;
        }
        count++;
        if (visitor != NULL && !visitor(schema, &payload, context)) break;
        if (!tinydb_leaf_read_advance_checked(cursor)) {
            complete = false;
            set_message(message, message_size, "leaf scan encountered corrupt traversal metadata");
            break;
        }
    }

    free(cursor);
    end_root_scope(table, previous_root);
    if (scan_complete != NULL) *scan_complete = complete;
    return complete ? count : 0u;
}

uint32_t tinydb_record_payload_scan_range(Table* table,
                                          const TableSchema* schema,
                                          uint32_t min_id,
                                          uint32_t max_id,
                                          TinyDBRecordPayloadVisitor visitor,
                                          void* context,
                                          bool* scan_complete,
                                          char* message,
                                          size_t message_size) {
    if (scan_complete != NULL) *scan_complete = false;
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || schema == NULL) {
        set_message(message, message_size, "table and schema are required");
        return 0u;
    }
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return 0u;
    }
    if (min_id > max_id) {
        if (scan_complete != NULL) *scan_complete = true;
        return 0u;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, min_id);
    if (cursor == NULL) {
        end_root_scope(table, previous_root);
        set_message(message, message_size, "unable to seek to payload range start");
        return 0u;
    }

    uint32_t count = 0u;
    bool complete = true;
    while (!cursor->end_of_table) {
        uint32_t key = 0u;
        if (!cursor_key(cursor, &key)) {
            complete = false;
            set_message(message, message_size, "unable to read primary key during payload range scan");
            break;
        }
        if (key > max_id) break;
        if (key < min_id) {
            complete = false;
            set_message(message, message_size, "payload range cursor moved before its lower bound");
            break;
        }

        TinyDBRecordPayload payload;
        if (!cursor_to_payload(schema, cursor, &payload)) {
            complete = false;
            set_message(message, message_size, "unable to decode logical row payload during range scan");
            break;
        }
        count++;
        if (visitor != NULL && !visitor(schema, &payload, context)) break;
        if (!tinydb_leaf_read_advance_checked(cursor)) {
            complete = false;
            set_message(message, message_size, "payload range scan encountered corrupt traversal metadata");
            break;
        }
    }

    free(cursor);
    end_root_scope(table, previous_root);
    if (scan_complete != NULL) *scan_complete = complete;
    return complete ? count : 0u;
}

static bool payload_existing_row_is_compact_v2(Cursor* cursor) {
    if (cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= cursor->table->pager->num_pages) {
        return false;
    }
    void* page = get_page(cursor->table->pager, cursor->page_num);
    const void* value = NULL;
    uint32_t stored_length = 0u;
    if (!tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &value,
                                   &stored_length) ||
        value == NULL || stored_length < TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE) {
        return false;
    }
    const unsigned char* bytes = (const unsigned char*)value;
    return tinydb_row_envelope_read_u32_le(
               bytes + TINYDB_ROW_ENVELOPE_MAGIC_OFFSET) ==
               TINYDB_ROW_ENVELOPE_MAGIC &&
           tinydb_row_envelope_read_u16_le(
               bytes + TINYDB_ROW_ENVELOPE_VERSION_OFFSET) ==
               TINYDB_ROW_ENVELOPE_VERSION_COMPACT_V2;
}

bool tinydb_record_payload_update(Table* table,
                                  const TableSchema* schema,
                                  uint32_t id,
                                  const TinyDBRecordPayload* payload,
                                  char* message,
                                  size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || table->pager == NULL || schema == NULL ||
        payload == NULL) {
        set_message(message,
                    message_size,
                    "table, schema, and payload are required for payload-native UPDATE");
        return false;
    }
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }
    if (!ci_equal(schema->columns[0].name, "id")) {
        set_message(message,
                    message_size,
                    "payload-native UPDATE requires the first column to be id INT");
        return false;
    }
    if (ci_equal(schema->name, "users")) {
        set_message(message,
                    message_size,
                    "payload-native UPDATE excludes the legacy users table so its secondary indexes stay synchronized");
        return false;
    }
    if (schema->root_page_num >= table->pager->num_pages ||
        payload->length != schema->row_size ||
        payload->length < sizeof(uint32_t)) {
        set_message(message,
                    message_size,
                    "payload length or schema root is invalid for payload-native UPDATE");
        return false;
    }

    uint32_t encoded_id = 0u;
    memcpy(&encoded_id, payload->bytes, sizeof(encoded_id));
    if (encoded_id != id) {
        set_message(message,
                    message_size,
                    "payload-native UPDATE cannot change the primary-key id");
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, id);
    if (!cursor_key_equals(cursor, id)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "primary key not found");
        return false;
    }

    void* page = get_page(table->pager, cursor->page_num);
    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    bool ready = false;
    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;

    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        const void* value = NULL;
        uint32_t stored_length = 0u;
        ready = payload->length <= ROW_SIZE &&
                tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE) &&
                tinydb_leaf_page_value_at(page,
                                          PAGE_SIZE,
                                          cursor->cell_num,
                                          &value,
                                          &stored_length) &&
                value != NULL && stored_length == ROW_SIZE &&
                payload->length <= stored_length;
        if (!ready) {
            set_message(message,
                        message_size,
                        "payload-native UPDATE cannot fit this payload in a fixed V1 row slot");
        }
    } else if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        TinyDBSlottedLeafV2Slot old_slot;
        ready = tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
                payload_existing_row_is_compact_v2(cursor) &&
                tinydb_row_envelope_encode_compact_v2(schema,
                                                       payload,
                                                       envelope,
                                                       sizeof(envelope),
                                                       &envelope_length) &&
                envelope_length > 0u && envelope_length <= UINT16_MAX &&
                tinydb_slotted_leaf_v2_find(page,
                                            PAGE_SIZE,
                                            id,
                                            &old_slot,
                                            NULL) &&
                envelope_length <=
                    tinydb_slotted_leaf_v2_free_bytes(page, PAGE_SIZE) +
                        old_slot.value_length;
        if (!ready) {
            set_message(message,
                        message_size,
                        "payload-native UPDATE requires an existing compact V2 row whose replacement fits without splitting");
        }
    } else {
        set_message(message,
                    message_size,
                    "payload-native UPDATE does not support this leaf format");
    }

    if (!ready) {
        free(cursor);
        end_root_scope(table, previous_root);
        return false;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }

    bool updated = false;
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        const void* value = NULL;
        uint32_t stored_length = 0u;
        if (tinydb_leaf_page_value_at(page,
                                      PAGE_SIZE,
                                      cursor->cell_num,
                                      &value,
                                      &stored_length) &&
            value != NULL && stored_length == ROW_SIZE) {
            void* writable = (void*)value;
            memset(writable, 0, stored_length);
            memcpy(writable, payload->bytes, payload->length);
            updated = true;
        }
    } else {
        updated = tinydb_slotted_leaf_v2_update(page,
                                                PAGE_SIZE,
                                                id,
                                                envelope,
                                                (uint16_t)envelope_length);
    }

    if (updated) mark_page_dirty(table->pager, cursor->page_num);
    free(cursor);
    end_root_scope(table, previous_root);
    if (!updated) {
        set_message(message,
                    message_size,
                    "payload-native UPDATE failed without publishing a replacement row");
        return false;
    }

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_find(Table* table,
                        const TableSchema* schema,
                        uint32_t id,
                        TinyDBRecord* record) {
    if (table == NULL || schema == NULL || record == NULL) return false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema, message, sizeof(message))) {
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, id);
    bool found = cursor_key_equals(cursor, id) &&
                 cursor_to_record(schema, cursor, record);
    free(cursor);
    end_root_scope(table, previous_root);
    return found;
}

uint32_t tinydb_record_scan(Table* table,
                            const TableSchema* schema,
                            TinyDBRecordVisitor visitor,
                            void* context) {
    if (table == NULL || schema == NULL) return 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema, message, sizeof(message))) {
        return 0u;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_start(table);
    if (cursor == NULL) {
        end_root_scope(table, previous_root);
        return 0u;
    }

    uint32_t count = 0u;
    bool corrupt = false;
    while (!cursor->end_of_table) {
        TinyDBRecord record;
        if (!cursor_to_record(schema, cursor, &record)) {
            corrupt = true;
            break;
        }
        count++;
        if (visitor != NULL && !visitor(schema, &record, context)) break;
        if (!tinydb_leaf_read_advance_checked(cursor)) {
            corrupt = true;
            break;
        }
    }

    free(cursor);
    end_root_scope(table, previous_root);
    return corrupt ? 0u : count;
}
