#include "leaf_format.h"
#include "leaf_page_access.h"
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

static uint32_t read_u32(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
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

static bool insert_payload(Table* table,
                           const TableSchema* schema,
                           uint32_t id,
                           const char* description,
                           uint32_t score,
                           char* message,
                           size_t message_size) {
    TinyDBRecordPayload payload;
    return make_payload(schema, id, description, score, &payload) &&
           tinydb_record_payload_insert(table,
                                        schema,
                                        &payload,
                                        message,
                                        message_size);
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

static uint32_t payload_count(Table* table, const TableSchema* schema) {
    bool complete = false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    uint32_t count = tinydb_record_payload_scan(table,
                                                schema,
                                                NULL,
                                                NULL,
                                                &complete,
                                                message,
                                                sizeof(message));
    return complete ? count : UINT32_MAX;
}

static uint32_t root_key_count(Table* table, const TableSchema* schema) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return UINT32_MAX;
    }
    unsigned char* root =
        (unsigned char*)get_page(table->pager, schema->root_page_num);
    if (get_node_type(root) != NODE_INTERNAL || root[IS_ROOT_OFFSET] == 0u) {
        return UINT32_MAX;
    }
    return read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

static bool root_has_v2_leaf_children(Table* table,
                                      const TableSchema* schema,
                                      uint32_t expected_children) {
    if (expected_children < 2u || expected_children > 3u ||
        table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           PAGE_SIZE);
    uint32_t key_count = read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (get_node_type(root) != NODE_INTERNAL || root[IS_ROOT_OFFSET] == 0u ||
        read_u32(root + PARENT_POINTER_OFFSET) != 0u ||
        key_count + 1u != expected_children) {
        return false;
    }

    uint32_t page_nums[3] = {0u, 0u, 0u};
    uint32_t separators[2] = {0u, 0u};
    for (uint32_t i = 0u; i < key_count; i++) {
        page_nums[i] = read_u32(root + INTERNAL_NODE_HEADER_SIZE +
                                i * INTERNAL_NODE_CELL_SIZE);
        separators[i] = read_u32(root + INTERNAL_NODE_HEADER_SIZE +
                                 i * INTERNAL_NODE_CELL_SIZE +
                                 INTERNAL_NODE_CHILD_SIZE);
    }
    page_nums[key_count] = read_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET);

    uint32_t mins[3] = {0u, 0u, 0u};
    uint32_t maxes[3] = {0u, 0u, 0u};
    for (uint32_t i = 0u; i < expected_children; i++) {
        if (page_nums[i] == 0u || page_nums[i] >= table->pager->num_pages) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (page_nums[j] == page_nums[i]) return false;
        }

        unsigned char leaf[PAGE_SIZE];
        memcpy(leaf, get_page(table->pager, page_nums[i]), PAGE_SIZE);
        uint32_t count = 0u;
        uint32_t prev = INVALID_PAGE_NUM;
        uint32_t next = INVALID_PAGE_NUM;
        if (tinydb_leaf_format_detect_page(leaf, PAGE_SIZE) !=
                TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE) ||
            leaf[IS_ROOT_OFFSET] != 0u ||
            read_u32(leaf + PARENT_POINTER_OFFSET) != schema->root_page_num ||
            !tinydb_leaf_page_count(leaf, PAGE_SIZE, &count) || count == 0u ||
            !tinydb_leaf_page_key_at(leaf, PAGE_SIZE, 0u, &mins[i]) ||
            !tinydb_leaf_page_key_at(leaf, PAGE_SIZE, count - 1u, &maxes[i]) ||
            !tinydb_leaf_page_prev(leaf, PAGE_SIZE, &prev) ||
            !tinydb_leaf_page_next(leaf, PAGE_SIZE, &next) ||
            prev != (i == 0u ? 0u : page_nums[i - 1u]) ||
            next != (i + 1u == expected_children ? 0u : page_nums[i + 1u])) {
            return false;
        }
        if (i > 0u && maxes[i - 1u] >= mins[i]) return false;
    }

    for (uint32_t i = 0u; i < key_count; i++) {
        if (separators[i] != maxes[i]) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_native_insert_probe <db>\n");
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

    if (!insert_payload(table, &schema, 10u, "ten", 100u, message, sizeof(message)) ||
        !insert_payload(table, &schema, 5u, "five", 50u, message, sizeof(message)) ||
        !insert_payload(table, &schema, 20u, "twenty", 200u, message, sizeof(message)) ||
        payload_count(table, &schema) != 3u ||
        !expect_payload(table, &schema, 5u, "five", 50u) ||
        !expect_payload(table, &schema, 10u, "ten", 100u) ||
        !expect_payload(table, &schema, 20u, "twenty", 200u)) {
        fprintf(stderr, "payload-native root-leaf insertion failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (insert_payload(table,
                       &schema,
                       10u,
                       "duplicate must not publish",
                       999u,
                       message,
                       sizeof(message)) ||
        strstr(message, "duplicate primary key") == NULL ||
        payload_count(table, &schema) != 3u ||
        !expect_payload(table, &schema, 10u, "ten", 100u)) {
        fprintf(stderr, "duplicate insert was not fail-closed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    char long_text[TINYDB_RECORD_TEXT_MAX + 1u];
    memset(long_text, 'w', TINYDB_RECORD_TEXT_MAX - 1u);
    long_text[TINYDB_RECORD_TEXT_MAX - 1u] = '\0';

    uint32_t root_split_key = 0u;
    uint32_t next_id = 1000u;
    while (get_node_type(get_page(table->pager, schema.root_page_num)) == NODE_LEAF) {
        if (!insert_payload(table,
                            &schema,
                            next_id,
                            long_text,
                            next_id + 1u,
                            message,
                            sizeof(message))) {
            fprintf(stderr, "payload-native root split trigger failed: %s\n", message);
            db_close(table);
            remove(argv[1]);
            return 1;
        }
        root_split_key = next_id;
        next_id += 10u;
        if (next_id > 10000u) {
            fprintf(stderr, "payload-native root never split\n");
            db_close(table);
            remove(argv[1]);
            return 1;
        }
    }

    if (root_split_key == 0u || root_key_count(table, &schema) != 1u ||
        !root_has_v2_leaf_children(table, &schema, 2u) ||
        !expect_payload(table,
                        &schema,
                        root_split_key,
                        long_text,
                        root_split_key + 1u)) {
        fprintf(stderr, "payload-native root split topology/data check failed\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    uint32_t nonroot_split_key = 0u;
    while (root_key_count(table, &schema) == 1u) {
        if (!insert_payload(table,
                            &schema,
                            next_id,
                            long_text,
                            next_id + 1u,
                            message,
                            sizeof(message))) {
            fprintf(stderr, "payload-native non-root split trigger failed: %s\n", message);
            db_close(table);
            remove(argv[1]);
            return 1;
        }
        nonroot_split_key = next_id;
        next_id += 10u;
        if (next_id > 20000u) {
            fprintf(stderr, "payload-native non-root leaf never split\n");
            db_close(table);
            remove(argv[1]);
            return 1;
        }
    }

    uint32_t count_after_nonroot_split = payload_count(table, &schema);
    if (nonroot_split_key == 0u || root_key_count(table, &schema) != 2u ||
        count_after_nonroot_split == UINT32_MAX ||
        !root_has_v2_leaf_children(table, &schema, 3u) ||
        !expect_payload(table,
                        &schema,
                        nonroot_split_key,
                        long_text,
                        nonroot_split_key + 1u) ||
        !expect_payload(table, &schema, 5u, "five", 50u) ||
        !expect_payload(table, &schema, 10u, "ten", 100u) ||
        !expect_payload(table, &schema, 20u, "twenty", 200u)) {
        fprintf(stderr, "payload-native non-root split topology/data check failed\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    uint32_t post_split_key = next_id;
    if (!insert_payload(table,
                        &schema,
                        post_split_key,
                        "post split tail append",
                        post_split_key + 1u,
                        message,
                        sizeof(message)) ||
        root_key_count(table, &schema) != 2u ||
        payload_count(table, &schema) != count_after_nonroot_split + 1u ||
        !root_has_v2_leaf_children(table, &schema, 3u) ||
        !expect_payload(table,
                        &schema,
                        post_split_key,
                        "post split tail append",
                        post_split_key + 1u)) {
        fprintf(stderr, "post-nonroot-split tail append failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    uint32_t final_count = count_after_nonroot_split + 1u;
    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) {
        remove(argv[1]);
        return 1;
    }
    schema = wide_schema(table->root_page_num);
    if (payload_count(table, &schema) != final_count ||
        root_key_count(table, &schema) != 2u ||
        !root_has_v2_leaf_children(table, &schema, 3u) ||
        !expect_payload(table,
                        &schema,
                        root_split_key,
                        long_text,
                        root_split_key + 1u) ||
        !expect_payload(table,
                        &schema,
                        nonroot_split_key,
                        long_text,
                        nonroot_split_key + 1u) ||
        !expect_payload(table,
                        &schema,
                        post_split_key,
                        "post split tail append",
                        post_split_key + 1u) ||
        !expect_payload(table, &schema, 5u, "five", 50u) ||
        !expect_payload(table, &schema, 10u, "ten", 100u) ||
        !expect_payload(table, &schema, 20u, "twenty", 200u)) {
        fprintf(stderr, "reopen lost payload-native split topology/data\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    TinyDBRecord legacy;
    if (tinydb_record_find(table, &schema, 10u, &legacy)) {
        fprintf(stderr, "legacy TinyDBRecord unexpectedly accepted 308-byte inserted row\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    db_close(table);
    remove(argv[1]);
    printf("PAYLOAD_NATIVE_INSERT_OK row_size=308 initial_root=1 duplicate_guard=1 root_split=1 nonroot_split=1 tail_append=1 reopen=1 rows=%u root_split_key=%u nonroot_split_key=%u\n",
           final_count,
           root_split_key,
           nonroot_split_key);
    return 0;
}
