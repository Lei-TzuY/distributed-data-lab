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
#define BASELINE_ROWS 7u

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

static void make_values(uint32_t id,
                        const char* text,
                        TinyDBValue values[3]) {
    memset(values, 0, sizeof(TinyDBValue) * 3u);
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "%s", text);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 700u;
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

static bool initialize_leaf(Pager* pager,
                            uint32_t page_num,
                            uint32_t parent_page_num,
                            uint32_t prev_page_num,
                            uint32_t next_page_num) {
    unsigned char* leaf = (unsigned char*)get_page(pager, page_num);
    memset(leaf, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(leaf, PAGE_SIZE)) return false;
    leaf[IS_ROOT_OFFSET] = 0u;
    write_u32(leaf + PARENT_POINTER_OFFSET, parent_page_num);
    tinydb_slotted_split_write_u32(
        leaf + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
        prev_page_num);
    tinydb_slotted_split_write_u32(
        leaf + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
        next_page_num);
    return true;
}

static bool seed_height_two_tree(TinyDB* db,
                                 TableSchema* schema,
                                 uint32_t leaf_pages[CHILD_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root_page_num = schema->root_page_num;

    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        leaf_pages[i] = get_unused_page_num(pager);
        if (leaf_pages[i] == 0u || leaf_pages[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaf_pages[i]);
    }

    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        if (!initialize_leaf(pager,
                             leaf_pages[i],
                             root_page_num,
                             i == 0u ? 0u : leaf_pages[i - 1u],
                             i + 1u == CHILD_COUNT ? 0u : leaf_pages[i + 1u])) {
            return false;
        }
    }

    unsigned char* left = (unsigned char*)get_page(pager, leaf_pages[0]);
    unsigned char* middle = (unsigned char*)get_page(pager, leaf_pages[1]);
    unsigned char* right = (unsigned char*)get_page(pager, leaf_pages[2]);
    if (!raw_insert(schema, left, 10u, "left-10") ||
        !raw_insert(schema, left, 20u, "left-20") ||
        !raw_insert(schema, left, 30u, "left-30") ||
        !raw_insert(schema, middle, 40u, "middle-40") ||
        !raw_insert(schema, right, 50u, "right-50") ||
        !raw_insert(schema, right, 60u, "right-60") ||
        !raw_insert(schema, right, 70u, "right-70") ||
        !tinydb_slotted_leaf_v2_validate(left, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(middle, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right, PAGE_SIZE)) {
        return false;
    }
    for (uint32_t i = 0u; i < CHILD_COUNT; i++) mark_page_dirty(pager, leaf_pages[i]);

    unsigned char* root = (unsigned char*)get_page(pager, root_page_num);
    memset(root, 0, PAGE_USABLE_SIZE);
    root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root[IS_ROOT_OFFSET] = 1u;
    write_u32(root + PARENT_POINTER_OFFSET, 0u);
    write_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET, 2u);
    unsigned char* cell0 = root + INTERNAL_NODE_HEADER_SIZE;
    unsigned char* cell1 = cell0 + INTERNAL_NODE_CELL_SIZE;
    write_u32(cell0, leaf_pages[0]);
    write_u32(cell0 + INTERNAL_NODE_CHILD_SIZE, 30u);
    write_u32(cell1, leaf_pages[1]);
    write_u32(cell1 + INTERNAL_NODE_CHILD_SIZE, 40u);
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET, leaf_pages[2]);
    if (!tinydb_parent_stage_validate(root, PAGE_SIZE)) return false;
    mark_page_dirty(pager, root_page_num);
    pager_commit(pager);

    return tinydb_record_scan(table, schema, NULL, NULL) == BASELINE_ROWS &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool root_shape(Table* table,
                       const TableSchema* schema,
                       const uint32_t leaf_pages[CHILD_COUNT],
                       uint32_t first_separator,
                       uint32_t second_separator) {
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           sizeof(root));
    return tinydb_parent_stage_validate(root, PAGE_SIZE) &&
           root[IS_ROOT_OFFSET] != 0u &&
           read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) == 2u &&
           tinydb_parent_stage_child_at(root, 0u) == leaf_pages[0] &&
           tinydb_parent_stage_child_at(root, 1u) == leaf_pages[1] &&
           tinydb_parent_stage_child_at(root, 2u) == leaf_pages[2] &&
           tinydb_parent_stage_key_at(root, 0u) == first_separator &&
           tinydb_parent_stage_key_at(root, 1u) == second_separator;
}

static bool root_shape_without_middle(Table* table,
                                      const TableSchema* schema,
                                      const uint32_t leaf_pages[CHILD_COUNT],
                                      uint32_t separator) {
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           sizeof(root));
    return tinydb_parent_stage_validate(root, PAGE_SIZE) &&
           root[IS_ROOT_OFFSET] != 0u &&
           read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) == 1u &&
           tinydb_parent_stage_child_at(root, 0u) == leaf_pages[0] &&
           tinydb_parent_stage_child_at(root, 1u) == leaf_pages[2] &&
           tinydb_parent_stage_key_at(root, 0u) == separator;
}

static bool leaf_has_max(Table* table,
                         uint32_t page_num,
                         uint32_t expected_count,
                         uint32_t expected_max) {
    unsigned char leaf[PAGE_SIZE];
    memcpy(leaf, get_page(table->pager, page_num), sizeof(leaf));
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE) &&
           tinydb_leaf_page_count(leaf, PAGE_SIZE, &count) &&
           count == expected_count && count > 0u &&
           tinydb_leaf_page_key_at(leaf, PAGE_SIZE, count - 1u, &max_key) &&
           max_key == expected_max;
}

static bool leaf_links(Table* table,
                       uint32_t page_num,
                       uint32_t expected_prev,
                       uint32_t expected_next) {
    unsigned char leaf[PAGE_SIZE];
    uint32_t prev = INVALID_PAGE_NUM;
    uint32_t next = INVALID_PAGE_NUM;
    memcpy(leaf, get_page(table->pager, page_num), sizeof(leaf));
    return tinydb_leaf_page_prev(leaf, PAGE_SIZE, &prev) &&
           tinydb_leaf_page_next(leaf, PAGE_SIZE, &next) &&
           prev == expected_prev && next == expected_next;
}

static bool free_list_contains(const Pager* pager, uint32_t page_num) {
    if (pager == NULL) return false;
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static bool record_present(Table* table,
                           const TableSchema* schema,
                           uint32_t key) {
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
    uint32_t leaf_pages[CHILD_COUNT] = {0u, 0u, 0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_height_two_tree(db, schema, leaf_pages) ||
        !root_shape(table, schema, leaf_pages, 30u, 40u)) {
        fprintf(stderr, "unable to seed height-two V2 max-delete tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!exec_ok(db, "BEGIN;") ||
        !delete_key(table, schema, 30u, message) ||
        record_present(table, schema, 30u) ||
        !root_shape(table, schema, leaf_pages, 20u, 40u) ||
        !leaf_has_max(table, leaf_pages[0], 2u, 20u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "transactional separator-update delete failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!record_present(table, schema, 30u) ||
        !root_shape(table, schema, leaf_pages, 30u, 40u) ||
        !leaf_has_max(table, leaf_pages[0], 3u, 30u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "separator-update rollback did not restore the tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t baseline_free_count = table->pager->free_page_count;
    if (!exec_ok(db, "BEGIN;") ||
        !delete_key(table, schema, 40u, message) ||
        record_present(table, schema, 40u) ||
        !root_shape_without_middle(table, schema, leaf_pages, 30u) ||
        !leaf_links(table, leaf_pages[0], 0u, leaf_pages[2]) ||
        !leaf_links(table, leaf_pages[2], leaf_pages[0], 0u) ||
        table->pager->free_page_count != baseline_free_count + 1u ||
        !free_list_contains(table->pager, leaf_pages[1]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "transactional empty-leaf removal failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!record_present(table, schema, 40u) ||
        !root_shape(table, schema, leaf_pages, 30u, 40u) ||
        !leaf_links(table, leaf_pages[0], 0u, leaf_pages[1]) ||
        !leaf_links(table, leaf_pages[1], leaf_pages[0], leaf_pages[2]) ||
        !leaf_links(table, leaf_pages[2], leaf_pages[1], 0u) ||
        table->pager->free_page_count != baseline_free_count ||
        free_list_contains(table->pager, leaf_pages[1]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "empty-leaf rollback did not restore topology or allocator state\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 40u, message) ||
        record_present(table, schema, 40u) ||
        !root_shape_without_middle(table, schema, leaf_pages, 30u) ||
        !leaf_links(table, leaf_pages[0], 0u, leaf_pages[2]) ||
        !leaf_links(table, leaf_pages[2], leaf_pages[0], 0u) ||
        table->pager->free_page_count != baseline_free_count + 1u ||
        !free_list_contains(table->pager, leaf_pages[1]) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed empty-leaf removal failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 30u, message) ||
        record_present(table, schema, 30u) ||
        !root_shape_without_middle(table, schema, leaf_pages, 20u) ||
        !leaf_has_max(table, leaf_pages[0], 2u, 20u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 2u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed separator-update delete failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!delete_key(table, schema, 70u, message) ||
        record_present(table, schema, 70u) ||
        !root_shape_without_middle(table, schema, leaf_pages, 20u) ||
        !leaf_has_max(table, leaf_pages[2], 2u, 60u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 3u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "root-rightmost max delete failed: %s\n", message);
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL ||
        !root_shape_without_middle(table, schema, leaf_pages, 20u) ||
        record_present(table, schema, 30u) ||
        record_present(table, schema, 40u) ||
        record_present(table, schema, 70u) ||
        !record_present(table, schema, 20u) ||
        !record_present(table, schema, 60u) ||
        !leaf_links(table, leaf_pages[0], 0u, leaf_pages[2]) ||
        !leaf_links(table, leaf_pages[2], leaf_pages[0], 0u) ||
        table->pager->free_page_count != baseline_free_count + 1u ||
        !free_list_contains(table->pager, leaf_pages[1]) ||
        !leaf_has_max(table, leaf_pages[0], 2u, 20u) ||
        !leaf_has_max(table, leaf_pages[2], 2u, 60u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 3u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "V2 empty-leaf/max-key delete state did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("V2_MAX_DELETE_OK separator_update=yes rollback=yes "
           "empty_leaf_remove=yes allocator_rollback=yes sibling_relink=yes "
           "root_rightmost=yes reopen=yes integrity=yes wal=yes\n");
    tinydb_close(db);
    return EXIT_SUCCESS;
}
