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
#define PARENT_COUNT 3u
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
    snprintf(values[1].text, sizeof(values[1].text), "wide-merge-%u", id);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 6000u;
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
                      uint32_t parents[PARENT_COUNT],
                      uint32_t leaves[LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root = schema->root_page_num;
    const uint32_t keys[LEAF_COUNT] = {10u, 20u, 30u, 40u, 50u, 60u};

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
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) {
            return false;
        }
        mark_page_dirty(pager, leaves[i]);
    }

    const uint32_t p0_children[2] = {leaves[0], leaves[1]};
    const uint32_t p0_keys[1] = {10u};
    const uint32_t p1_children[2] = {leaves[2], leaves[3]};
    const uint32_t p1_keys[1] = {30u};
    const uint32_t p2_children[2] = {leaves[4], leaves[5]};
    const uint32_t p2_keys[1] = {50u};
    const uint32_t root_children[3] = {parents[0], parents[1], parents[2]};
    const uint32_t root_keys[2] = {20u, 40u};
    if (!build_internal(pager, parents[0], root, false,
                        p0_children, p0_keys, 2u) ||
        !build_internal(pager, parents[1], root, false,
                        p1_children, p1_keys, 2u) ||
        !build_internal(pager, parents[2], root, false,
                        p2_children, p2_keys, 2u) ||
        !build_internal(pager, root, 0u, true,
                        root_children, root_keys, 3u)) {
        return false;
    }
    for (uint32_t i = 0u; i < PARENT_COUNT; i++) mark_page_dirty(pager, parents[i]);
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

static bool original_state(Table* table,
                           TableSchema* schema,
                           const uint32_t parents[PARENT_COUNT],
                           const uint32_t leaves[LEAF_COUNT],
                           uint32_t free_before) {
    const uint32_t p0_children[2] = {leaves[0], leaves[1]};
    const uint32_t p0_keys[1] = {10u};
    const uint32_t p1_children[2] = {leaves[2], leaves[3]};
    const uint32_t p1_keys[1] = {30u};
    const uint32_t p2_children[2] = {leaves[4], leaves[5]};
    const uint32_t p2_keys[1] = {50u};
    const uint32_t root_children[3] = {parents[0], parents[1], parents[2]};
    const uint32_t root_keys[2] = {20u, 40u};
    return internal_matches(table, parents[0], p0_children, p0_keys, 2u) &&
           internal_matches(table, parents[1], p1_children, p1_keys, 2u) &&
           internal_matches(table, parents[2], p2_children, p2_keys, 2u) &&
           internal_matches(table, schema->root_page_num,
                            root_children, root_keys, 3u) &&
           leaf_state(table, leaves[0], parents[0], 0u, leaves[1]) &&
           leaf_state(table, leaves[1], parents[0], leaves[0], leaves[2]) &&
           leaf_state(table, leaves[2], parents[1], leaves[1], leaves[3]) &&
           leaf_state(table, leaves[3], parents[1], leaves[2], leaves[4]) &&
           leaf_state(table, leaves[4], parents[2], leaves[3], leaves[5]) &&
           leaf_state(table, leaves[5], parents[2], leaves[4], 0u) &&
           table->pager->free_page_count == free_before;
}

static bool merged_state(Table* table,
                         TableSchema* schema,
                         bool merge_right,
                         const uint32_t parents[PARENT_COUNT],
                         const uint32_t leaves[LEAF_COUNT],
                         uint32_t free_before) {
    if (merge_right) {
        const uint32_t merged_children[3] = {leaves[2], leaves[4], leaves[5]};
        const uint32_t merged_keys[2] = {30u, 50u};
        const uint32_t root_children[2] = {parents[0], parents[2]};
        const uint32_t root_keys[1] = {20u};
        return internal_matches(table, parents[2], merged_children, merged_keys, 3u) &&
               internal_matches(table, schema->root_page_num,
                                root_children, root_keys, 2u) &&
               leaf_state(table, leaves[1], parents[0], leaves[0], leaves[2]) &&
               leaf_state(table, leaves[2], parents[2], leaves[1], leaves[4]) &&
               leaf_state(table, leaves[4], parents[2], leaves[2], leaves[5]) &&
               leaf_state(table, leaves[5], parents[2], leaves[4], 0u) &&
               table->pager->free_page_count == free_before + 2u &&
               free_has(table->pager, leaves[3]) && free_has(table->pager, parents[1]) &&
               !present(table, schema, 40u) &&
               tinydb_record_scan(table, schema, NULL, NULL) == 5u;
    }

    const uint32_t merged_children[3] = {leaves[0], leaves[1], leaves[3]};
    const uint32_t merged_keys[2] = {10u, 20u};
    const uint32_t root_children[2] = {parents[1], parents[2]};
    const uint32_t root_keys[1] = {40u};
    return internal_matches(table, parents[1], merged_children, merged_keys, 3u) &&
           internal_matches(table, schema->root_page_num,
                            root_children, root_keys, 2u) &&
           leaf_state(table, leaves[0], parents[1], 0u, leaves[1]) &&
           leaf_state(table, leaves[1], parents[1], leaves[0], leaves[3]) &&
           leaf_state(table, leaves[3], parents[1], leaves[1], leaves[4]) &&
           leaf_state(table, leaves[4], parents[2], leaves[3], leaves[5]) &&
           table->pager->free_page_count == free_before + 2u &&
           free_has(table->pager, leaves[2]) && free_has(table->pager, parents[0]) &&
           !present(table, schema, 30u) &&
           tinydb_record_scan(table, schema, NULL, NULL) == 5u;
}

static bool run_case(const char* path, bool merge_right) {
    remove(path);
    TinyDB* db = tinydb_open(path);
    if (db == NULL ||
        !exec_ok(db,
                 "CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return false;
    }
    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t parents[PARENT_COUNT] = {0u, 0u, 0u};
    uint32_t leaves[LEAF_COUNT] = {0u, 0u, 0u, 0u, 0u, 0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_tree(db, schema, parents, leaves)) {
        fprintf(stderr, "unable to seed wide-root merge tree\n");
        tinydb_close(db);
        return false;
    }
    uint32_t free_before = table->pager->free_page_count;
    uint32_t removed_key = merge_right ? 40u : 30u;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!exec_ok(db, "BEGIN;") ||
        !delete_key(table, schema, removed_key, message) ||
        !merged_state(table, schema, merge_right, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;") ||
        !present(table, schema, removed_key) ||
        !original_state(table, schema, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "wide-root merge rollback failed for key %u: %s\n",
                removed_key, message);
        tinydb_close(db);
        return false;
    }

    if (!delete_key(table, schema, removed_key, message) ||
        !merged_state(table, schema, merge_right, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "wide-root merge commit failed for key %u: %s\n",
                removed_key, message);
        tinydb_close(db);
        return false;
    }

    tinydb_close(db);
    db = tinydb_open(path);
    if (db == NULL) return false;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    bool ok = schema != NULL &&
              merged_state(table, schema, merge_right, parents, leaves, free_before) &&
              exec_ok(db, "PRAGMA integrity_check;");
    if (!ok) fprintf(stderr, "wide-root merge did not survive reopen for key %u\n", removed_key);
    tinydb_close(db);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    char right_path[1024];
    char left_path[1024];
    if (snprintf(right_path, sizeof(right_path), "%s.right", argv[1]) < 0 ||
        snprintf(left_path, sizeof(left_path), "%s.left", argv[1]) < 0) {
        return EXIT_FAILURE;
    }
    if (!run_case(right_path, true) || !run_case(left_path, false)) {
        return EXIT_FAILURE;
    }
    printf("V2_INTERNAL_MERGE_WIDE_ROOT_OK right=yes left=yes root_shrink=yes "
           "height_stable=yes descendant_reparent=yes third_subtree=yes "
           "rollback=yes allocator=yes reopen=yes integrity=yes wal=yes\n");
    return EXIT_SUCCESS;
}
