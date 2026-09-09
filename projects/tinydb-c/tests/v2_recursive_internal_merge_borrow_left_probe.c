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

#define LEAF_COUNT 10u
#define PARENT_COUNT 5u
#define GRAND_COUNT 2u
#define BASELINE_ROWS 10u

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
    snprintf(values[1].text, sizeof(values[1].text), "recursive-left-%u", id);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 13000u;
    return tinydb_record_encode(schema, values, 3u, &record,
                                message, sizeof(message)) &&
           tinydb_record_payload_from_record(schema, &record, &payload,
                                             message, sizeof(message)) &&
           tinydb_row_envelope_encode_compact_v2(schema, &payload,
                                                 envelope, PAGE_SIZE,
                                                 &envelope_length) &&
           envelope_length > 0u && envelope_length <= UINT16_MAX &&
           tinydb_slotted_leaf_v2_insert(page, PAGE_SIZE, id, envelope,
                                         (uint16_t)envelope_length);
}

static bool build_internal(Pager* pager,
                           uint32_t page_num,
                           uint32_t parent_page_num,
                           bool is_root,
                           const uint32_t* children,
                           const uint32_t* separators,
                           uint32_t child_count) {
    if (pager == NULL || children == NULL || separators == NULL ||
        child_count < 2u) return false;
    unsigned char* page = (unsigned char*)get_page(pager, page_num);
    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = is_root ? 1u : 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, child_count - 1u);
    for (uint32_t i = 0u; i + 1u < child_count; i++) {
        unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE +
                              (size_t)i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, children[i]);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, separators[i]);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              children[child_count - 1u]);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool allocate_pages(Pager* pager,
                           uint32_t grands[GRAND_COUNT],
                           uint32_t parents[PARENT_COUNT],
                           uint32_t leaves[LEAF_COUNT]) {
    for (uint32_t i = 0u; i < GRAND_COUNT; i++) {
        grands[i] = get_unused_page_num(pager);
        if (grands[i] == 0u || grands[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, grands[i]);
    }
    for (uint32_t i = 0u; i < PARENT_COUNT; i++) {
        parents[i] = get_unused_page_num(pager);
        if (parents[i] == 0u || parents[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, parents[i]);
    }
    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        leaves[i] = get_unused_page_num(pager);
        if (leaves[i] == 0u || leaves[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaves[i]);
    }
    return true;
}

static bool seed_tree(TinyDB* db,
                      TableSchema* schema,
                      uint32_t grands[GRAND_COUNT],
                      uint32_t parents[PARENT_COUNT],
                      uint32_t leaves[LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    const uint32_t keys[LEAF_COUNT] = {
        10u,20u,30u,40u,50u,60u,70u,80u,90u,100u
    };
    if (!allocate_pages(pager, grands, parents, leaves)) return false;

    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        unsigned char* leaf = (unsigned char*)get_page(pager, leaves[i]);
        memset(leaf, 0, PAGE_SIZE);
        if (!tinydb_slotted_leaf_v2_init(leaf, PAGE_SIZE)) return false;
        leaf[IS_ROOT_OFFSET] = 0u;
        write_u32(leaf + PARENT_POINTER_OFFSET, parents[i / 2u]);
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

    for (uint32_t i = 0u; i < PARENT_COUNT; i++) {
        const uint32_t children[2] = {leaves[2u * i], leaves[2u * i + 1u]};
        const uint32_t separators[1] = {keys[2u * i]};
        uint32_t grand = i < 3u ? grands[0] : grands[1];
        if (!build_internal(pager, parents[i], grand, false,
                            children, separators, 2u)) return false;
        mark_page_dirty(pager, parents[i]);
    }

    const uint32_t left_children[3] = {parents[0],parents[1],parents[2]};
    const uint32_t left_keys[2] = {20u,40u};
    const uint32_t right_children[2] = {parents[3],parents[4]};
    const uint32_t right_keys[1] = {80u};
    const uint32_t root_children[2] = {grands[0],grands[1]};
    const uint32_t root_keys[1] = {60u};
    if (!build_internal(pager, grands[0], schema->root_page_num, false,
                        left_children, left_keys, 3u) ||
        !build_internal(pager, grands[1], schema->root_page_num, false,
                        right_children, right_keys, 2u) ||
        !build_internal(pager, schema->root_page_num, 0u, true,
                        root_children, root_keys, 2u)) return false;
    mark_page_dirty(pager, grands[0]);
    mark_page_dirty(pager, grands[1]);
    mark_page_dirty(pager, schema->root_page_num);
    pager_commit(pager);
    return tinydb_record_scan(table, schema, NULL, NULL) == BASELINE_ROWS &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool internal_matches(Table* table,
                             uint32_t page_num,
                             uint32_t expected_parent,
                             bool expected_root,
                             const uint32_t* children,
                             const uint32_t* separators,
                             uint32_t child_count) {
    unsigned char page[PAGE_SIZE];
    memcpy(page, get_page(table->pager, page_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(page, PAGE_SIZE) ||
        (page[IS_ROOT_OFFSET] != 0u) != expected_root ||
        read_u32(page + PARENT_POINTER_OFFSET) != expected_parent ||
        read_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET) != child_count - 1u) {
        return false;
    }
    for (uint32_t i = 0u; i < child_count; i++) {
        if (tinydb_parent_stage_child_at(page, i) != children[i]) return false;
        if (i + 1u < child_count &&
            tinydb_parent_stage_key_at(page, i) != separators[i]) return false;
    }
    return true;
}

static bool leaf_state(Table* table,
                       uint32_t page_num,
                       uint32_t expected_parent,
                       uint32_t expected_prev,
                       uint32_t expected_next) {
    unsigned char page[PAGE_SIZE];
    uint32_t prev = INVALID_PAGE_NUM;
    uint32_t next = INVALID_PAGE_NUM;
    memcpy(page, get_page(table->pager, page_num), PAGE_SIZE);
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           read_u32(page + PARENT_POINTER_OFFSET) == expected_parent &&
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

static bool original_state(Table* table,
                           TableSchema* schema,
                           const uint32_t grands[GRAND_COUNT],
                           const uint32_t parents[PARENT_COUNT],
                           const uint32_t leaves[LEAF_COUNT],
                           uint32_t free_before) {
    const uint32_t root_children[2] = {grands[0],grands[1]};
    const uint32_t root_keys[1] = {60u};
    const uint32_t left_children[3] = {parents[0],parents[1],parents[2]};
    const uint32_t left_keys[2] = {20u,40u};
    const uint32_t right_children[2] = {parents[3],parents[4]};
    const uint32_t right_keys[1] = {80u};
    return internal_matches(table, schema->root_page_num, 0u, true,
                            root_children, root_keys, 2u) &&
           internal_matches(table, grands[0], schema->root_page_num, false,
                            left_children, left_keys, 3u) &&
           internal_matches(table, grands[1], schema->root_page_num, false,
                            right_children, right_keys, 2u) &&
           table->pager->free_page_count == free_before &&
           tinydb_record_scan(table, schema, NULL, NULL) == BASELINE_ROWS &&
           present(table, schema, 80u) &&
           leaf_state(table, leaves[5], parents[2], leaves[4], leaves[6]) &&
           leaf_state(table, leaves[6], parents[3], leaves[5], leaves[7]) &&
           leaf_state(table, leaves[7], parents[3], leaves[6], leaves[8]) &&
           leaf_state(table, leaves[8], parents[4], leaves[7], leaves[9]);
}

static bool cascaded_state(Table* table,
                           TableSchema* schema,
                           const uint32_t grands[GRAND_COUNT],
                           const uint32_t parents[PARENT_COUNT],
                           const uint32_t leaves[LEAF_COUNT],
                           uint32_t free_before) {
    const uint32_t root_children[2] = {grands[0],grands[1]};
    const uint32_t root_keys[1] = {40u};
    const uint32_t left_children[2] = {parents[0],parents[1]};
    const uint32_t left_keys[1] = {20u};
    const uint32_t right_children[2] = {parents[2],parents[4]};
    const uint32_t right_keys[1] = {60u};
    const uint32_t donor_children[2] = {leaves[4],leaves[5]};
    const uint32_t donor_keys[1] = {50u};
    const uint32_t kept_children[3] = {leaves[6],leaves[8],leaves[9]};
    const uint32_t kept_keys[2] = {70u,90u};
    if (!internal_matches(table, schema->root_page_num, 0u, true,
                          root_children, root_keys, 2u) ||
        !internal_matches(table, grands[0], schema->root_page_num, false,
                          left_children, left_keys, 2u) ||
        !internal_matches(table, grands[1], schema->root_page_num, false,
                          right_children, right_keys, 2u) ||
        !internal_matches(table, parents[2], grands[1], false,
                          donor_children, donor_keys, 2u) ||
        !internal_matches(table, parents[4], grands[1], false,
                          kept_children, kept_keys, 3u) ||
        !leaf_state(table, leaves[5], parents[2], leaves[4], leaves[6]) ||
        !leaf_state(table, leaves[6], parents[4], leaves[5], leaves[8]) ||
        !leaf_state(table, leaves[8], parents[4], leaves[6], leaves[9]) ||
        table->pager->free_page_count != free_before + 2u ||
        !free_has(table->pager, leaves[7]) ||
        !free_has(table->pager, parents[3]) ||
        present(table, schema, 80u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 9u) {
        return false;
    }

    const uint32_t p0_children[2] = {leaves[0],leaves[1]};
    const uint32_t p0_keys[1] = {10u};
    const uint32_t p1_children[2] = {leaves[2],leaves[3]};
    const uint32_t p1_keys[1] = {30u};
    return internal_matches(table, parents[0], grands[0], false,
                            p0_children, p0_keys, 2u) &&
           internal_matches(table, parents[1], grands[0], false,
                            p1_children, p1_keys, 2u) &&
           leaf_state(table, leaves[3], parents[1], leaves[2], leaves[4]) &&
           leaf_state(table, leaves[9], parents[4], leaves[8], 0u);
}

static bool delete_key(Table* table,
                       const TableSchema* schema,
                       uint32_t key,
                       char message[TINYDB_RECORD_MESSAGE_MAX]) {
    message[0] = '\0';
    return tinydb_record_delete(table, schema, key,
                                message, TINYDB_RECORD_MESSAGE_MAX);
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
    uint32_t grands[GRAND_COUNT] = {0u,0u};
    uint32_t parents[PARENT_COUNT] = {0u,0u,0u,0u,0u};
    uint32_t leaves[LEAF_COUNT] = {0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_tree(db, schema, grands, parents, leaves)) {
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t free_before = table->pager->free_page_count;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!exec_ok(db, "BEGIN;") ||
        !delete_key(table, schema, 80u, message) ||
        !cascaded_state(table, schema, grands, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;") ||
        !original_state(table, schema, grands, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "mirrored recursive cascade rollback failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 80u, message) ||
        !cascaded_state(table, schema, grands, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "mirrored recursive cascade commit failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    bool ok = schema != NULL &&
              cascaded_state(table, schema, grands, parents, leaves, free_before) &&
              exec_ok(db, "PRAGMA integrity_check;");
    tinydb_close(db);
    if (!ok) return EXIT_FAILURE;

    printf("V2_RECURSIVE_INTERNAL_MERGE_BORROW_LEFT_OK mirror=yes height4=yes "
           "root_separator_down=yes donor_subtree=yes lower_merge=yes "
           "rollback=yes allocator=yes wal=yes reopen=yes integrity=yes\n");
    return EXIT_SUCCESS;
}
