#include "generic_index_epoch.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_mutation_policy.h"
#include "leaf_page_access.h"
#include "record.h"
#include "record_payload.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool tinydb_record_insert_base(Table* table,
                               const TableSchema* schema,
                               const TinyDBValue* values,
                               uint32_t value_count,
                               char* message,
                               size_t message_size);

bool tinydb_record_delete_base(Table* table,
                               const TableSchema* schema,
                               uint32_t id,
                               char* message,
                               size_t message_size);

uint32_t tinydb_record_delete_all_base(Table* table,
                                       const TableSchema* schema,
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

static bool mutation_tree_is_fixed_v1(Table* table,
                                      const TableSchema* schema,
                                      char* message,
                                      size_t message_size) {
    if (schema == NULL) {
        set_message(message,
                    message_size,
                    "schema is required before generic mutation");
        return false;
    }
    return tinydb_leaf_tree_mutation_supported(table,
                                               schema->root_page_num,
                                               message,
                                               message_size);
}

static bool prepare_index_epoch(Table* table,
                                const TableSchema* schema,
                                char* message,
                                size_t message_size) {
    if (tinydb_generic_index_epoch_before_mutation(table, schema)) return true;
    set_message(message,
                message_size,
                "unable to persist generic-index mutation epoch");
    return false;
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

static bool cursor_matches_key(Cursor* cursor, uint32_t key) {
    if (cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= cursor->table->pager->num_pages) {
        return false;
    }
    void* page = get_page(cursor->table->pager, cursor->page_num);
    uint32_t found = 0u;
    return get_node_type(page) == NODE_LEAF &&
           tinydb_leaf_page_key_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &found) &&
           found == key;
}

static bool update_fixed_v1(Cursor* cursor,
                            const TinyDBRecordPayload* payload) {
    void* page = get_page(cursor->table->pager, cursor->page_num);
    const void* value = NULL;
    uint32_t stored_length = 0u;
    if (!tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE) ||
        !tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &value,
                                   &stored_length) ||
        value == NULL || stored_length != ROW_SIZE ||
        payload->length > stored_length) {
        return false;
    }

    void* writable = (void*)value;
    memset(writable, 0, stored_length);
    memcpy(writable, payload->bytes, payload->length);
    mark_page_dirty(cursor->table->pager, cursor->page_num);
    return true;
}

static bool existing_row_is_compact_v2(Cursor* cursor) {
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

static bool update_slotted_v2(Cursor* cursor,
                              const TableSchema* schema,
                              uint32_t key,
                              const TinyDBRecordPayload* payload) {
    void* page = get_page(cursor->table->pager, cursor->page_num);
    if (tinydb_leaf_format_detect_page(page, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !existing_row_is_compact_v2(cursor)) {
        return false;
    }

    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               payload,
                                               envelope,
                                               sizeof(envelope),
                                               &envelope_length) ||
        envelope_length == 0u || envelope_length > UINT16_MAX) {
        return false;
    }
    if (!tinydb_slotted_leaf_v2_update(page,
                                       PAGE_SIZE,
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length)) {
        return false;
    }
    mark_page_dirty(cursor->table->pager, cursor->page_num);
    return true;
}

static bool update_existing_mixed(Table* table,
                                  const TableSchema* schema,
                                  uint32_t id,
                                  const TinyDBValue* values,
                                  uint32_t value_count,
                                  char* message,
                                  size_t message_size) {
    if (table == NULL || schema == NULL) {
        set_message(message, message_size, "table and schema are required");
        return false;
    }
    if (!tinydb_schema_supports_records(schema, message, message_size)) {
        return false;
    }
    if (ci_equal(schema->name, "users")) {
        set_message(message,
                    message_size,
                    "use the legacy users execution path so its secondary indexes stay synchronized");
        return false;
    }

    TinyDBRecord record;
    if (!tinydb_record_encode(schema,
                              values,
                              value_count,
                              &record,
                              message,
                              message_size)) {
        return false;
    }
    TinyDBRecordPayload payload;
    if (!tinydb_record_payload_from_record(schema,
                                           &record,
                                           &payload,
                                           message,
                                           message_size)) {
        return false;
    }
    uint32_t encoded_id = 0u;
    memcpy(&encoded_id, payload.bytes, sizeof(encoded_id));
    if (encoded_id != id) {
        set_message(message,
                    message_size,
                    "generic UPDATE cannot change the primary-key id");
        return false;
    }

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, id);
    if (!cursor_matches_key(cursor, id)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "primary key not found");
        return false;
    }

    void* page = get_page(table->pager, cursor->page_num);
    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    bool updated = false;
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        updated = update_fixed_v1(cursor, &payload);
    } else if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        updated = update_slotted_v2(cursor, schema, id, &payload);
    }

    free(cursor);
    end_root_scope(table, previous_root);
    if (!updated) {
        set_message(message,
                    message_size,
                    format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2
                        ? "unable to update logical leaf value: slotted V2 rows are read-only unless already stored as compact envelope V2 and the replacement fits without splitting"
                        : "unable to update existing logical leaf value");
        return false;
    }

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

static bool insert_key_preserves_tree_boundaries(Table* table,
                                                  uint32_t page_num,
                                                  const void* page,
                                                  uint32_t key) {
    if (table == NULL || table->pager == NULL || page == NULL ||
        page_num >= table->pager->num_pages) {
        return false;
    }

    uint32_t count = 0u;
    uint32_t last_key = 0u;
    if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count) || count == 0u ||
        !tinydb_leaf_page_key_at(page,
                                 PAGE_SIZE,
                                 count - 1u,
                                 &last_key)) {
        return false;
    }

    /* Internal separators are the maximum key of the child to their left.
     * Therefore a non-splitting insert is topology-neutral as long as it does
     * not increase this leaf's maximum key. Root leaves have no parent
     * separator, but the same bound keeps this narrow fast path predictable. */
    if (key >= last_key) return false;

    uint32_t prev_page_num = 0u;
    if (!tinydb_leaf_page_prev(page, PAGE_SIZE, &prev_page_num)) return false;
    if (prev_page_num == 0u) return true;
    if (prev_page_num >= table->pager->num_pages || prev_page_num == page_num) {
        return false;
    }

    void* prev_page = get_page(table->pager, prev_page_num);
    uint32_t prev_next = 0u;
    uint32_t prev_count = 0u;
    uint32_t prev_last_key = 0u;
    if (!tinydb_leaf_page_next(prev_page, PAGE_SIZE, &prev_next) ||
        prev_next != page_num ||
        !tinydb_leaf_page_count(prev_page, PAGE_SIZE, &prev_count) ||
        prev_count == 0u ||
        !tinydb_leaf_page_key_at(prev_page,
                                 PAGE_SIZE,
                                 prev_count - 1u,
                                 &prev_last_key)) {
        return false;
    }

    /* A lower-boundary insert may move this leaf's first key leftward, but it
     * must remain strictly above the previous sibling's maximum key. */
    return key > prev_last_key;
}

static bool insert_non_split_slotted_v2(Table* table,
                                        const TableSchema* schema,
                                        const TinyDBValue* values,
                                        uint32_t value_count,
                                        char* message,
                                        size_t message_size) {
    if (table == NULL || schema == NULL ||
        !tinydb_schema_supports_records(schema, message, message_size)) {
        return false;
    }
    if (ci_equal(schema->name, "users")) {
        set_message(message,
                    message_size,
                    "use the legacy users execution path so its secondary indexes stay synchronized");
        return false;
    }

    TinyDBRecord record;
    if (!tinydb_record_encode(schema,
                              values,
                              value_count,
                              &record,
                              message,
                              message_size)) {
        return false;
    }
    TinyDBRecordPayload payload;
    if (!tinydb_record_payload_from_record(schema,
                                           &record,
                                           &payload,
                                           message,
                                           message_size)) {
        return false;
    }

    uint32_t key = 0u;
    memcpy(&key, payload.bytes, sizeof(key));
    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    if (cursor == NULL) {
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "unable to locate target leaf for slotted V2 insert");
        return false;
    }
    if (cursor_matches_key(cursor, key)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "duplicate primary key");
        return false;
    }

    void* page = get_page(table->pager, cursor->page_num);
    if (tinydb_leaf_format_detect_page(page, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) ||
        !insert_key_preserves_tree_boundaries(table,
                                              cursor->page_num,
                                              page,
                                              key)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 insert is allowed only when the target V2 leaf can accept the key without changing a parent separator or crossing a sibling key boundary");
        return false;
    }

    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               &payload,
                                               envelope,
                                               sizeof(envelope),
                                               &envelope_length) ||
        envelope_length == 0u || envelope_length > UINT16_MAX ||
        TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length >
            tinydb_slotted_leaf_v2_free_bytes(page, PAGE_SIZE)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 insert requires existing free space and cannot split the leaf");
        return false;
    }

    if (!prepare_index_epoch(table, schema, message, message_size)) {
        free(cursor);
        end_root_scope(table, previous_root);
        return false;
    }

    unsigned char before[PAGE_USABLE_SIZE];
    memcpy(before, page, sizeof(before));
    bool inserted = tinydb_slotted_leaf_v2_insert(page,
                                                  PAGE_SIZE,
                                                  key,
                                                  envelope,
                                                  (uint16_t)envelope_length);
    if (!inserted) {
        memcpy(page, before, sizeof(before));
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 non-split insert failed without modifying the page");
        return false;
    }

    mark_page_dirty(table->pager, cursor->page_num);
    free(cursor);
    end_root_scope(table, previous_root);
    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

static bool insert_schema_sized_payload(Table* table,
                                        const TableSchema* schema,
                                        const TinyDBValue* values,
                                        uint32_t value_count,
                                        char* message,
                                        size_t message_size) {
    if (table == NULL || schema == NULL) {
        set_message(message,
                    message_size,
                    "table and schema are required before generic mutation");
        return false;
    }
    if (schema->row_size <= ROW_SIZE) return false;

    TinyDBRecordPayload payload;
    if (!tinydb_record_payload_encode_values(schema,
                                             values,
                                             value_count,
                                             &payload,
                                             message,
                                             message_size)) {
        return false;
    }
    return tinydb_record_payload_insert(table,
                                        schema,
                                        &payload,
                                        message,
                                        message_size);
}

bool tinydb_record_insert(Table* table,
                          const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    /* The legacy TinyDBRecord carrier remains exactly ROW_SIZE bytes. Do not
     * make wide generic SQL rows detour through that compatibility object:
     * encode their declared schema layout directly into a sized payload and
     * hand it to the payload-native V2 INSERT path. Narrow schemas retain the
     * established V1/mixed-leaf routing below. */
    if (schema != NULL && schema->row_size > ROW_SIZE) {
        return insert_schema_sized_payload(table,
                                           schema,
                                           values,
                                           value_count,
                                           message,
                                           message_size);
    }

    char policy_message[TINYDB_RECORD_MESSAGE_MAX];
    if (mutation_tree_is_fixed_v1(table,
                                  schema,
                                  policy_message,
                                  sizeof(policy_message))) {
        if (!prepare_index_epoch(table, schema, message, message_size)) return false;
        return tinydb_record_insert_base(table,
                                         schema,
                                         values,
                                         value_count,
                                         message,
                                         message_size);
    }

    if (insert_non_split_slotted_v2(table,
                                    schema,
                                    values,
                                    value_count,
                                    message,
                                    message_size)) {
        return true;
    }
    if (message != NULL && message_size > 0u && message[0] == '\0') {
        set_message(message,
                    message_size,
                    policy_message[0] != '\0'
                        ? policy_message
                        : "generic insert is not supported for this leaf layout");
    }
    return false;
}

bool tinydb_record_update(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    /* Existing-row UPDATE does not change the B+ tree key set, separators, or
     * sibling topology. Fixed V1 values may always be rewritten in place.
     * Slotted V2 writes require an already-explicit compact V2 envelope so a
     * normal UPDATE cannot silently migrate older raw/envelope formats. */
    if (table == NULL || schema == NULL) {
        set_message(message,
                    message_size,
                    "table and schema are required before generic mutation");
        return false;
    }
    if (!prepare_index_epoch(table, schema, message, message_size)) return false;
    return update_existing_mixed(table,
                                 schema,
                                 id,
                                 values,
                                 value_count,
                                 message,
                                 message_size);
}

bool tinydb_record_delete(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          char* message,
                          size_t message_size) {
    if (!mutation_tree_is_fixed_v1(table, schema, message, message_size)) {
        return false;
    }
    if (!prepare_index_epoch(table, schema, message, message_size)) return false;
    return tinydb_record_delete_base(table, schema, id, message, message_size);
}

uint32_t tinydb_record_delete_all(Table* table,
                                  const TableSchema* schema,
                                  char* message,
                                  size_t message_size) {
    if (!mutation_tree_is_fixed_v1(table, schema, message, message_size)) {
        return 0u;
    }
    if (!prepare_index_epoch(table, schema, message, message_size)) return 0u;
    return tinydb_record_delete_all_base(table, schema, message, message_size);
}
