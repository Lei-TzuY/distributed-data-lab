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

#define LEAF_COUNT 6u
#define BASELINE_ROWS 6u

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

static bool raw_insert(const TableSchema* schema,
                       unsigned char page[PAGE_SIZE],
                       uint32_t id) {
    TinyDBValue values[3];
    TinyDBRecord record;
    TinyDBRecordPayload payload;
    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    memset(values, 0, sizeof(values));
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "borrow-left-%u", id);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 3000u;
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

static bool build_internal(Pager* pager,
                           uint32_t page_num,
                           uint32_t parent_page_num,
                           bool is_root,
                           const uint32_t* children,
                           const uint32_t* separators,
                           uint32_t child_count) {
    if (child_count < 2u) return false;
    unsigned char* page = (unsigned char*)get_page(pager, page_num);
    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = is_root ? 1u : 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, child_count - 1u);
    for (uint32_t i = 0u; i + 1u < child_count; i++) {
        unsigned char* cell =
            page + INTERNAL_NODE_HEADER_SIZE + i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, children[i]);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, separators[i]);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              children[child_count - 1u]);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool seed_tree(TinyDB* db,
                      TableSchema* schema,
                      uint32_t* parent_left,
                      uint32_t* parent_right,
                      uint32_t leaves[LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root = schema->root_page_num;
    const uint32_t keys[LEAF_COUNT] = {10u, 20u, 30u, 40u, 50u, 60u};

    *parent_left = get_unused_page_num(pager);
    (void)get_page(pager, *parent_left);
    *parent_right = get_unused_page_num(pager);
    (void)get_page(pager, *parent_right);
    if (*parent_left == 0u || *parent_right == 0u ||
        *parent_left == INVALID_PAGE_NUM || *parent_right == INVALID_PAGE_NUM ||
        *parent_left == *parent_right) {
        return false;
    }

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
        write_u32(leaf + PARENT_POINTER_OFFSET,
                  i < 3u ? *parent_left : *parent_right);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            i == 0u ? 0u : leaves[i - 1u]);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
            i + 1u == LEAF_COUNT ? 0u : leaves[i + 1u]);
        if (!raw_insert(schema, leaf, keys[i]) ||
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) {
            return false;
        }
        mark_page_dirty(pager, leaves[i]);
    }

    const uint32_t left_children[3] = {leaves[0], leaves[1], leaves[2]};
    const uint32_t left_keys[2] = {10u, 20u};
    const uint32_t right_children[3] = {leaves[3], leaves[4], leaves[5]};
    const uint32_t right_keys[2] = {40u, 50u};
    const uint32_t root_children[2] = {*parent_left, *parent_right};
    const uint32_t root_keys[1] = {30u};
    if (!build_internal(pager,
                        *parent_left,
                        root,
                        false,
                        left_children,
                        left_keys,
                        3u) ||
        !build_internal(pager,
                        *parent_right,
                        root,
                        false,
                        right_children,
                        right_keys,
                        3u) ||
        !build_internal(pager,
                        root,
                        0u,
                        true,
                        root_children,
                        root_keys,
                        2u)) {
        return false;
    }
    mark_page_dirty(pager, *parent_left);
    mark_page_dirty(pager, *parent_right);
    mark_page_dirty(pager, root);
    pager_commit(pager);

    return tinydb_record_scan(table, schema, NULL, NULL) == BASELINE_ROWS &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool internal_matches(Table* table,
                             uint32_t page_num,
                             const uint32_t* children,
                             const uint32_t* separators,
                             uint32_t child_count) {
    unsigned char page[PAGE_SIZE];
    memcpy(page, get_page(table->pager, page_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(page, PAGE_SIZE) ||
        tinydb_parent_stage_read_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            child_count - 1u) {
        return false;
    }
    for (uint32_t i = 0u; i < child_count; i++) {
        if (tinydb_parent_stage_child_at(page, i) != children[i]) return false;
        if (i + 1u < child_count &&
            tinydb_parent_stage_key_at(page, i) != separators[i]) {
            return false;
        }
    }
    return true;
}

static bool leaf_links(Table* table,
                       uint32_t page_num,
                       uint32_t expected_prev,
                       uint32_t expected_next) {
    unsigned char page[PAGE_SIZE];
    uint32_t prev = INVALID_PAGE_NUM;
    uint32_t next = INVALID_PAGE_NUM;
    memcpy(page, get_page(table->pager, page_num), PAGE_SIZE);
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           tinydb_leaf_page_prev(page, PAGE_SIZE, &prev) &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next) &&
           prev == expected_prev && next == expected_next;
}

static bool leaf_parent(Table* table,
                        uint32_t page_num,
                        uint32_t expected_parent) {
    unsigned char page[PAGE_SIZE];
    memcpy(page, get_page(table->pager, page_num), PAGE_SIZE);
    return page[NODE_TYPE_OFFSET] == (unsigned char)NODE_LEAF &&
           read_u32(page + PARENT_POINTER_OFFSET) == expected_parent;
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

static bool delete_key(Table* table,
                       const TableSchema* schema,
                       uint32_t key,
                       char message[TINYDB_RECORD_MESSAGE_MAX]) {
    message[0] = '\0';
    return tinydb_record_delete(table,
                                schema,
                                key,
                                message,
                                TINYDB_RECORD_MESSAGE_MAX);
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
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
    uint32_t parent_left = 0u;
    uint32_t parent_right = 0u;
    uint32_t leaves[LEAF_COUNT] = {0u, 0u, 0u, 0u, 0u, 0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_tree(db, schema, &parent_left, &parent_right, leaves)) {
        fprintf(stderr, "unable to seed borrow-left tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    const uint32_t root_children[2] = {parent_left, parent_right};
    const uint32_t root_before_keys[1] = {30u};
    const uint32_t root_after_borrow_keys[1] = {20u};
    const uint32_t left_before_children[3] = {leaves[0], leaves[1], leaves[2]};
    const uint32_t left_before_keys[2] = {10u, 20u};
    const uint32_t left_after_children[2] = {leaves[0], leaves[1]};
    const uint32_t left_after_keys[1] = {10u};
    const uint32_t right_after_first_children[2] = {leaves[4], leaves[5]};
    const uint32_t right_after_first_keys[1] = {50u};
    const uint32_t right_after_borrow_children[2] = {leaves[2], leaves[5]};
    const uint32_t right_after_borrow_keys[1] = {30u};
    uint32_t free_before = table->pager->free_page_count;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!delete_key(table, schema, 40u, message) || present(table, schema, 40u) ||
        !internal_matches(table, parent_left, left_before_children,
                          left_before_keys, 3u) ||
        !internal_matches(table, parent_right, right_after_first_children,
                          right_after_first_keys, 2u) ||
        !internal_matches(table, schema->root_page_num, root_children,
                          root_before_keys, 2u) ||
        !leaf_links(table, leaves[2], leaves[1], leaves[4]) ||
        !leaf_links(table, leaves[4], leaves[2], leaves[5]) ||
        table->pager->free_page_count != free_before + 1u ||
        !free_has(table->pager, leaves[3]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 5u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "preparing right-parent underflow failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!exec_ok(db, "BEGIN;") || !delete_key(table, schema, 50u, message) ||
        present(table, schema, 50u) ||
        !internal_matches(table, parent_left, left_after_children,
                          left_after_keys, 2u) ||
        !internal_matches(table, parent_right, right_after_borrow_children,
                          right_after_borrow_keys, 2u) ||
        !internal_matches(table, schema->root_page_num, root_children,
                          root_after_borrow_keys, 2u) ||
        !leaf_links(table, leaves[1], leaves[0], leaves[2]) ||
        !leaf_links(table, leaves[2], leaves[1], leaves[5]) ||
        !leaf_links(table, leaves[5], leaves[2], 0u) ||
        !leaf_parent(table, leaves[2], parent_right) ||
        !leaf_parent(table, leaves[1], parent_left) ||
        table->pager->free_page_count != free_before + 2u ||
        !free_has(table->pager, leaves[3]) ||
        !free_has(table->pager, leaves[4]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 4u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;") ||
        !present(table, schema, 50u) ||
        !internal_matches(table, parent_left, left_before_children,
                          left_before_keys, 3u) ||
        !internal_matches(table, parent_right, right_after_first_children,
                          right_after_first_keys, 2u) ||
        !internal_matches(table, schema->root_page_num, root_children,
                          root_before_keys, 2u) ||
        !leaf_links(table, leaves[2], leaves[1], leaves[4]) ||
        !leaf_links(table, leaves[4], leaves[2], leaves[5]) ||
        !leaf_parent(table, leaves[2], parent_left) ||
        table->pager->free_page_count != free_before + 1u ||
        !free_has(table->pager, leaves[3]) || free_has(table->pager, leaves[4])) {
        fprintf(stderr, "borrow-left rollback failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 50u, message) || present(table, schema, 50u) ||
        !internal_matches(table, parent_left, left_after_children,
                          left_after_keys, 2u) ||
        !internal_matches(table, parent_right, right_after_borrow_children,
                          right_after_borrow_keys, 2u) ||
        !internal_matches(table, schema->root_page_num, root_children,
                          root_after_borrow_keys, 2u) ||
        !leaf_links(table, leaves[1], leaves[0], leaves[2]) ||
        !leaf_links(table, leaves[2], leaves[1], leaves[5]) ||
        !leaf_parent(table, leaves[2], parent_right) ||
        table->pager->free_page_count != free_before + 2u ||
        !free_has(table->pager, leaves[3]) || !free_has(table->pager, leaves[4]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 4u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed borrow-left redistribution failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL || present(table, schema, 40u) || present(table, schema, 50u) ||
        !present(table, schema, 10u) || !present(table, schema, 20u) ||
        !present(table, schema, 30u) || !present(table, schema, 60u) ||
        !internal_matches(table, parent_left, left_after_children,
                          left_after_keys, 2u) ||
        !internal_matches(table, parent_right, right_after_borrow_children,
                          right_after_borrow_keys, 2u) ||
        !internal_matches(table, schema->root_page_num, root_children,
                          root_after_borrow_keys, 2u) ||
        !leaf_links(table, leaves[1], leaves[0], leaves[2]) ||
        !leaf_links(table, leaves[2], leaves[1], leaves[5]) ||
        !leaf_parent(table, leaves[2], parent_right) ||
        table->pager->free_page_count != free_before + 2u ||
        !free_has(table->pager, leaves[3]) || !free_has(table->pager, leaves[4]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 4u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "borrow-left redistribution did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("V2_INTERNAL_BORROW_LEFT_OK redistribute_left=yes donor_reparent=yes "
           "root_separator=yes rollback=yes allocator=yes reopen=yes "
           "integrity=yes wal=yes\n");
    tinydb_close(db);
    return EXIT_SUCCESS;
}
