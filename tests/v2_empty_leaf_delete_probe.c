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

#define CHILD_COUNT 3u

static bool exec_ok(TinyDB* db, const char* sql) {
    TinyDBSqlResult result;
    if (tinydb_execute_sql(db, sql, &result) != TINYDB_SQL_SUCCESS) {
        fprintf(stderr, "SQL failed: %s (%s)\n", sql, result.message);
        return false;
    }
    return true;
}

static TableSchema* schema_named(Table* table, const char* name) {
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

static uint32_t read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static bool add_row(const TableSchema* schema,
                    unsigned char page[PAGE_SIZE],
                    uint32_t id) {
    TinyDBValue values[3];
    TinyDBRecord record;
    TinyDBRecordPayload payload;
    unsigned char envelope[PAGE_SIZE];
    uint32_t length = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    memset(values, 0, sizeof(values));
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "leaf-%u", id);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 1000u;
    return tinydb_record_encode(schema, values, 3u, &record, message,
                                sizeof(message)) &&
           tinydb_record_payload_from_record(schema, &record, &payload, message,
                                             sizeof(message)) &&
           tinydb_row_envelope_encode_compact_v2(schema, &payload, envelope,
                                                 sizeof(envelope), &length) &&
           length > 0u && length <= UINT16_MAX &&
           tinydb_slotted_leaf_v2_insert(page, PAGE_SIZE, id, envelope,
                                         (uint16_t)length);
}

static bool seed_tree(TinyDB* db,
                      TableSchema* schema,
                      uint32_t leaves[CHILD_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root_num = schema->root_page_num;
    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        leaves[i] = get_unused_page_num(pager);
        if (leaves[i] == 0u || leaves[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaves[i]);
    }
    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        unsigned char* leaf = (unsigned char*)get_page(pager, leaves[i]);
        memset(leaf, 0, PAGE_SIZE);
        if (!tinydb_slotted_leaf_v2_init(leaf, PAGE_SIZE)) return false;
        leaf[IS_ROOT_OFFSET] = 0u;
        write_u32(leaf + PARENT_POINTER_OFFSET, root_num);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            i == 0u ? 0u : leaves[i - 1u]);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
            i + 1u == CHILD_COUNT ? 0u : leaves[i + 1u]);
        if (!add_row(schema, leaf, (i + 1u) * 10u) ||
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaves[i]);
    }

    unsigned char* root = (unsigned char*)get_page(pager, root_num);
    memset(root, 0, PAGE_USABLE_SIZE);
    root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root[IS_ROOT_OFFSET] = 1u;
    write_u32(root + PARENT_POINTER_OFFSET, 0u);
    write_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET, 2u);
    unsigned char* c0 = root + INTERNAL_NODE_HEADER_SIZE;
    unsigned char* c1 = c0 + INTERNAL_NODE_CELL_SIZE;
    write_u32(c0, leaves[0]);
    write_u32(c0 + INTERNAL_NODE_CHILD_SIZE, 10u);
    write_u32(c1, leaves[1]);
    write_u32(c1 + INTERNAL_NODE_CHILD_SIZE, 20u);
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET, leaves[2]);
    if (!tinydb_parent_stage_validate(root, PAGE_SIZE)) return false;
    mark_page_dirty(pager, root_num);
    pager_commit(pager);
    return tinydb_record_scan(table, schema, NULL, NULL) == 3u &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool internal_root(Table* table,
                          const TableSchema* schema,
                          const uint32_t leaves[CHILD_COUNT],
                          uint32_t child_count) {
    unsigned char root[PAGE_SIZE];
    memcpy(root, get_page(table->pager, schema->root_page_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root, PAGE_SIZE) ||
        root[IS_ROOT_OFFSET] == 0u ||
        read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) != child_count - 1u) {
        return false;
    }
    if (child_count == 3u) {
        return tinydb_parent_stage_child_at(root, 0u) == leaves[0] &&
               tinydb_parent_stage_child_at(root, 1u) == leaves[1] &&
               tinydb_parent_stage_child_at(root, 2u) == leaves[2] &&
               tinydb_parent_stage_key_at(root, 0u) == 10u &&
               tinydb_parent_stage_key_at(root, 1u) == 20u;
    }
    return child_count == 2u &&
           tinydb_parent_stage_child_at(root, 0u) == leaves[0] &&
           tinydb_parent_stage_child_at(root, 1u) == leaves[2] &&
           tinydb_parent_stage_key_at(root, 0u) == 10u;
}

static bool root_leaf(Table* table,
                      const TableSchema* schema,
                      uint32_t expected_key) {
    unsigned char root[PAGE_SIZE];
    uint32_t count = 0u, key = 0u, prev = UINT32_MAX, next = UINT32_MAX;
    memcpy(root, get_page(table->pager, schema->root_page_num), PAGE_SIZE);
    return tinydb_slotted_leaf_v2_validate(root, PAGE_SIZE) &&
           root[IS_ROOT_OFFSET] != 0u &&
           read_u32(root + PARENT_POINTER_OFFSET) == 0u &&
           tinydb_leaf_page_count(root, PAGE_SIZE, &count) && count == 1u &&
           tinydb_leaf_page_key_at(root, PAGE_SIZE, 0u, &key) &&
           key == expected_key &&
           tinydb_leaf_page_prev(root, PAGE_SIZE, &prev) && prev == 0u &&
           tinydb_leaf_page_next(root, PAGE_SIZE, &next) && next == 0u;
}

static bool links(Table* table, uint32_t page_num,
                  uint32_t expected_prev, uint32_t expected_next) {
    unsigned char page[PAGE_SIZE];
    uint32_t prev = UINT32_MAX, next = UINT32_MAX;
    memcpy(page, get_page(table->pager, page_num), PAGE_SIZE);
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           tinydb_leaf_page_prev(page, PAGE_SIZE, &prev) &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next) &&
           prev == expected_prev && next == expected_next;
}

static bool free_has(const Pager* pager, uint32_t page_num) {
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static bool present(Table* table, const TableSchema* schema, uint32_t key) {
    TinyDBRecord record;
    return tinydb_record_find(table, schema, key, &record);
}

static bool remove_key(Table* table, const TableSchema* schema, uint32_t key,
                       char message[TINYDB_RECORD_MESSAGE_MAX]) {
    message[0] = '\0';
    return tinydb_record_delete(table, schema, key, message,
                                TINYDB_RECORD_MESSAGE_MAX);
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    remove(argv[1]);
    TinyDB* db = tinydb_open(argv[1]);
    if (db == NULL ||
        !exec_ok(db, "CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return EXIT_FAILURE;
    }
    Table* table = tinydb_table(db);
    TableSchema* schema = schema_named(table, "items");
    uint32_t leaves[CHILD_COUNT] = {0u, 0u, 0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_tree(db, schema, leaves) || !internal_root(table, schema, leaves, 3u)) {
        fprintf(stderr, "unable to seed V2 empty-leaf tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t free_before = table->pager->free_page_count;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!exec_ok(db, "BEGIN;") || !remove_key(table, schema, 20u, message) ||
        present(table, schema, 20u) || !internal_root(table, schema, leaves, 2u) ||
        table->pager->free_page_count != free_before + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") || !exec_ok(db, "ROLLBACK;") ||
        !present(table, schema, 20u) || !internal_root(table, schema, leaves, 3u) ||
        table->pager->free_page_count != free_before) {
        fprintf(stderr, "interior empty-leaf rollback failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!remove_key(table, schema, 20u, message) || present(table, schema, 20u) ||
        !internal_root(table, schema, leaves, 2u) ||
        !links(table, leaves[0], 0u, leaves[2]) ||
        !links(table, leaves[2], leaves[0], 0u) ||
        table->pager->free_page_count != free_before + 1u ||
        !free_has(table->pager, leaves[1]) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed interior empty-leaf removal failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!exec_ok(db, "BEGIN;") || !remove_key(table, schema, 10u, message) ||
        present(table, schema, 10u) || !present(table, schema, 30u) ||
        !root_leaf(table, schema, 30u) ||
        table->pager->free_page_count != free_before + 3u ||
        !free_has(table->pager, leaves[0]) || !free_has(table->pager, leaves[1]) ||
        !free_has(table->pager, leaves[2]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") || !exec_ok(db, "ROLLBACK;") ||
        !present(table, schema, 10u) || !present(table, schema, 30u) ||
        !internal_root(table, schema, leaves, 2u) ||
        table->pager->free_page_count != free_before + 1u ||
        free_has(table->pager, leaves[0]) || !free_has(table->pager, leaves[1]) ||
        free_has(table->pager, leaves[2])) {
        fprintf(stderr, "transactional root collapse rollback failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!remove_key(table, schema, 10u, message) || present(table, schema, 10u) ||
        !present(table, schema, 30u) || !root_leaf(table, schema, 30u) ||
        table->pager->free_page_count != free_before + 3u ||
        !free_has(table->pager, leaves[0]) || !free_has(table->pager, leaves[1]) ||
        !free_has(table->pager, leaves[2]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed root collapse failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = schema_named(table, "items");
    if (schema == NULL || present(table, schema, 10u) || present(table, schema, 20u) ||
        !present(table, schema, 30u) || !root_leaf(table, schema, 30u) ||
        table->pager->free_page_count != free_before + 3u ||
        !free_has(table->pager, leaves[0]) || !free_has(table->pager, leaves[1]) ||
        !free_has(table->pager, leaves[2]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "root collapse did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("V2_EMPTY_LEAF_DELETE_OK interior_child_remove=yes sibling_relink=yes "
           "rollback=yes page_reclaim=yes root_collapse=yes "
           "root_collapse_rollback=yes reopen=yes integrity=yes wal=yes\n");
    tinydb_close(db);
    return EXIT_SUCCESS;
}
