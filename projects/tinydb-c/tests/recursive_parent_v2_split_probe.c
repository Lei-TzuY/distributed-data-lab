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
#define TARGET_EXTRA_INTERNAL_COUNT INTERNAL_NODE_MAX_KEYS
#define SIBLING_INTERNAL_COUNT 2u
#define MINIMAL_INTERNAL_COUNT \
    (TARGET_EXTRA_INTERNAL_COUNT + SIBLING_INTERNAL_COUNT)
#define TARGET_GRAND_LEAF_COUNT \
    (FULL_CHILD_COUNT + TARGET_EXTRA_INTERNAL_COUNT * 2u)
#define TOTAL_LEAF_COUNT \
    (TARGET_GRAND_LEAF_COUNT + SIBLING_INTERNAL_COUNT * 2u)
#define RANGE_STEP 100000u
#define TARGET_LEAF_INDEX (INTERNAL_NODE_MAX_KEYS / 2u)
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

static void set_internal_cell(unsigned char* page,
                              uint32_t index,
                              uint32_t child_page_num,
                              uint32_t max_key) {
    unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE +
                          index * INTERNAL_NODE_CELL_SIZE;
    write_u32(cell, child_page_num);
    write_u32(cell + INTERNAL_NODE_CHILD_SIZE, max_key);
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

static uint32_t leaf_max_for_index(uint32_t leaf_index) {
    return (leaf_index + 1u) * RANGE_STEP;
}

static bool seed_height_four_tree(TinyDB* db,
                                  TableSchema* schema,
                                  uint32_t* full_parent_page_num,
                                  uint32_t* full_grandparent_page_num,
                                  uint32_t* sibling_grandparent_page_num,
                                  uint32_t* target_leaf_page_num,
                                  uint32_t* candidate_key,
                                  uint32_t* baseline_rows) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root_page_num = schema->root_page_num;

    *full_parent_page_num = get_unused_page_num(pager);
    (void)get_page(pager, *full_parent_page_num);
    *full_grandparent_page_num = get_unused_page_num(pager);
    (void)get_page(pager, *full_grandparent_page_num);
    *sibling_grandparent_page_num = get_unused_page_num(pager);
    (void)get_page(pager, *sibling_grandparent_page_num);
    if (*full_parent_page_num == 0u ||
        *full_grandparent_page_num == 0u ||
        *sibling_grandparent_page_num == 0u) {
        return false;
    }

    uint32_t minimal_internal_pages[MINIMAL_INTERNAL_COUNT];
    for (uint32_t i = 0u; i < MINIMAL_INTERNAL_COUNT; i++) {
        minimal_internal_pages[i] = get_unused_page_num(pager);
        if (minimal_internal_pages[i] == 0u ||
            minimal_internal_pages[i] == INVALID_PAGE_NUM) {
            return false;
        }
        (void)get_page(pager, minimal_internal_pages[i]);
    }

    uint32_t leaves[TOTAL_LEAF_COUNT];
    for (uint32_t i = 0u; i < TOTAL_LEAF_COUNT; i++) {
        leaves[i] = get_unused_page_num(pager);
        if (leaves[i] == 0u || leaves[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaves[i]);
    }

    char long_text[TINYDB_RECORD_TEXT_MAX + 1];
    make_long_text(long_text, 'a');
    unsigned char candidate_envelope[PAGE_SIZE];
    uint32_t candidate_length = 0u;
    uint32_t previous_max = TARGET_LEAF_INDEX * RANGE_STEP;
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

    for (uint32_t i = 0u; i < TOTAL_LEAF_COUNT; i++) {
        uint32_t parent_page_num = 0u;
        if (i < FULL_CHILD_COUNT) {
            parent_page_num = *full_parent_page_num;
        } else if (i < TARGET_GRAND_LEAF_COUNT) {
            uint32_t offset = i - FULL_CHILD_COUNT;
            parent_page_num = minimal_internal_pages[offset / 2u];
        } else {
            uint32_t offset = i - TARGET_GRAND_LEAF_COUNT;
            parent_page_num =
                minimal_internal_pages[TARGET_EXTRA_INTERNAL_COUNT +
                                       offset / 2u];
        }
        uint32_t prev_page_num = i == 0u ? 0u : leaves[i - 1u];
        uint32_t next_page_num = i + 1u == TOTAL_LEAF_COUNT
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
        uint32_t max_key = leaf_max_for_index(i);
        if (i == TARGET_LEAF_INDEX) {
            if (!raw_insert(schema, leaf, max_key, long_text)) return false;
            uint32_t filler_key = previous_max + 1000u;
            while (tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) >=
                   candidate_required) {
                if (filler_key >= max_key) return false;
                if (!raw_insert(schema, leaf, filler_key, long_text)) return false;
                filler_key += 1000u;
            }
            if (tinydb_slotted_leaf_v2_count(leaf, PAGE_SIZE) < 2u ||
                tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) >=
                    candidate_required) {
                return false;
            }
            *target_leaf_page_num = leaves[i];
        } else if (!raw_insert(schema, leaf, max_key, "s")) {
            return false;
        }
        if (!tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaves[i]);
    }

    unsigned char* full_parent =
        (unsigned char*)get_page(pager, *full_parent_page_num);
    initialize_internal(full_parent,
                        *full_grandparent_page_num,
                        false,
                        INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        set_internal_cell(full_parent,
                          i,
                          leaves[i],
                          leaf_max_for_index(i));
    }
    write_u32(full_parent + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              leaves[INTERNAL_NODE_MAX_KEYS]);
    mark_page_dirty(pager, *full_parent_page_num);

    for (uint32_t i = 0u; i < TARGET_EXTRA_INTERNAL_COUNT; i++) {
        uint32_t leaf_start = FULL_CHILD_COUNT + i * 2u;
        unsigned char* node =
            (unsigned char*)get_page(pager, minimal_internal_pages[i]);
        initialize_internal(node,
                            *full_grandparent_page_num,
                            false,
                            1u);
        set_internal_cell(node,
                          0u,
                          leaves[leaf_start],
                          leaf_max_for_index(leaf_start));
        write_u32(node + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
                  leaves[leaf_start + 1u]);
        mark_page_dirty(pager, minimal_internal_pages[i]);
    }

    for (uint32_t i = 0u; i < SIBLING_INTERNAL_COUNT; i++) {
        uint32_t internal_index = TARGET_EXTRA_INTERNAL_COUNT + i;
        uint32_t leaf_start = TARGET_GRAND_LEAF_COUNT + i * 2u;
        unsigned char* node = (unsigned char*)get_page(
            pager, minimal_internal_pages[internal_index]);
        initialize_internal(node,
                            *sibling_grandparent_page_num,
                            false,
                            1u);
        set_internal_cell(node,
                          0u,
                          leaves[leaf_start],
                          leaf_max_for_index(leaf_start));
        write_u32(node + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
                  leaves[leaf_start + 1u]);
        mark_page_dirty(pager, minimal_internal_pages[internal_index]);
    }

    unsigned char* full_grandparent =
        (unsigned char*)get_page(pager, *full_grandparent_page_num);
    initialize_internal(full_grandparent,
                        root_page_num,
                        false,
                        INTERNAL_NODE_MAX_KEYS);
    set_internal_cell(full_grandparent,
                      0u,
                      *full_parent_page_num,
                      leaf_max_for_index(FULL_CHILD_COUNT - 1u));
    for (uint32_t child_index = 1u;
         child_index < INTERNAL_NODE_MAX_KEYS;
         child_index++) {
        uint32_t minimal_index = child_index - 1u;
        uint32_t last_leaf_index = FULL_CHILD_COUNT +
                                   minimal_index * 2u + 1u;
        set_internal_cell(full_grandparent,
                          child_index,
                          minimal_internal_pages[minimal_index],
                          leaf_max_for_index(last_leaf_index));
    }
    write_u32(full_grandparent + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              minimal_internal_pages[TARGET_EXTRA_INTERNAL_COUNT - 1u]);
    mark_page_dirty(pager, *full_grandparent_page_num);

    unsigned char* sibling_grandparent =
        (unsigned char*)get_page(pager, *sibling_grandparent_page_num);
    initialize_internal(sibling_grandparent,
                        root_page_num,
                        false,
                        1u);
    uint32_t sibling_first_last_leaf = TARGET_GRAND_LEAF_COUNT + 1u;
    set_internal_cell(
        sibling_grandparent,
        0u,
        minimal_internal_pages[TARGET_EXTRA_INTERNAL_COUNT],
        leaf_max_for_index(sibling_first_last_leaf));
    write_u32(
        sibling_grandparent + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
        minimal_internal_pages[TARGET_EXTRA_INTERNAL_COUNT + 1u]);
    mark_page_dirty(pager, *sibling_grandparent_page_num);

    unsigned char* root =
        (unsigned char*)get_page(pager, root_page_num);
    initialize_internal(root, 0u, true, 1u);
    set_internal_cell(root,
                      0u,
                      *full_grandparent_page_num,
                      leaf_max_for_index(TARGET_GRAND_LEAF_COUNT - 1u));
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              *sibling_grandparent_page_num);
    mark_page_dirty(pager, root_page_num);

    pager_commit(pager);
    *baseline_rows = tinydb_record_scan(table, schema, NULL, NULL);
    return *target_leaf_page_num != INVALID_PAGE_NUM &&
           *baseline_rows >= TOTAL_LEAF_COUNT &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool root_has_recursive_split(Table* table,
                                     const TableSchema* schema,
                                     uint32_t old_grandparent_page_num,
                                     uint32_t sibling_grandparent_page_num,
                                     uint32_t* new_grandparent_page_num_out) {
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
    if (child0 != old_grandparent_page_num ||
        child2 != sibling_grandparent_page_num || child1 == 0u ||
        child1 == child0 || child1 == child2 ||
        child1 >= table->pager->num_pages) {
        return false;
    }

    unsigned char left[PAGE_SIZE];
    unsigned char middle[PAGE_SIZE];
    memcpy(left, get_page(table->pager, child0), PAGE_SIZE);
    memcpy(middle, get_page(table->pager, child1), PAGE_SIZE);
    if (get_node_type(left) != NODE_INTERNAL ||
        get_node_type(middle) != NODE_INTERNAL ||
        left[IS_ROOT_OFFSET] != 0u || middle[IS_ROOT_OFFSET] != 0u ||
        read_u32(left + PARENT_POINTER_OFFSET) != schema->root_page_num ||
        read_u32(middle + PARENT_POINTER_OFFSET) != schema->root_page_num ||
        read_u32(left + INTERNAL_NODE_NUM_KEYS_OFFSET) >=
            INTERNAL_NODE_MAX_KEYS ||
        read_u32(middle + INTERNAL_NODE_NUM_KEYS_OFFSET) >=
            INTERNAL_NODE_MAX_KEYS) {
        return false;
    }

    *new_grandparent_page_num_out = child1;
    return true;
}

static bool original_topology_restored(Table* table,
                                       const TableSchema* schema,
                                       uint32_t full_parent_page_num,
                                       uint32_t full_grandparent_page_num,
                                       uint32_t sibling_grandparent_page_num) {
    unsigned char root[PAGE_SIZE];
    unsigned char parent[PAGE_SIZE];
    unsigned char grandparent[PAGE_SIZE];
    memcpy(root,
           get_page(table->pager, schema->root_page_num),
           PAGE_SIZE);
    memcpy(parent,
           get_page(table->pager, full_parent_page_num),
           PAGE_SIZE);
    memcpy(grandparent,
           get_page(table->pager, full_grandparent_page_num),
           PAGE_SIZE);
    return get_node_type(root) == NODE_INTERNAL && root[IS_ROOT_OFFSET] != 0u &&
           read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) == 1u &&
           read_u32(root + INTERNAL_NODE_HEADER_SIZE) ==
               full_grandparent_page_num &&
           read_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET) ==
               sibling_grandparent_page_num &&
           read_u32(parent + INTERNAL_NODE_NUM_KEYS_OFFSET) ==
               INTERNAL_NODE_MAX_KEYS &&
           read_u32(grandparent + INTERNAL_NODE_NUM_KEYS_OFFSET) ==
               INTERNAL_NODE_MAX_KEYS;
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

static bool wal_exists(const Pager* pager) {
    FILE* file = fopen(pager->wal_filename, "rb");
    if (file == NULL) return false;
    fclose(file);
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
    uint32_t full_grandparent_page_num = INVALID_PAGE_NUM;
    uint32_t sibling_grandparent_page_num = INVALID_PAGE_NUM;
    uint32_t target_leaf_page_num = INVALID_PAGE_NUM;
    uint32_t candidate_key = 0u;
    uint32_t baseline_rows = 0u;
    if (schema == NULL || schema->row_size != 264u ||
        !seed_height_four_tree(db,
                               schema,
                               &full_parent_page_num,
                               &full_grandparent_page_num,
                               &sibling_grandparent_page_num,
                               &target_leaf_page_num,
                               &candidate_key,
                               &baseline_rows)) {
        fprintf(stderr, "unable to seed height-four V2 tree\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    uint32_t baseline_pages = table->pager->num_pages;
    uint32_t new_grandparent = INVALID_PAGE_NUM;
    if (!exec_ok(db, "BEGIN;") ||
        !insert_candidate(table, schema, candidate_key) ||
        !root_has_recursive_split(table,
                                  schema,
                                  full_grandparent_page_num,
                                  sibling_grandparent_page_num,
                                  &new_grandparent) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        table->pager->num_pages != baseline_pages + 3u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;")) {
        fprintf(stderr, "transactional recursive split failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (table->pager->num_pages != baseline_pages ||
        !original_topology_restored(table,
                                    schema,
                                    full_parent_page_num,
                                    full_grandparent_page_num,
                                    sibling_grandparent_page_num) ||
        record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "recursive rollback did not restore topology\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (!insert_candidate(table, schema, candidate_key) ||
        !root_has_recursive_split(table,
                                  schema,
                                  full_grandparent_page_num,
                                  sibling_grandparent_page_num,
                                  &new_grandparent) ||
        table->pager->num_pages != baseline_pages + 3u ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !wal_exists(table->pager)) {
        fprintf(stderr, "recursive autocommit split failed\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL ||
        !root_has_recursive_split(table,
                                  schema,
                                  full_grandparent_page_num,
                                  sibling_grandparent_page_num,
                                  &new_grandparent) ||
        !record_present(table, schema, candidate_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != baseline_rows + 1u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "recursive split did not survive reopen\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    tinydb_close(db);
    printf("RECURSIVE_PARENT_V2_SPLIT_OK cascade=yes rollback=yes commit=yes "
           "reopen=yes integrity=yes wal=yes pages=3\n");
    return EXIT_SUCCESS;
}
