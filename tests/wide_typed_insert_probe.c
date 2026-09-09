#include "record.h"
#include "record_payload.h"
#include "slotted_leaf_v2.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TableSchema wide_schema(uint32_t root_page_num) {
    TableSchema schema;
    memset(&schema, 0, sizeof(schema));
    snprintf(schema.name, sizeof(schema.name), "%s", "wide_typed_rows");
    schema.root_page_num = root_page_num;
    schema.num_columns = 3u;

    snprintf(schema.columns[0].name, sizeof(schema.columns[0].name), "%s", "id");
    schema.columns[0].type = COL_TYPE_INT;
    schema.columns[0].offset = 0u;
    schema.columns[0].size = 4u;

    snprintf(schema.columns[1].name, sizeof(schema.columns[1].name), "%s", "description");
    schema.columns[1].type = COL_TYPE_VARCHAR;
    schema.columns[1].offset = 4u;
    schema.columns[1].size = 300u;

    snprintf(schema.columns[2].name, sizeof(schema.columns[2].name), "%s", "score");
    schema.columns[2].type = COL_TYPE_INT;
    schema.columns[2].offset = 304u;
    schema.columns[2].size = 4u;
    schema.row_size = 308u;
    return schema;
}

static void make_values(TinyDBValue values[3],
                        uint32_t id,
                        const char* description,
                        uint32_t score) {
    memset(values, 0, sizeof(TinyDBValue) * 3u);
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "%s", description);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = score;
}

static bool expect_row(Table* table,
                       const TableSchema* schema,
                       uint32_t id,
                       const char* description,
                       uint32_t score) {
    TinyDBRecordPayload payload;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    memset(&payload, 0, sizeof(payload));
    memset(values, 0, sizeof(values));
    message[0] = '\0';

    if (!tinydb_record_payload_find(table,
                                    schema,
                                    id,
                                    &payload,
                                    message,
                                    sizeof(message)) ||
        payload.length != schema->row_size ||
        !tinydb_record_payload_decode_values(schema,
                                             &payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &value_count,
                                             message,
                                             sizeof(message))) {
        fprintf(stderr, "wide row lookup/decode failed for %u: %s\n", id, message);
        return false;
    }

    return value_count == 3u &&
           values[0].type == COL_TYPE_INT && values[0].int_value == id &&
           values[1].type == COL_TYPE_VARCHAR &&
           strcmp(values[1].text, description) == 0 &&
           values[2].type == COL_TYPE_INT && values[2].int_value == score;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: wide_typed_insert_probe <db>\n");
        return 1;
    }

    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return 2;

    TableSchema schema = wide_schema(table->root_page_num);
    void* root = get_page(table->pager, schema.root_page_num);
    if (schema.row_size <= ROW_SIZE ||
        !tinydb_record_payload_schema_supported(&schema, NULL, 0u) ||
        !tinydb_slotted_leaf_v2_init(root, PAGE_SIZE)) {
        fprintf(stderr, "unable to initialize 308-byte typed-row schema\n");
        db_close(table);
        remove(argv[1]);
        return 3;
    }
    set_node_root(root, true);

    const char* descriptions[3] = {
        "typed wide row one",
        "typed wide row two uses a distinct VARCHAR payload",
        "typed wide row three"
    };
    const uint32_t ids[3] = {7u, 3u, 11u};
    const uint32_t scores[3] = {70u, 30u, 110u};
    char message[TINYDB_RECORD_MESSAGE_MAX];
    for (uint32_t i = 0u; i < 3u; i++) {
        TinyDBValue values[3];
        make_values(values, ids[i], descriptions[i], scores[i]);
        message[0] = '\0';
        if (!tinydb_record_insert(table,
                                  &schema,
                                  values,
                                  3u,
                                  message,
                                  sizeof(message))) {
            fprintf(stderr, "typed wide insert failed for %u: %s\n", ids[i], message);
            db_close(table);
            remove(argv[1]);
            return 4;
        }
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        if (!expect_row(table, &schema, ids[i], descriptions[i], scores[i])) {
            db_close(table);
            remove(argv[1]);
            return 5;
        }
    }

    TinyDBValue duplicate[3];
    make_values(duplicate, 7u, "duplicate must not replace the stored row", 999u);
    message[0] = '\0';
    if (tinydb_record_insert(table,
                             &schema,
                             duplicate,
                             3u,
                             message,
                             sizeof(message)) ||
        strstr(message, "duplicate primary key") == NULL ||
        !expect_row(table, &schema, 7u, descriptions[0], scores[0])) {
        fprintf(stderr, "typed wide duplicate guard failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 6;
    }

    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) {
        remove(argv[1]);
        return 7;
    }
    schema = wide_schema(table->root_page_num);
    for (uint32_t i = 0u; i < 3u; i++) {
        if (!expect_row(table, &schema, ids[i], descriptions[i], scores[i])) {
            fprintf(stderr, "reopen lost typed wide row %u\n", ids[i]);
            db_close(table);
            remove(argv[1]);
            return 8;
        }
    }

    TinyDBRecord legacy;
    if (tinydb_record_find(table, &schema, 7u, &legacy)) {
        fprintf(stderr, "legacy fixed record unexpectedly accepted 308-byte row\n");
        db_close(table);
        remove(argv[1]);
        return 9;
    }

    db_close(table);
    remove(argv[1]);
    printf("WIDE_TYPED_INSERT_OK row_size=%u rows=3 duplicate_guard=1 reopen=1\n",
           schema.row_size);
    return 0;
}
