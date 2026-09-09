#include "engine.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_leaf_v2_split.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEAF_COUNT 3u

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

static uint32_t read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static bool raw_insert(const TableSchema* schema,
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
    snprintf(values[1].text, sizeof(values[1].text), "boundary-%u", id);
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
                      uint32_t leaves[LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root = schema->root_page_num;
    const uint32_t keys[LEAF_COUNT] = {10u, 20u, 30u};
    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        leaves[i] = get_unused_page_num(pager);
        if (leaves[i] == 0u || leaves[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaves[i]);
    }
    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        unsigned char* leaf = (unsigned char*)get_page(pager, leaves[i]);
        memset(leaf, 0, PAGE_SIZE);
        if (!tinydb_slotted_leaf_v2_init(leaf, PAGE_SIZE)) return false;
        leaf[IS_ROOT_OFFSET] = 0u;
        write_u32(leaf + PARENT_POINTER_OFFSET, root);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            i == 0u ? 0u : leaves[i - 1u]);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
            i + 1u == LEAF_COUNT ? 0u : leaves[i + 1u]);
        if (!raw_insert(schema, leaf, keys[i]) ||
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaves[i]);
    }

    unsigned char* root_page = (unsigned char*)get_page(pager, root);
    memset(root_page, 0, PAGE_USABLE_SIZE);
    root_page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root_page[IS_ROOT_OFFSET] = 1u;
    write_u32(root_page + PARENT_POINTER_OFFSET, 0u);
    write_u32(root_page + INTERNAL_NODE_NUM_KEYS_OFFSET, 2u);
    unsigned char* c0 = root_page + INTERNAL_NODE_HEADER_SIZE;
    unsigned char* c1 = c0 + INTERNAL_NODE_CELL_SIZE;
    write_u32(c0, leaves[0]);
    write_u32(c0 + INTERNAL_NODE_CHILD_SIZE, 10u);
    write_u32(c1, leaves[1]);
    write_u32(c1 + INTERNAL_NODE_CHILD_SIZE, 20u);
    write_u32(root_page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, leaves[2]);
    if (!tinydb_parent_stage_validate(root_page, PAGE_SIZE)) return false;
    mark_page_dirty(pager, root);
    pager_commit(pager);
    return tinydb_record_scan(table, schema, NULL, NULL) == 3u &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool root_matches(Table* table, const TableSchema* schema,
                         const uint32_t* children, const uint32_t* keys,
                         uint32_t child_count) {
    unsigned char root[PAGE_SIZE];
    memcpy(root, get_page(table->pager, schema->root_page_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root, PAGE_SIZE) ||
        root[IS_ROOT_OFFSET] == 0u ||
        read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) != child_count - 1u) {
        return false;
    }
    for (uint32_t i = 0u; i < child_count; i++) {
        if (tinydb_parent_stage_child_at(root, i) != children[i]) return false;
        if (i + 1u < child_count && tinydb_parent_stage_key_at(root, i) != keys[i]) {
            return false;
        }
    }
    return true;
}

static bool root_leaf(Table* table, const TableSchema* schema,
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

static bool leaf_links(Table* table, uint32_t page_num,
                       uint32_t expected_prev, uint32_t expected_next) {
    unsigned char leaf[PAGE_SIZE];
    uint32_t prev = UINT32_MAX, next = UINT32_MAX;
    memcpy(leaf, get_page(table->pager, page_num), PAGE_SIZE);
    return tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE) &&
           tinydb_leaf_page_prev(leaf, PAGE_SIZE, &prev) &&
           tinydb_leaf_page_next(leaf, PAGE_SIZE, &next) &&
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

static bool delete_key(Table* table, const TableSchema* schema, uint32_t key,
                       char message[TINYDB_RECORD_MESSAGE_MAX]) {
    message[0] = '\0';
    return tinydb_record_delete(table, schema, key, message,
                                TINYDB_RECORD_MESSAGE_MAX);
}

static bool run_boundary_case(const char* path, bool remove_left) {
    remove(path);
    TinyDB* db = tinydb_open(path);
    if (db == NULL ||
        !exec_ok(db, "CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return false;
    }
    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t leaves[LEAF_COUNT] = {0u, 0u, 0u};
    if (schema == NULL || schema->row_size != 264u || !seed_tree(db, schema, leaves)) {
        tinydb_close(db);
        return false;
    }

    const uint32_t before_children[3] = {leaves[0], leaves[1], leaves[2]};
    const uint32_t before_keys[2] = {10u, 20u};
    uint32_t target_key = remove_left ? 10u : 30u;
    uint32_t target_page = remove_left ? leaves[0] : leaves[2];
    uint32_t after_children[2];
    uint32_t after_keys[1];
    if (remove_left) {
        after_children[0] = leaves[1]; after_children[1] = leaves[2]; after_keys[0] = 20u;
    } else {
        after_children[0] = leaves[0]; after_children[1] = leaves[1]; after_keys[0] = 10u;
    }
    uint32_t free_before = table->pager->free_page_count;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!root_matches(table, schema, before_children, before_keys, 3u) ||
        !exec_ok(db, "BEGIN;") || !delete_key(table, schema, target_key, message) ||
        present(table, schema, target_key) ||
        !root_matches(table, schema, after_children, after_keys, 2u) ||
        table->pager->free_page_count != free_before + 1u ||
        !free_has(table->pager, target_page) ||
        !exec_ok(db, "PRAGMA integrity_check;") || !exec_ok(db, "ROLLBACK;") ||
        !present(table, schema, target_key) ||
        !root_matches(table, schema, before_children, before_keys, 3u) ||
        table->pager->free_page_count != free_before) {
        fprintf(stderr, "%s boundary rollback failed: %s\n",
                remove_left ? "left" : "right", message);
        tinydb_close(db);
        return false;
    }

    if (!delete_key(table, schema, target_key, message) ||
        !root_matches(table, schema, after_children, after_keys, 2u) ||
        table->pager->free_page_count != free_before + 1u ||
        !free_has(table->pager, target_page) ||
        (remove_left && !leaf_links(table, leaves[1], 0u, leaves[2])) ||
        (!remove_left && !leaf_links(table, leaves[1], leaves[0], 0u))) {
        fprintf(stderr, "committed %s boundary delete failed: %s\n",
                remove_left ? "left" : "right", message);
        tinydb_close(db);
        return false;
    }

    uint32_t collapse_key = 20u;
    uint32_t survivor_key = remove_left ? 30u : 10u;
    uint32_t collapse_page = leaves[1];
    uint32_t survivor_page = remove_left ? leaves[2] : leaves[0];
    if (!exec_ok(db, "BEGIN;") || !delete_key(table, schema, collapse_key, message) ||
        present(table, schema, collapse_key) || !present(table, schema, survivor_key) ||
        !root_leaf(table, schema, survivor_key) ||
        table->pager->free_page_count != free_before + 3u ||
        !free_has(table->pager, target_page) ||
        !free_has(table->pager, collapse_page) ||
        !free_has(table->pager, survivor_page) ||
        !exec_ok(db, "PRAGMA integrity_check;") || !exec_ok(db, "ROLLBACK;") ||
        !present(table, schema, collapse_key) || !present(table, schema, survivor_key) ||
        !root_matches(table, schema, after_children, after_keys, 2u) ||
        table->pager->free_page_count != free_before + 1u ||
        free_has(table->pager, collapse_page) || free_has(table->pager, survivor_page)) {
        fprintf(stderr, "%s root-collapse rollback failed: %s\n",
                remove_left ? "left" : "right", message);
        tinydb_close(db);
        return false;
    }

    if (!delete_key(table, schema, collapse_key, message) ||
        !root_leaf(table, schema, survivor_key) ||
        table->pager->free_page_count != free_before + 3u ||
        !free_has(table->pager, target_page) ||
        !free_has(table->pager, collapse_page) ||
        !free_has(table->pager, survivor_page) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed %s root collapse failed: %s\n",
                remove_left ? "left" : "right", message);
        tinydb_close(db);
        return false;
    }

    tinydb_close(db);
    db = tinydb_open(path);
    if (db == NULL) return false;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    bool ok = schema != NULL && !present(table, schema, target_key) &&
              !present(table, schema, collapse_key) &&
              present(table, schema, survivor_key) &&
              root_leaf(table, schema, survivor_key) &&
              table->pager->free_page_count == free_before + 3u &&
              free_has(table->pager, target_page) &&
              free_has(table->pager, collapse_page) &&
              free_has(table->pager, survivor_page) &&
              tinydb_record_scan(table, schema, NULL, NULL) == 1u &&
              exec_ok(db, "PRAGMA integrity_check;");
    tinydb_close(db);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    char left_path[768];
    char right_path[768];
    int left_len = snprintf(left_path, sizeof(left_path), "%s.left", argv[1]);
    int right_len = snprintf(right_path, sizeof(right_path), "%s.right", argv[1]);
    if (left_len < 0 || right_len < 0 ||
        (size_t)left_len >= sizeof(left_path) ||
        (size_t)right_len >= sizeof(right_path) ||
        !run_boundary_case(left_path, true) ||
        !run_boundary_case(right_path, false)) {
        return EXIT_FAILURE;
    }
    printf("V2_EMPTY_LEAF_BOUNDARY_OK left=yes right=yes rollback=yes "
           "allocator=yes root_collapse=yes collapse_rollback=yes "
           "reopen=yes integrity=yes wal=yes\n");
    return EXIT_SUCCESS;
}
