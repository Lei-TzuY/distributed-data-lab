#include "record.h"
#include "record_payload.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TableSchema wide_schema(uint32_t root_page_num) {
    TableSchema schema;
    memset(&schema, 0, sizeof(schema));
    snprintf(schema.name, sizeof(schema.name), "%s", "wide_payloads");
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

static bool make_payload(const TableSchema* schema,
                         uint32_t id,
                         const char* description,
                         uint32_t score,
                         TinyDBRecordPayload* payload) {
    TinyDBValue values[3];
    char message[TINYDB_RECORD_MESSAGE_MAX];
    memset(values, 0, sizeof(values));
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "%s", description);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = score;
    return tinydb_record_payload_encode_values(schema,
                                               values,
                                               3u,
                                               payload,
                                               message,
                                               sizeof(message));
}

static bool insert_wide(void* page,
                        const TableSchema* schema,
                        uint32_t id,
                        const char* description,
                        uint32_t score) {
    TinyDBRecordPayload payload;
    unsigned char envelope[512];
    uint32_t envelope_length = 0u;
    if (!make_payload(schema, id, description, score, &payload) ||
        !tinydb_row_envelope_encode_compact_v2(schema,
                                               &payload,
                                               envelope,
                                               sizeof(envelope),
                                               &envelope_length) ||
        envelope_length == 0u || envelope_length > UINT16_MAX) {
        return false;
    }
    return tinydb_slotted_leaf_v2_insert(page,
                                         PAGE_SIZE,
                                         id,
                                         envelope,
                                         (uint16_t)envelope_length);
}

static bool expect_payload(Table* table,
                           const TableSchema* schema,
                           uint32_t id,
                           const char* description,
                           uint32_t score) {
    TinyDBRecordPayload payload;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_find(table,
                                    schema,
                                    id,
                                    &payload,
                                    message,
                                    sizeof(message)) ||
        !tinydb_record_payload_decode_values(schema,
                                             &payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &value_count,
                                             message,
                                             sizeof(message))) {
        fprintf(stderr, "payload lookup/decode failed for %u: %s\n", id, message);
        return false;
    }
    return value_count == 3u &&
           values[0].int_value == id &&
           strcmp(values[1].text, description) == 0 &&
           values[2].int_value == score;
}

static bool payload_missing(Table* table,
                            const TableSchema* schema,
                            uint32_t id) {
    TinyDBRecordPayload payload;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return !tinydb_record_payload_find(table,
                                       schema,
                                       id,
                                       &payload,
                                       message,
                                       sizeof(message));
}

static bool fill_until_tight(void* page, const TableSchema* schema) {
    uint32_t id = 1000u;
    while (tinydb_slotted_leaf_v2_free_bytes(page, PAGE_SIZE) >= 128u) {
        if (!insert_wide(page, schema, id, "x", id + 1u)) break;
        id++;
        if (id > 2000u) return false;
    }
    return tinydb_slotted_leaf_v2_free_bytes(page, PAGE_SIZE) < 256u;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_native_update_probe <db>\n");
        return 1;
    }
    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return 1;

    uint32_t root = table->root_page_num;
    void* page = get_page(table->pager, root);
    TableSchema schema = wide_schema(root);
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(&schema, message, sizeof(message)) ||
        schema.row_size <= ROW_SIZE ||
        !tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) {
        fprintf(stderr, "wide schema/V2 initialization failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }
    set_node_root(page, true);

    if (!insert_wide(page, &schema, 7u, "alpha", 70u) ||
        !insert_wide(page, &schema, 19u, "beta", 190u) ||
        !insert_wide(page, &schema, 31u, "gamma", 310u)) {
        fprintf(stderr, "wide V2 seed failed\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    const char* updated_text = "updated beta payload without legacy row narrowing";
    TinyDBRecordPayload updated;
    if (!make_payload(&schema, 19u, updated_text, 1919u, &updated) ||
        !tinydb_record_payload_update(table,
                                      &schema,
                                      19u,
                                      &updated,
                                      message,
                                      sizeof(message)) ||
        !expect_payload(table, &schema, 19u, updated_text, 1919u) ||
        !expect_payload(table, &schema, 7u, "alpha", 70u) ||
        !expect_payload(table, &schema, 31u, "gamma", 310u)) {
        fprintf(stderr, "payload-native wide update failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    TinyDBRecordPayload wrong_key;
    if (!make_payload(&schema, 20u, "must not publish", 2020u, &wrong_key) ||
        tinydb_record_payload_update(table,
                                     &schema,
                                     19u,
                                     &wrong_key,
                                     message,
                                     sizeof(message)) ||
        !expect_payload(table, &schema, 19u, updated_text, 1919u)) {
        fprintf(stderr, "primary-key guard failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    page = get_page(table->pager, root);
    if (!fill_until_tight(page, &schema)) {
        fprintf(stderr, "unable to make V2 page tight enough for growth rejection\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    char long_text[TINYDB_RECORD_TEXT_MAX + 1u];
    memset(long_text, 'z', TINYDB_RECORD_TEXT_MAX - 1u);
    long_text[TINYDB_RECORD_TEXT_MAX - 1u] = '\0';
    TinyDBRecordPayload too_large;
    if (!make_payload(&schema, 19u, long_text, 9999u, &too_large) ||
        tinydb_record_payload_update(table,
                                     &schema,
                                     19u,
                                     &too_large,
                                     message,
                                     sizeof(message)) ||
        !expect_payload(table, &schema, 19u, updated_text, 1919u)) {
        fprintf(stderr, "capacity fail-closed guard failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) {
        remove(argv[1]);
        return 1;
    }
    schema = wide_schema(table->root_page_num);
    if (!expect_payload(table, &schema, 19u, updated_text, 1919u)) {
        fprintf(stderr, "reopen lost payload-native update\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    TinyDBRecord legacy;
    if (tinydb_record_find(table, &schema, 19u, &legacy)) {
        fprintf(stderr, "legacy TinyDBRecord unexpectedly accepted 308-byte updated row\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!tinydb_record_payload_delete(table,
                                      &schema,
                                      19u,
                                      message,
                                      sizeof(message)) ||
        !payload_missing(table, &schema, 19u) ||
        !expect_payload(table, &schema, 7u, "alpha", 70u) ||
        !expect_payload(table, &schema, 31u, "gamma", 310u)) {
        fprintf(stderr, "payload-native wide delete failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) {
        remove(argv[1]);
        return 1;
    }
    schema = wide_schema(table->root_page_num);
    if (!payload_missing(table, &schema, 19u) ||
        !expect_payload(table, &schema, 7u, "alpha", 70u) ||
        !expect_payload(table, &schema, 31u, "gamma", 310u)) {
        fprintf(stderr, "reopen lost payload-native delete\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    db_close(table);
    remove(argv[1]);
    printf("PAYLOAD_NATIVE_UPDATE_OK row_size=308 pk_guard=1 capacity_guard=1 reopen=1 delete_reopen=1\n");
    return 0;
}