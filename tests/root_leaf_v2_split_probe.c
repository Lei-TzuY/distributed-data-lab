#include "engine.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_split.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LONG_TEXT_LENGTH 250u
#define KEY_STEP 1000u
#define POST_INSERT_COUNT 3u

static bool exec_ok(TinyDB* db, const char* sql) {
    TinyDBSqlResult result;
    if (tinydb_execute_sql(db, sql, &result) != TINYDB_SQL_SUCCESS) {
        fprintf(stderr, "SQL failed: %s (%s)\n", sql, result.message);
        return false;
    }
    return true;
}

static TableSchema* find_schema(Table* table, const char* name) {
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static uint32_t read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static void make_long_text(char text[TINYDB_RECORD_TEXT_MAX + 1], char marker) {
    for (uint32_t i = 0u; i < LONG_TEXT_LENGTH; i++) {
        text[i] = (char)(marker + (char)(i % 3u));
    }
    text[LONG_TEXT_LENGTH] = '\0';
}

static void make_values(uint32_t id,
                        const char* text,
                        TinyDBValue values[3]) {
    memset(values, 0, sizeof(TinyDBValue) * 3u);
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "%s", text);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 321u;
}

static bool encode_envelope(const TableSchema* schema,
                            uint32_t id,
                            const char* text,
                            unsigned char envelope[PAGE_SIZE],
                            uint32_t* envelope_length) {
    TinyDBValue values[3];
    TinyDBRecord record;
    TinyDBRecordPayload payload;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    make_values(id, text, values);
    return tinydb_record_encode(schema,
                                values,
                                3u,
                                &record,
                                message,
                                sizeof(message)) &&
           tinydb_record_payload_from_record(schema,
                                             &record,
                                             &payload,
                                             message,
                                             sizeof(message)) &&
           tinydb_row_envelope_encode_compact_v2(schema,
                                                 &payload,
                                                 envelope,
                                                 PAGE_SIZE,
                                                 envelope_length) &&
           *envelope_length > 0u && *envelope_length <= UINT16_MAX;
}

static bool raw_insert(const TableSchema* schema,
                       unsigned char page[PAGE_SIZE],
                       uint32_t id,
                       const char* text) {
    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    return encode_envelope(schema, id, text, envelope, &envelope_length) &&
           tinydb_slotted_leaf_v2_insert(page,
                                         PAGE_SIZE,
                                         id,
                                         envelope,
                                         (uint16_t)envelope_length);
}

static bool seed_full_root_leaf(TinyDB* db,
                                TableSchema* schema,
                                uint32_t* candidate_key,
                                uint32_t* baseline_rows) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    unsigned char* root =
        (unsigned char*)get_page(pager, schema->root_page_num);
    memset(root, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(root, PAGE_SIZE)) return false;
    root[IS_ROOT_OFFSET] = 1u;
    write_u32(root + PARENT_POINTER_OFFSET, 0u);
    tinydb_slotted_split_write_u32(
        root + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
        0u);
    tinydb_slotted_split_write_u32(
        root + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
        0u);

    char long_text[TINYDB_RECORD_TEXT_MAX + 1];
    make_long_text(long_text, 'a');
    unsigned char candidate_envelope[PAGE_SIZE];
    uint32_t candidate_length = 0u;
    if (!encode_envelope(schema,
                         KEY_STEP,
                         long_text,
                         candidate_envelope,
                         &candidate_length)) {
        return false;
    }
    uint32_t candidate_required =
        TINYDB_SLOTTED_V2_SLOT_SIZE + candidate_length;

    uint32_t key = KEY_STEP;
    uint32_t inserted = 0u;
    while (tinydb_slotted_leaf_v2_free_bytes(root, PAGE_SIZE) >=
           candidate_required) {
        if (!raw_insert(schema, root, key, long_text)) return false;
        inserted++;
        if (key > UINT32_MAX - KEY_STEP) return false;
        key += KEY_STEP;
    }

    if (inserted < 2u ||
        tinydb_slotted_leaf_v2_free_bytes(root, PAGE_SIZE) >=
            candidate_required ||
        !tinydb_slotted_leaf_v2_validate(root, PAGE_SIZE)) {
        return false;
    }

    *candidate_key = key;
    mark_page_dirty(pager, schema->root_page_num);
    pager_commit(pager);
    *baseline_rows = tinydb_record_scan(table, schema, NULL, NULL);
    return *baseline_rows == inserted &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool root_is_original_leaf(Table* table,
                                  const TableSchema* schema,
                                  uint32_t expected_rows) {
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           sizeof(root));
    uint32_t count = 0u;
    uint32_t prev = INVALID_PAGE_NUM;
    uint32_t next = INVALID_PAGE_NUM;
    return tinydb_leaf_format_detect_page(root, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(root, PAGE_SIZE) &&
           root[IS_ROOT_OFFSET] != 0u &&
           read_u32(root + PARENT_POINTER_OFFSET) == 0u &&
           tinydb_leaf_page_count(root, PAGE_SIZE, &count) &&
           count == expected_rows &&
           tinydb_leaf_page_prev(root, PAGE_SIZE, &prev) && prev == 0u &&
           tinydb_leaf_page_next(root, PAGE_SIZE, &next) && next == 0u;
}

static bool root_has_two_leaf_children(Table* table,
                                       const TableSchema* schema) {
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           sizeof(root));
    if (get_node_type(root) != NODE_INTERNAL || root[IS_ROOT_OFFSET] == 0u ||
        read_u32(root + PARENT_POINTER_OFFSET) != 0u ||
        read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return false;
    }

    uint32_t left_page_num = read_u32(root + INTERNAL_NODE_HEADER_SIZE);
    uint32_t separator = read_u32(root + INTERNAL_NODE_HEADER_SIZE +
                                  INTERNAL_NODE_CHILD_SIZE);
    uint32_t right_page_num = read_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
    if (left_page_num == 0u || right_page_num == 0u ||
        left_page_num == right_page_num ||
        left_page_num >= table->pager->num_pages ||
        right_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char left[PAGE_SIZE];
    unsigned char right[PAGE_SIZE];
    memcpy(left, get_page(table->pager, left_page_num), sizeof(left));
    memcpy(right, get_page(table->pager, right_page_num), sizeof(right));
    uint32_t left_count = 0u;
    uint32_t right_count = 0u;
    uint32_t left_max = 0u;
    uint32_t right_min = 0u;
    uint32_t left_prev = INVALID_PAGE_NUM;
    uint32_t left_next = INVALID_PAGE_NUM;
    uint32_t right_prev = INVALID_PAGE_NUM;
    uint32_t right_next = INVALID_PAGE_NUM;
    return get_node_type(left) == NODE_LEAF &&
           get_node_type(right) == NODE_LEAF &&
           left[IS_ROOT_OFFSET] == 0u && right[IS_ROOT_OFFSET] == 0u &&
           read_u32(left + PARENT_POINTER_OFFSET) == schema->root_page_num &&
           read_u32(right + PARENT_POINTER_OFFSET) == schema->root_page_num &&
           tinydb_slotted_leaf_v2_validate(left, PAGE_SIZE) &&
           tinydb_slotted_leaf_v2_validate(right, PAGE_SIZE) &&
           tinydb_leaf_page_count(left, PAGE_SIZE, &left_count) &&
           tinydb_leaf_page_count(right, PAGE_SIZE, &right_count) &&
           left_count > 0u && right_count > 0u &&
           tinydb_leaf_page_key_at(left, PAGE_SIZE, left_count - 1u, &left_max) &&
           tinydb_leaf_page_key_at(right, PAGE_SIZE, 0u, &right_min) &&
           separator == left_max && left_max < right_min &&
           tinydb_leaf_page_prev(left, PAGE_SIZE, &left_prev) && left_prev == 0u &&
           tinydb_leaf_page_next(left, PAGE_SIZE, &left_next) &&
           left_next == right_page_num &&
           tinydb_leaf_page_prev(right, PAGE_SIZE, &right_prev) &&
           right_prev == left_page_num &&
           tinydb_leaf_page_next(right, PAGE_SIZE, &right_next) && right_next == 0u;
}

static bool record_present(Table* table,
                           const TableSchema* schema,
                           uint32_t key) {
    TinyDBRecord record;
    return tinydb_record_find(table, schema, key, &record);
}

static bool insert_value(Table* table,
                         const TableSchema* schema,
                         uint32_t key,
                         const char* text) {
    TinyDBValue values[3];
    char message[TINYDB_RECORD_MESSAGE_MAX];
    make_values(key, text, values);
    if (!tinydb_record_insert(table,
                              schema,
                              values,
                              3u,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "insert %u failed: %s\n", key, message);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s DATABASE\n", argv[0]);
        return EXIT_FAILURE;
    }
    remove(argv[1]);

    TinyDB* db = tinydb_open(argv[1]);
    if (db == NULL ||
        !exec_ok(db,
                 "CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return EXIT_FAILURE;
    }

    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t candidate_key = 0u;
    uint32_t baseline_rows = 0u;
    if (schema == NULL || schema->row_size != 264u ||
        !seed_full_root_leaf(db,
                             schema,
                             &candidate_key,
                             &baseline_rows)) {
        fprintf(stderr, "unable to seed full V2 root leaf\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t baseline_pages = table->pager->num_pages;
    char long_text[TINYDB_RECORD_TEXT_MAX + 1];
    make_long_text(long_text, 'x');
    if (!root_is_original_leaf(table, schema, baseline_rows) ||
        !exec_ok(db, "BEGIN;") ||
        !insert_value(table, schema, candidate_key, long_text) ||
        !root_has_two_leaf_children(table, schema) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "transactional V2 root-leaf growth failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (table->pager->num_pages != baseline_pages ||
        !root_is_original_leaf(table, schema, baseline_rows) ||
        record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "V2 root-leaf rollback leaked topology or allocation\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!insert_value(table, schema, candidate_key, long_text) ||
        !root_has_two_leaf_children(table, schema) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed V2 root-leaf growth failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t post_keys[POST_INSERT_COUNT];
    for (uint32_t i = 0u; i < POST_INSERT_COUNT; i++) {
        post_keys[i] = candidate_key + (i + 1u) * KEY_STEP;
        if (!insert_value(table, schema, post_keys[i], "post-growth") ||
            !record_present(table, schema, post_keys[i])) {
            fprintf(stderr, "post-growth insert failed\n");
            tinydb_close(db);
            return EXIT_FAILURE;
        }
    }
    if (!root_has_two_leaf_children(table, schema) ||
        tinydb_record_scan(table, schema, NULL, NULL) !=
            baseline_rows + 1u + POST_INSERT_COUNT ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "post-growth root topology became inconsistent\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL ||
        !root_has_two_leaf_children(table, schema) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) !=
            baseline_rows + 1u + POST_INSERT_COUNT ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "V2 root-leaf growth did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }
    for (uint32_t i = 0u; i < POST_INSERT_COUNT; i++) {
        if (!record_present(table, schema, post_keys[i])) {
            fprintf(stderr, "post-growth row %u disappeared after reopen\n", post_keys[i]);
            tinydb_close(db);
            return EXIT_FAILURE;
        }
    }

    printf("ROOT_LEAF_V2_SPLIT_OK root_growth=yes rollback=yes commit=yes "
           "post_growth_insert=yes reopen=yes integrity=yes wal=yes "
           "baseline_rows=%u candidate=%u\n",
           baseline_rows,
           candidate_key);
    tinydb_close(db);
    return EXIT_SUCCESS;
}
