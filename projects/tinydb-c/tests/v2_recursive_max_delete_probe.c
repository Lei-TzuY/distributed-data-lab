#include "engine.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_leaf_v2_split.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTERNAL_COUNT 6u
#define LEAF_COUNT 8u
#define BASELINE_ROWS 10u

enum {
    NODE_A = 0,
    NODE_S,
    NODE_Q,
    NODE_P,
    NODE_U,
    NODE_V,
};

enum {
    LEAF_Q0 = 0,
    LEAF_Q1,
    LEAF_P0,
    LEAF_P1,
    LEAF_U0,
    LEAF_U1,
    LEAF_V0,
    LEAF_V1,
};

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

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
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
    values[2].int_value = id + 900u;
}

static bool raw_insert(const TableSchema* schema,
                       unsigned char page[PAGE_SIZE],
                       uint32_t id) {
    TinyDBValue values[3];
    TinyDBRecord record;
    TinyDBRecordPayload payload;
    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    make_values(id, "recursive-max", values);
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
                                                 &envelope_length) &&
           envelope_length > 0u && envelope_length <= UINT16_MAX &&
           tinydb_slotted_leaf_v2_insert(page,
                                         PAGE_SIZE,
                                         id,
                                         envelope,
                                         (uint16_t)envelope_length);
}

static bool initialize_leaf(Pager* pager,
                            uint32_t page_num,
                            uint32_t parent_page_num,
                            uint32_t prev_page_num,
                            uint32_t next_page_num) {
    unsigned char* page = (unsigned char*)get_page(pager, page_num);
    memset(page, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    tinydb_slotted_split_write_u32(
        page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
        prev_page_num);
    tinydb_slotted_split_write_u32(
        page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
        next_page_num);
    return true;
}

static bool initialize_internal(Pager* pager,
                                uint32_t page_num,
                                uint32_t parent_page_num,
                                bool is_root,
                                uint32_t left_child,
                                uint32_t left_max,
                                uint32_t right_child) {
    unsigned char* page = (unsigned char*)get_page(pager, page_num);
    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = is_root ? 1u : 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE;
    write_u32(cell, left_child);
    write_u32(cell + INTERNAL_NODE_CHILD_SIZE, left_max);
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, right_child);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool seed_tree(TinyDB* db,
                      TableSchema* schema,
                      uint32_t internal_pages[INTERNAL_COUNT],
                      uint32_t leaf_pages[LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root = schema->root_page_num;

    for (uint32_t i = 0u; i < INTERNAL_COUNT; i++) {
        internal_pages[i] = get_unused_page_num(pager);
        if (internal_pages[i] == 0u ||
            internal_pages[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, internal_pages[i]);
    }
    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        leaf_pages[i] = get_unused_page_num(pager);
        if (leaf_pages[i] == 0u || leaf_pages[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaf_pages[i]);
    }

    const uint32_t leaf_parents[LEAF_COUNT] = {
        internal_pages[NODE_Q], internal_pages[NODE_Q],
        internal_pages[NODE_P], internal_pages[NODE_P],
        internal_pages[NODE_U], internal_pages[NODE_U],
        internal_pages[NODE_V], internal_pages[NODE_V],
    };
    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        if (!initialize_leaf(pager,
                             leaf_pages[i],
                             leaf_parents[i],
                             i == 0u ? 0u : leaf_pages[i - 1u],
                             i + 1u == LEAF_COUNT ? 0u : leaf_pages[i + 1u])) {
            return false;
        }
    }

    const uint32_t singleton_keys[6] = {10u, 100u, 200u, 400u, 500u, 600u};
    const uint32_t singleton_leaves[6] = {
        LEAF_Q0, LEAF_Q1, LEAF_P0, LEAF_U0, LEAF_U1, LEAF_V0,
    };
    for (uint32_t i = 0u; i < 6u; i++) {
        unsigned char* page =
            (unsigned char*)get_page(pager, leaf_pages[singleton_leaves[i]]);
        if (!raw_insert(schema, page, singleton_keys[i])) return false;
    }
    unsigned char* p1 = (unsigned char*)get_page(pager, leaf_pages[LEAF_P1]);
    unsigned char* v1 = (unsigned char*)get_page(pager, leaf_pages[LEAF_V1]);
    if (!raw_insert(schema, p1, 250u) || !raw_insert(schema, p1, 300u) ||
        !raw_insert(schema, v1, 650u) || !raw_insert(schema, v1, 700u)) {
        return false;
    }
    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        unsigned char* page = (unsigned char*)get_page(pager, leaf_pages[i]);
        if (!tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaf_pages[i]);
    }

    if (!initialize_internal(pager,
                             internal_pages[NODE_Q],
                             internal_pages[NODE_A],
                             false,
                             leaf_pages[LEAF_Q0],
                             10u,
                             leaf_pages[LEAF_Q1]) ||
        !initialize_internal(pager,
                             internal_pages[NODE_P],
                             internal_pages[NODE_A],
                             false,
                             leaf_pages[LEAF_P0],
                             200u,
                             leaf_pages[LEAF_P1]) ||
        !initialize_internal(pager,
                             internal_pages[NODE_U],
                             internal_pages[NODE_S],
                             false,
                             leaf_pages[LEAF_U0],
                             400u,
                             leaf_pages[LEAF_U1]) ||
        !initialize_internal(pager,
                             internal_pages[NODE_V],
                             internal_pages[NODE_S],
                             false,
                             leaf_pages[LEAF_V0],
                             600u,
                             leaf_pages[LEAF_V1]) ||
        !initialize_internal(pager,
                             internal_pages[NODE_A],
                             root,
                             false,
                             internal_pages[NODE_Q],
                             100u,
                             internal_pages[NODE_P]) ||
        !initialize_internal(pager,
                             internal_pages[NODE_S],
                             root,
                             false,
                             internal_pages[NODE_U],
                             500u,
                             internal_pages[NODE_V]) ||
        !initialize_internal(pager,
                             root,
                             0u,
                             true,
                             internal_pages[NODE_A],
                             300u,
                             internal_pages[NODE_S])) {
        return false;
    }
    for (uint32_t i = 0u; i < INTERNAL_COUNT; i++) {
        mark_page_dirty(pager, internal_pages[i]);
    }
    mark_page_dirty(pager, root);
    pager_commit(pager);

    return tinydb_record_scan(table, schema, NULL, NULL) == BASELINE_ROWS &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool root_separator(Table* table,
                           const TableSchema* schema,
                           uint32_t expected) {
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           sizeof(root));
    return tinydb_parent_stage_validate(root, PAGE_SIZE) &&
           tinydb_parent_stage_key_at(root, 0u) == expected;
}

static bool leaf_max(Table* table,
                     uint32_t page_num,
                     uint32_t expected_count,
                     uint32_t expected_max) {
    unsigned char page[PAGE_SIZE];
    memcpy(page, get_page(table->pager, page_num), sizeof(page));
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           tinydb_leaf_page_count(page, PAGE_SIZE, &count) &&
           count == expected_count && count > 0u &&
           tinydb_leaf_page_key_at(page, PAGE_SIZE, count - 1u, &max_key) &&
           max_key == expected_max;
}

static bool record_present(Table* table,
                           const TableSchema* schema,
                           uint32_t key) {
    TinyDBRecord record;
    return tinydb_record_find(table, schema, key, &record);
}

static bool delete_key(Table* table,
                       const TableSchema* schema,
                       uint32_t key) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_delete(table,
                              schema,
                              key,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "delete %u failed: %s\n", key, message);
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
    uint32_t internal_pages[INTERNAL_COUNT];
    uint32_t leaf_pages[LEAF_COUNT];
    if (schema == NULL || schema->row_size != 264u ||
        !seed_tree(db, schema, internal_pages, leaf_pages) ||
        !root_separator(table, schema, 300u)) {
        fprintf(stderr, "unable to seed recursive max-delete tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!exec_ok(db, "BEGIN;") ||
        !delete_key(table, schema, 300u) ||
        record_present(table, schema, 300u) ||
        !root_separator(table, schema, 250u) ||
        !leaf_max(table, leaf_pages[LEAF_P1], 1u, 250u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "recursive max-delete transaction failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!record_present(table, schema, 300u) ||
        !root_separator(table, schema, 300u) ||
        !leaf_max(table, leaf_pages[LEAF_P1], 2u, 300u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "recursive max-delete rollback failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 300u) ||
        record_present(table, schema, 300u) ||
        !root_separator(table, schema, 250u) ||
        !leaf_max(table, leaf_pages[LEAF_P1], 1u, 250u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed recursive max-delete failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 700u) ||
        record_present(table, schema, 700u) ||
        !root_separator(table, schema, 250u) ||
        !leaf_max(table, leaf_pages[LEAF_V1], 1u, 650u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 2u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "all-rightmost max-delete failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL ||
        !root_separator(table, schema, 250u) ||
        record_present(table, schema, 300u) ||
        record_present(table, schema, 700u) ||
        !record_present(table, schema, 250u) ||
        !record_present(table, schema, 650u) ||
        !leaf_max(table, leaf_pages[LEAF_P1], 1u, 250u) ||
        !leaf_max(table, leaf_pages[LEAF_V1], 1u, 650u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 2u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "recursive max-delete state did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("V2_RECURSIVE_MAX_DELETE_OK ancestor_propagation=yes rollback=yes "
           "root_rightmost_chain=yes reopen=yes integrity=yes wal=yes\n");
    tinydb_close(db);
    return EXIT_SUCCESS;
}
