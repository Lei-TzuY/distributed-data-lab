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

#define LEAF_COUNT 4u
#define BASELINE_ROWS 4u

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
    snprintf(values[1].text, sizeof(values[1].text), "merge-root-%u", id);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 4000u;
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
    const uint32_t keys[LEAF_COUNT] = {10u, 20u, 30u, 40u};

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
                  i < 2u ? *parent_left : *parent_right);
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

    const uint32_t left_children[2] = {leaves[0], leaves[1]};
    const uint32_t left_keys[1] = {10u};
    const uint32_t right_children[2] = {leaves[2], leaves[3]};
    const uint32_t right_keys[1] = {30u};
    const uint32_t root_children[2] = {*parent_left, *parent_right};
    const uint32_t root_keys[1] = {20u};
    if (!build_internal(pager,
                        *parent_left,
                        root,
                        false,
                        left_children,
                        left_keys,
                        2u) ||
        !build_internal(pager,
                        *parent_right,
                        root,
                        false,
                        right_children,
                        right_keys,
                        2u) ||
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
                             bool is_root,
                             const uint32_t* children,
                             const uint32_t* separators,
                             uint32_t child_count) {
    unsigned char page[PAGE_SIZE];
    memcpy(page, get_page(table->pager, page_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(page, PAGE_SIZE) ||
        (page[IS_ROOT_OFFSET] != 0u) != is_root ||
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

static bool merged_state(Table* table,
                         TableSchema* schema,
                         const uint32_t leaves[LEAF_COUNT],
                         uint32_t removed_index,
                         uint32_t parent_left,
                         uint32_t parent_right,
                         uint32_t free_before) {
    const uint32_t keys[LEAF_COUNT] = {10u, 20u, 30u, 40u};
    uint32_t survivors[3];
    uint32_t survivor_keys[3];
    uint32_t out = 0u;
    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        if (i == removed_index) continue;
        survivors[out] = leaves[i];
        survivor_keys[out] = keys[i];
        out++;
    }
    if (out != 3u) return false;
    const uint32_t root_keys[2] = {survivor_keys[0], survivor_keys[1]};
    if (!internal_matches(table,
                          schema->root_page_num,
                          true,
                          survivors,
                          root_keys,
                          3u)) {
        return false;
    }
    for (uint32_t i = 0u; i < 3u; i++) {
        if (!leaf_state(table,
                        survivors[i],
                        schema->root_page_num,
                        i == 0u ? 0u : survivors[i - 1u],
                        i == 2u ? 0u : survivors[i + 1u])) {
            return false;
        }
    }
    return table->pager->free_page_count == free_before + 3u &&
           free_has(table->pager, leaves[removed_index]) &&
           free_has(table->pager, parent_left) &&
           free_has(table->pager, parent_right) &&
           !present(table, schema, keys[removed_index]) &&
           tinydb_record_scan(table, schema, NULL, NULL) == 3u;
}

static bool original_state(Table* table,
                           TableSchema* schema,
                           const uint32_t leaves[LEAF_COUNT],
                           uint32_t parent_left,
                           uint32_t parent_right,
                           uint32_t free_before) {
    const uint32_t left_children[2] = {leaves[0], leaves[1]};
    const uint32_t left_keys[1] = {10u};
    const uint32_t right_children[2] = {leaves[2], leaves[3]};
    const uint32_t right_keys[1] = {30u};
    const uint32_t root_children[2] = {parent_left, parent_right};
    const uint32_t root_keys[1] = {20u};
    if (!internal_matches(table,
                          parent_left,
                          false,
                          left_children,
                          left_keys,
                          2u) ||
        !internal_matches(table,
                          parent_right,
                          false,
                          right_children,
                          right_keys,
                          2u) ||
        !internal_matches(table,
                          schema->root_page_num,
                          true,
                          root_children,
                          root_keys,
                          2u)) {
        return false;
    }
    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        if (!leaf_state(table,
                        leaves[i],
                        i < 2u ? parent_left : parent_right,
                        i == 0u ? 0u : leaves[i - 1u],
                        i + 1u == LEAF_COUNT ? 0u : leaves[i + 1u])) {
            return false;
        }
    }
    return table->pager->free_page_count == free_before &&
           !free_has(table->pager, parent_left) &&
           !free_has(table->pager, parent_right) &&
           tinydb_record_scan(table, schema, NULL, NULL) == BASELINE_ROWS;
}

static bool run_case(const char* path, uint32_t removed_index) {
    const uint32_t keys[LEAF_COUNT] = {10u, 20u, 30u, 40u};
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
    uint32_t parent_left = 0u;
    uint32_t parent_right = 0u;
    uint32_t leaves[LEAF_COUNT] = {0u, 0u, 0u, 0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_tree(db, schema, &parent_left, &parent_right, leaves)) {
        fprintf(stderr, "unable to seed minimum internal merge tree\n");
        tinydb_close(db);
        return false;
    }
    uint32_t free_before = table->pager->free_page_count;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!exec_ok(db, "BEGIN;") ||
        !delete_key(table, schema, keys[removed_index], message) ||
        !merged_state(table,
                      schema,
                      leaves,
                      removed_index,
                      parent_left,
                      parent_right,
                      free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;") ||
        !present(table, schema, keys[removed_index]) ||
        !original_state(table,
                        schema,
                        leaves,
                        parent_left,
                        parent_right,
                        free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr,
                "transactional internal merge/root collapse failed for key %u: %s\n",
                keys[removed_index],
                message);
        tinydb_close(db);
        return false;
    }

    if (!delete_key(table, schema, keys[removed_index], message) ||
        !merged_state(table,
                      schema,
                      leaves,
                      removed_index,
                      parent_left,
                      parent_right,
                      free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr,
                "committed internal merge/root collapse failed for key %u: %s\n",
                keys[removed_index],
                message);
        tinydb_close(db);
        return false;
    }

    tinydb_close(db);
    db = tinydb_open(path);
    if (db == NULL) return false;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    bool ok = schema != NULL &&
              merged_state(table,
                           schema,
                           leaves,
                           removed_index,
                           parent_left,
                           parent_right,
                           free_before) &&
              exec_ok(db, "PRAGMA integrity_check;");
    if (!ok) {
        fprintf(stderr,
                "internal merge/root collapse did not survive reopen for key %u\n",
                keys[removed_index]);
    }
    tinydb_close(db);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    char left_path[1024];
    char right_path[1024];
    if (snprintf(left_path, sizeof(left_path), "%s.left", argv[1]) < 0 ||
        snprintf(right_path, sizeof(right_path), "%s.right", argv[1]) < 0) {
        return EXIT_FAILURE;
    }
    if (!run_case(left_path, 1u) || !run_case(right_path, 2u)) {
        return EXIT_FAILURE;
    }
    printf("V2_INTERNAL_MERGE_ROOT_OK left=yes right=yes height_drop=yes "
           "descendant_reparent=yes rollback=yes allocator=yes reopen=yes "
           "integrity=yes wal=yes\n");
    return EXIT_SUCCESS;
}
