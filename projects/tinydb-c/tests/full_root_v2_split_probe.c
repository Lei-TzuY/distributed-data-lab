#include "engine.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_split.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHILD_COUNT (INTERNAL_NODE_MAX_KEYS + 1u)
#define RANGE_STEP 100000u
#define TARGET_CHILD_INDEX (INTERNAL_NODE_MAX_KEYS / 2u)
#define LONG_TEXT_LENGTH 250u

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
    values[2].int_value = id + 123u;
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

static bool seed_full_root(TinyDB* db,
                           TableSchema* schema,
                           uint32_t* target_page_num,
                           uint32_t* candidate_key,
                           uint32_t* baseline_rows) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root_page_num = schema->root_page_num;
    uint32_t leaf_pages[CHILD_COUNT];

    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        leaf_pages[i] = get_unused_page_num(pager);
        if (leaf_pages[i] == 0u || leaf_pages[i] == INVALID_PAGE_NUM) {
            return false;
        }
        (void)get_page(pager, leaf_pages[i]);
    }

    char long_text[TINYDB_RECORD_TEXT_MAX + 1];
    make_long_text(long_text, 'a');
    unsigned char candidate_envelope[PAGE_SIZE];
    uint32_t candidate_length = 0u;
    uint32_t previous_max = TARGET_CHILD_INDEX * RANGE_STEP;
    *candidate_key = previous_max + 1u;
    if (!encode_envelope(schema,
                         *candidate_key,
                         long_text,
                         candidate_envelope,
                         &candidate_length)) {
        return false;
    }
    uint32_t candidate_required =
        TINYDB_SLOTTED_V2_SLOT_SIZE + candidate_length;

    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        unsigned char* leaf =
            (unsigned char*)get_page(pager, leaf_pages[i]);
        memset(leaf, 0, PAGE_SIZE);
        if (!tinydb_slotted_leaf_v2_init(leaf, PAGE_SIZE)) return false;
        leaf[IS_ROOT_OFFSET] = 0u;
        write_u32(leaf + PARENT_POINTER_OFFSET, root_page_num);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            i == 0u ? 0u : leaf_pages[i - 1u]);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
            i + 1u == CHILD_COUNT ? 0u : leaf_pages[i + 1u]);

        uint32_t max_key = (i + 1u) * RANGE_STEP;
        if (i == TARGET_CHILD_INDEX) {
            if (!raw_insert(schema, leaf, max_key, long_text)) return false;
            uint32_t key = previous_max + 1000u;
            while (tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) >=
                   candidate_required) {
                if (key >= max_key) return false;
                if (!raw_insert(schema, leaf, key, long_text)) return false;
                key += 1000u;
            }
            if (tinydb_slotted_leaf_v2_count(leaf, PAGE_SIZE) < 2u ||
                tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) >=
                    candidate_required) {
                return false;
            }
            *target_page_num = leaf_pages[i];
        } else {
            if (!raw_insert(schema, leaf, max_key, "s")) return false;
        }
        if (!tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaf_pages[i]);
    }

    unsigned char* root =
        (unsigned char*)get_page(pager, root_page_num);
    memset(root, 0, PAGE_USABLE_SIZE);
    root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root[IS_ROOT_OFFSET] = 1u;
    write_u32(root + PARENT_POINTER_OFFSET, 0u);
    write_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET, INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        unsigned char* cell = root + INTERNAL_NODE_HEADER_SIZE +
                              i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, leaf_pages[i]);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, (i + 1u) * RANGE_STEP);
    }
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              leaf_pages[CHILD_COUNT - 1u]);
    mark_page_dirty(pager, root_page_num);
    pager_commit(pager);

    *baseline_rows = tinydb_record_scan(table, schema, NULL, NULL);
    return *target_page_num != INVALID_PAGE_NUM &&
           *baseline_rows >= CHILD_COUNT &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool root_has_grown(Table* table, const TableSchema* schema) {
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           sizeof(root));
    if (get_node_type(root) != NODE_INTERNAL || root[IS_ROOT_OFFSET] == 0u ||
        read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return false;
    }

    uint32_t left_page_num = read_u32(root + INTERNAL_NODE_HEADER_SIZE);
    uint32_t right_page_num = read_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
    if (left_page_num == 0u || right_page_num == 0u ||
        left_page_num >= table->pager->num_pages ||
        right_page_num >= table->pager->num_pages ||
        left_page_num == right_page_num) {
        return false;
    }

    unsigned char left[PAGE_SIZE], right[PAGE_SIZE];
    memcpy(left, get_page(table->pager, left_page_num), sizeof(left));
    memcpy(right, get_page(table->pager, right_page_num), sizeof(right));
    return get_node_type(left) == NODE_INTERNAL &&
           get_node_type(right) == NODE_INTERNAL &&
           left[IS_ROOT_OFFSET] == 0u && right[IS_ROOT_OFFSET] == 0u &&
           read_u32(left + PARENT_POINTER_OFFSET) == schema->root_page_num &&
           read_u32(right + PARENT_POINTER_OFFSET) == schema->root_page_num;
}

static bool record_present(Table* table,
                           const TableSchema* schema,
                           uint32_t key) {
    TinyDBRecord record;
    return tinydb_record_find(table, schema, key, &record);
}

static bool insert_candidate(Table* table,
                             const TableSchema* schema,
                             uint32_t key) {
    TinyDBValue values[3];
    char long_text[TINYDB_RECORD_TEXT_MAX + 1];
    char message[TINYDB_RECORD_MESSAGE_MAX];
    make_long_text(long_text, 'x');
    make_values(key, long_text, values);
    if (!tinydb_record_insert(table,
                              schema,
                              values,
                              3u,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "candidate insert failed: %s\n", message);
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
    uint32_t target_page_num = INVALID_PAGE_NUM;
    uint32_t candidate_key = 0u;
    uint32_t baseline_rows = 0u;
    if (schema == NULL || schema->row_size != 264u ||
        !seed_full_root(db,
                        schema,
                        &target_page_num,
                        &candidate_key,
                        &baseline_rows)) {
        fprintf(stderr, "unable to seed full-root V2 tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t baseline_pages = table->pager->num_pages;
    if (!exec_ok(db, "BEGIN;") ||
        !insert_candidate(table, schema, candidate_key) ||
        !root_has_grown(table, schema) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "transactional full-root split failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (table->pager->num_pages != baseline_pages ||
        read_u32((unsigned char*)get_page(table->pager, schema->root_page_num) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != INTERNAL_NODE_MAX_KEYS ||
        record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "full-root split rollback leaked topology or allocation\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!insert_candidate(table, schema, candidate_key) ||
        !root_has_grown(table, schema) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed full-root V2 split failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL || !root_has_grown(table, schema) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "full-root V2 split did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("FULL_ROOT_V2_SPLIT_OK target_page=%u key=%u baseline_rows=%u root_growth=yes rollback=yes commit=yes reopen=yes integrity=yes wal=yes\n",
           target_page_num,
           candidate_key,
           baseline_rows);
    tinydb_close(db);
    remove(argv[1]);
    return EXIT_SUCCESS;
}
