#include "record.h"
#include "record_payload.h"
#include "leaf_value.h"

#include <ctype.h>

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static void set_message(char* message,
                        size_t message_size,
                        const char* format,
                        const char* detail) {
    if (message == NULL || message_size == 0) return;
    if (detail != NULL) {
        snprintf(message, message_size, format, detail);
    } else {
        snprintf(message, message_size, "%s", format);
    }
}

bool tinydb_schema_supports_records(const TableSchema* schema,
                                    char* message,
                                    size_t message_size) {
    if (message != NULL && message_size > 0) message[0] = '\0';
    if (schema == NULL) {
        set_message(message, message_size, "schema is required", NULL);
        return false;
    }
    if (schema->num_columns == 0 || schema->num_columns > MAX_COLUMNS_PER_TABLE) {
        set_message(message, message_size, "schema has an invalid column count", NULL);
        return false;
    }
    if (schema->row_size == 0 || schema->row_size > ROW_SIZE) {
        set_message(message,
                    message_size,
                    "schema row does not fit the current fixed B+ tree value slot",
                    NULL);
        return false;
    }
    if (!ci_equal(schema->columns[0].name, "id") ||
        schema->columns[0].type != COL_TYPE_INT ||
        schema->columns[0].offset != 0 ||
        schema->columns[0].size != sizeof(uint32_t)) {
        set_message(message,
                    message_size,
                    "generic records currently require the first column to be id INT",
                    NULL);
        return false;
    }

    uint32_t expected_offset = 0;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        if (column->size == 0 || column->offset > schema->row_size ||
            column->size > schema->row_size - column->offset) {
            set_message(message,
                        message_size,
                        "schema contains a column outside its serialized row bounds",
                        NULL);
            return false;
        }
        if (column->offset != expected_offset) {
            set_message(message,
                        message_size,
                        "schema columns must form a contiguous non-overlapping serialized row layout",
                        NULL);
            return false;
        }
        if (column->type == COL_TYPE_INT && column->size != sizeof(uint32_t)) {
            set_message(message,
                        message_size,
                        "INT columns must use a 4-byte serialized representation",
                        NULL);
            return false;
        }
        if (column->type != COL_TYPE_INT && column->type != COL_TYPE_VARCHAR) {
            set_message(message, message_size, "schema contains an unsupported column type", NULL);
            return false;
        }
        expected_offset += column->size;
    }
    if (expected_offset != schema->row_size) {
        set_message(message,
                    message_size,
                    "schema row size must match the serialized column layout",
                    NULL);
        return false;
    }
    return true;
}

bool tinydb_record_encode(const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          TinyDBRecord* record,
                          char* message,
                          size_t message_size) {
    if (!tinydb_schema_supports_records(schema, message, message_size)) return false;
    if (values == NULL || record == NULL || value_count != schema->num_columns) {
        set_message(message,
                    message_size,
                    "record value count does not match the table schema",
                    NULL);
        return false;
    }

    memset(record, 0, sizeof(*record));
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        const TinyDBValue* value = &values[i];
        unsigned char* destination = record->bytes + column->offset;

        if (value->type != column->type) {
            set_message(message,
                        message_size,
                        "record value type does not match column schema",
                        NULL);
            return false;
        }

        if (column->type == COL_TYPE_INT) {
            memcpy(destination, &value->int_value, sizeof(value->int_value));
        } else {
            size_t length = strlen(value->text);
            if (length >= column->size) {
                set_message(message,
                            message_size,
                            "VARCHAR value exceeds its serialized column capacity",
                            NULL);
                return false;
            }
            memcpy(destination, value->text, length);
            destination[length] = '\0';
        }
    }
    return true;
}

bool tinydb_record_decode(const TableSchema* schema,
                          const TinyDBRecord* record,
                          TinyDBValue* values,
                          uint32_t value_capacity,
                          uint32_t* value_count,
                          char* message,
                          size_t message_size) {
    if (!tinydb_schema_supports_records(schema, message, message_size)) return false;
    if (record == NULL || values == NULL || value_capacity < schema->num_columns) {
        set_message(message, message_size, "record decode buffer is too small", NULL);
        return false;
    }

    for (uint32_t i = 0; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        const unsigned char* source = record->bytes + column->offset;
        TinyDBValue* value = &values[i];
        memset(value, 0, sizeof(*value));
        value->type = column->type;

        if (column->type == COL_TYPE_INT) {
            memcpy(&value->int_value, source, sizeof(value->int_value));
        } else {
            uint32_t length = 0;
            while (length < column->size && source[length] != '\0') length++;
            if (length == column->size) {
                set_message(message,
                            message_size,
                            "serialized VARCHAR field is not NUL-terminated within its column capacity",
                            NULL);
                if (value_count != NULL) *value_count = 0;
                return false;
            }
            if (length >= sizeof(value->text)) length = (uint32_t)sizeof(value->text) - 1u;
            memcpy(value->text, source, length);
            value->text[length] = '\0';
        }
    }

    if (value_count != NULL) *value_count = schema->num_columns;
    return true;
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

static bool mutation_target_supported(const TableSchema* schema,
                                      char* message,
                                      size_t message_size) {
    if (!tinydb_schema_supports_records(schema, message, message_size)) return false;
    if (ci_equal(schema->name, "users")) {
        set_message(message,
                    message_size,
                    "use the legacy users execution path so its secondary indexes stay synchronized",
                    NULL);
        return false;
    }
    return true;
}

static bool cursor_matches_key(Table* table, Cursor* cursor, uint32_t key) {
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    return cursor->cell_num < num_cells &&
           *leaf_node_key(node, cursor->cell_num) == key;
}

static void commit_if_autocommit(Table* table) {
    if (!table->in_transaction) pager_commit(table->pager);
}

static bool record_to_payload(const TableSchema* schema,
                              const TinyDBRecord* record,
                              TinyDBRecordPayload* payload,
                              char* message,
                              size_t message_size) {
    return tinydb_record_payload_from_record(schema,
                                             record,
                                             payload,
                                             message,
                                             message_size);
}

static bool cursor_to_record(const TableSchema* schema,
                             Cursor* cursor,
                             TinyDBRecord* record,
                             char* message,
                             size_t message_size) {
    TinyDBRecordPayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.length = schema->row_size;
    if (!tinydb_leaf_value_read(cursor,
                                payload.bytes,
                                sizeof(payload.bytes),
                                payload.length)) {
        set_message(message, message_size, "unable to read logical leaf value", NULL);
        return false;
    }
    return tinydb_record_payload_to_record(schema,
                                           &payload,
                                           record,
                                           message,
                                           message_size);
}

bool tinydb_record_insert(Table* table,
                          const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    if (table == NULL || schema == NULL) {
        set_message(message, message_size, "table and schema are required", NULL);
        return false;
    }
    if (!mutation_target_supported(schema, message, message_size)) return false;

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
    if (!record_to_payload(schema,
                           &record,
                           &payload,
                           message,
                           message_size)) {
        return false;
    }

    uint32_t key = 0;
    memcpy(&key, payload.bytes, sizeof(key));
    uint32_t previous_root = 0;
    begin_root_scope(table, schema, &previous_root);

    Cursor* cursor = table_find(table, key);
    if (cursor_matches_key(table, cursor, key)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "duplicate primary key", NULL);
        return false;
    }

    if (!tinydb_leaf_value_insert(cursor,
                                  key,
                                  payload.bytes,
                                  payload.length)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "unable to insert logical leaf value", NULL);
        return false;
    }
    free(cursor);
    end_root_scope(table, previous_root);

    commit_if_autocommit(table);
    if (message != NULL && message_size > 0) message[0] = '\0';
    return true;
}

bool tinydb_record_update(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    if (table == NULL || schema == NULL) {
        set_message(message, message_size, "table and schema are required", NULL);
        return false;
    }
    if (!mutation_target_supported(schema, message, message_size)) return false;

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
    if (!record_to_payload(schema,
                           &record,
                           &payload,
                           message,
                           message_size)) {
        return false;
    }

    uint32_t encoded_id = 0;
    memcpy(&encoded_id, payload.bytes, sizeof(encoded_id));
    if (encoded_id != id) {
        set_message(message,
                    message_size,
                    "generic UPDATE cannot change the primary-key id",
                    NULL);
        return false;
    }

    uint32_t previous_root = 0;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = table_find(table, id);
    if (!cursor_matches_key(table, cursor, id)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "primary key not found", NULL);
        return false;
    }

    if (!tinydb_leaf_value_write(cursor, payload.bytes, payload.length)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "unable to update logical leaf value", NULL);
        return false;
    }
    free(cursor);
    end_root_scope(table, previous_root);

    commit_if_autocommit(table);
    if (message != NULL && message_size > 0) message[0] = '\0';
    return true;
}

bool tinydb_record_delete(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          char* message,
                          size_t message_size) {
    if (table == NULL || schema == NULL) {
        set_message(message, message_size, "table and schema are required", NULL);
        return false;
    }
    if (!mutation_target_supported(schema, message, message_size)) return false;

    uint32_t previous_root = 0;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = table_find(table, id);
    if (!cursor_matches_key(table, cursor, id)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "primary key not found", NULL);
        return false;
    }

    leaf_node_delete(cursor);
    free(cursor);
    end_root_scope(table, previous_root);

    commit_if_autocommit(table);
    if (message != NULL && message_size > 0) message[0] = '\0';
    return true;
}

uint32_t tinydb_record_delete_all(Table* table,
                                  const TableSchema* schema,
                                  char* message,
                                  size_t message_size) {
    if (table == NULL || schema == NULL) {
        set_message(message, message_size, "table and schema are required", NULL);
        return 0;
    }
    if (!mutation_target_supported(schema, message, message_size)) return 0;

    uint32_t previous_root = 0;
    begin_root_scope(table, schema, &previous_root);
    uint32_t deleted = 0;

    while (true) {
        Cursor* cursor = table_start(table);
        if (cursor->end_of_table) {
            free(cursor);
            break;
        }
        leaf_node_delete(cursor);
        free(cursor);
        deleted++;
    }

    end_root_scope(table, previous_root);
    commit_if_autocommit(table);
    if (message != NULL && message_size > 0) message[0] = '\0';
    return deleted;
}

bool tinydb_record_find(Table* table,
                        const TableSchema* schema,
                        uint32_t id,
                        TinyDBRecord* record) {
    if (table == NULL || schema == NULL || record == NULL) return false;
    char ignored[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema, ignored, sizeof(ignored))) return false;

    uint32_t previous_root = 0;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = table_find(table, id);
    bool found = cursor_matches_key(table, cursor, id);
    if (found) {
        found = cursor_to_record(schema,
                                 cursor,
                                 record,
                                 ignored,
                                 sizeof(ignored));
    }
    free(cursor);
    end_root_scope(table, previous_root);
    return found;
}

uint32_t tinydb_record_scan(Table* table,
                            const TableSchema* schema,
                            TinyDBRecordVisitor visitor,
                            void* context) {
    if (table == NULL || schema == NULL) return 0;
    char ignored[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema, ignored, sizeof(ignored))) return 0;

    uint32_t previous_root = 0;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = table_start(table);
    uint32_t count = 0;
    while (!cursor->end_of_table) {
        TinyDBRecord record;
        if (!cursor_to_record(schema,
                              cursor,
                              &record,
                              ignored,
                              sizeof(ignored))) {
            break;
        }
        count++;
        if (visitor != NULL && !visitor(schema, &record, context)) break;
        cursor_advance(cursor);
    }
    free(cursor);
    end_root_scope(table, previous_root);
    return count;
}

void tinydb_record_print(const TableSchema* schema,
                         const TinyDBRecord* record) {
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_decode(schema,
                              record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &count,
                              message,
                              sizeof(message))) {
        printf("<invalid record: %s>\n", message);
        return;
    }

    printf("(");
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0) printf(", ");
        if (values[i].type == COL_TYPE_INT) {
            printf("%u", values[i].int_value);
        } else {
            printf("%s", values[i].text);
        }
    }
    printf(")\n");
}
