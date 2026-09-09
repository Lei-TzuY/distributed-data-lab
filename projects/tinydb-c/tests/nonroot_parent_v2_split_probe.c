#include "engine.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_split.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FULL_CHILD_COUNT (INTERNAL_NODE_MAX_KEYS + 1u)
#define EXTRA_CHILD_COUNT 2u
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
    values[2].int_value = id + 777u;
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

static void initialize_internal(unsigned char* page,
                                uint32_t parent_page_num,
                                bool is_root,
                                uint32_t key_count) {
    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = is_root ? 1u : 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, key_count);
}

static bool seed_height_three_tree(TinyDB* db,
                                   TableSchema* schema,
                                   uint32_t* full_parent_page_num,
                                   uint32_t* sibling_parent_page_num,
                                   uint32_t* target_leaf_page_num,
                                   uint32_t* candidate_key,
                                   uint32_t* baseline_rows) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root_page_num = schema->root_page_num;

    *full_parent_page_num = get_unused_page_num(pager);
    (void)get_page(pager, *full_parent_page_num);
    *sibling_parent_page_num = get_unused_page_num(pager);
    (void)get_page(pager, *sibling_parent_page_num);
    if (*full_parent_page_num == 0u || *sibling_parent_page_num == 0u ||
        *full_parent_page_num == *sibling_parent_page_num) {
        return false;
    }

    uint32_t leaves[FULL_CHILD_COUNT + EXTRA_CHILD_COUNT];
    for (uint32_t i = 0u; i < FULL_CHILD_COUNT + EXTRA_CHILD_COUNT; i++) {
        leaves[i] = get_unused_page_num(pager);
        if (leaves[i] == 0u || leaves[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaves[i]);
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

    for (uint32_t i = 0u; i < FULL_CHILD_COUNT + EXTRA_CHILD_COUNT; i++) {
        uint32_t parent_page_num = i < FULL_CHILD_COUNT
            ? *full_parent_page_num
            : *sibling_parent_page_num;
        uint32_t prev_page_num = i == 0u ? 0u : leaves[i - 1u];
        uint32_t next_page_num = i + 1u == FULL_CHILD_COUNT + EXTRA_CHILD_COUNT
            ? 0u
            : leaves[i + 1u];
        if (!initialize_leaf(pager,
                             leaves[i],
                             parent_page_num,
                             prev_page_num,
                             next_page_num)) {
            return false;
        }

        unsigned char* leaf = (unsigned char*)get_page(pager, leaves[i]);
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
            *target_leaf_page_num = leaves[i];
        } else {
            if (!raw_insert(schema, leaf, max_key, "s")) return false;
        }
        if (!tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaves[i]);
    }

    unsigned char* full_parent =
        (unsigned char*)get_page(pager, *full_parent_page_num);
    initialize_internal(full_parent,
                        root_page_num,
                        false,
                        INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        unsigned char* cell = full_parent + INTERNAL_NODE_HEADER_SIZE +
                              i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, leaves[i]);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, (i + 1u) * RANGE_STEP);
    }
    write_u32(full_parent + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              leaves[FULL_CHILD_COUNT - 1u]);
    mark_page_dirty(pager, *full_parent_page_num);

    unsigned char* sibling_parent =
        (unsigned char*)get_page(pager, *sibling_parent_page_num);
    initialize_internal(sibling_parent, root_page_num, false, 1u);
    unsigned char* sibling_cell = sibling_parent + INTERNAL_NODE_HEADER_SIZE;
    write_u32(sibling_cell, leaves[FULL_CHILD_COUNT]);
    write_u32(sibling_cell + INTERNAL_NODE_CHILD_SIZE,
              (FULL_CHILD_COUNT + 1u) * RANGE_STEP);
    write_u32(sibling_parent + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              leaves[FULL_CHILD_COUNT + 1u]);
    mark_page_dirty(pager, *sibling_parent_page_num);

    unsigned char* root = (unsigned char*)get_page(pager, root_page_num);
    initialize_internal(root, 0u, true, 1u);
    unsigned char* root_cell = root + INTERNAL_NODE_HEADER_SIZE;
    write_u32(root_cell, *full_parent_page_num);
    write_u32(root_cell + INTERNAL_NODE_CHILD_SIZE,
              FULL_CHILD_COUNT * RANGE_STEP);
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              *sibling_parent_page_num);
    mark_page_dirty(pager, root_page_num);

    pager_commit(pager);
    *baseline_rows = tinydb_record_scan(table, schema, NULL, NULL);
    return *target_leaf_page_num != INVALID_PAGE_NUM &&
           *baseline_rows >= FULL_CHILD_COUNT + EXTRA_CHILD_COUNT &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool root_has_inserted_internal(Table* table,
                                       const TableSchema* schema,
                                       uint32_t old_full_parent_page_num,
                                       uint32_t sibling_parent_page_num,
                                       uint32_t* new_internal_page_num_out) {
    unsigned char root[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           PAGE_SIZE);
    if (get_node_type(root) != NODE_INTERNAL || root[IS_ROOT_OFFSET] == 0u ||
        read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u) {
        return false;
    }

    uint32_t child0 = read_u32(root + INTERNAL_NODE_HEADER_SIZE);
    uint32_t child1 = read_u32(root + INTERNAL_NODE_HEADER_SIZE +
                               INTERNAL_NODE_CELL_SIZE);
    uint32_t child2 = read_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
    if (child0 != old_full_parent_page_num ||
        child2 != sibling_parent_page_num || child1 == 0u ||
        child1 == child0 || child1 == child2 ||
        child1 >= table->pager->num_pages) {
        return false;
    }

    unsigned char left[PAGE_SIZE], middle[PAGE_SIZE], right[PAGE_SIZE];
    memcpy(left, get_page(table->pager, child0), PAGE_SIZE);
    memcpy(middle, get_page(table->pager, child1), PAGE_SIZE);
    memcpy(right, get_page(table->pager, child2), PAGE_SIZE);
    if (get_node_type(left) != NODE_INTERNAL ||
        get_node_type(middle) != NODE_INTERNAL ||
        get_node_type(right) != NODE_INTERNAL ||
        left[IS_ROOT_OFFSET] != 0u || middle[IS_ROOT_OFFSET] != 0u ||
        read_u32(left + PARENT_POINTER_OFFSET) != schema->root_page_num ||
        read_u32(middle + PARENT_POINTER_OFFSET) != schema->root_page_num ||
        read_u32(right + PARENT_POINTER_OFFSET) != schema->root_page_num ||
        read_u32(left + INTERNAL_NODE_NUM_KEYS_OFFSET) >=
            INTERNAL_NODE_MAX_KEYS ||
        read_u32(middle + INTERNAL_NODE_NUM_KEYS_OFFSET) >=
            INTERNAL_NODE_MAX_KEYS) {
        return false;
    }

    *new_internal_page_num_out = child1;
    return true;
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
    uint32_t full_parent_page_num = INVALID_PAGE_NUM;
    uint32_t sibling_parent_page_num = INVALID_PAGE_NUM;
    uint32_t target_leaf_page_num = INVALID_PAGE_NUM;
    uint32_t candidate_key = 0u;
    uint32_t baseline_rows = 0u;
    if (schema == NULL || schema->row_size != 264u ||
        !seed_height_three_tree(db,
                                schema,
                                &full_parent_page_num,
                                &sibling_parent_page_num,
                                &target_leaf_page_num,
                                &candidate_key,
                                &baseline_rows)) {
        fprintf(stderr, "unable to seed height-three V2 tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t baseline_pages = table->pager->num_pages;
    uint32_t inserted_internal = INVALID_PAGE_NUM;
    if (!exec_ok(db, "BEGIN;") ||
        !insert_candidate(table, schema, candidate_key) ||
        !root_has_inserted_internal(table,
                                    schema,
                                    full_parent_page_num,
                                    sibling_parent_page_num,
                                    &inserted_internal) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "transactional non-root parent split failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    unsigned char root_after_rollback[PAGE_SIZE];
    unsigned char parent_after_rollback[PAGE_SIZE];
    memcpy(root_after_rollback,
           get_page(table->pager, schema->root_page_num),
           PAGE_SIZE);
    memcpy(parent_after_rollback,
           get_page(table->pager, full_parent_page_num),
           PAGE_SIZE);
    if (table->pager->num_pages != baseline_pages ||
        read_u32(root_after_rollback + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        read_u32(parent_after_rollback + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS ||
        record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "non-root parent rollback leaked topology/allocation\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!insert_candidate(table, schema, candidate_key) ||
        !root_has_inserted_internal(table,
                                    schema,
                                    full_parent_page_num,
                                    sibling_parent_page_num,
                                    &inserted_internal) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "committed non-root parent split failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL ||
        !root_has_inserted_internal(table,
                                    schema,
                                    full_parent_page_num,
                                    sibling_parent_page_num,
                                    &inserted_internal) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "non-root parent split did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("NONROOT_PARENT_V2_SPLIT_OK full_parent=%u new_internal=%u target_leaf=%u key=%u root_insert=yes rollback=yes commit=yes reopen=yes integrity=yes wal=yes\n",
           full_parent_page_num,
           inserted_internal,
           target_leaf_page_num,
           candidate_key);
    tinydb_close(db);
    remove(argv[1]);
    return EXIT_SUCCESS;
}
