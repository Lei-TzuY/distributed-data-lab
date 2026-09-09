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

#define LEAF_COUNT 11u
#define LEAF_PARENT_COUNT 5u
#define GRAND_COUNT 2u
#define BASELINE_ROWS 11u

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
    snprintf(values[1].text, sizeof(values[1].text), "local-borrow-%u", id);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 7000u;
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

static bool allocate_pages(Pager* pager,
                           uint32_t grands[GRAND_COUNT],
                           uint32_t parents[LEAF_PARENT_COUNT],
                           uint32_t leaves[LEAF_COUNT]) {
    for (uint32_t i = 0u; i < GRAND_COUNT; i++) {
        grands[i] = get_unused_page_num(pager);
        if (grands[i] == 0u || grands[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, grands[i]);
    }
    for (uint32_t i = 0u; i < LEAF_PARENT_COUNT; i++) {
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

static bool seed_case(TinyDB* db,
                      TableSchema* schema,
                      bool borrow_from_right,
                      uint32_t grands[GRAND_COUNT],
                      uint32_t parents[LEAF_PARENT_COUNT],
                      uint32_t leaves[LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    uint32_t root = schema->root_page_num;
    const uint32_t keys[LEAF_COUNT] = {
        10u,20u,30u,40u,50u,60u,70u,80u,90u,100u,110u
    };
    if (!allocate_pages(pager, grands, parents, leaves)) return false;

    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        uint32_t parent_index;
        if (i < 7u) {
            if (borrow_from_right) {
                parent_index = i < 2u ? 0u : (i < 5u ? 1u : 2u);
            } else {
                parent_index = i < 3u ? 0u : (i < 5u ? 1u : 2u);
            }
        } else {
            parent_index = i < 9u ? 3u : 4u;
        }
        unsigned char* leaf = (unsigned char*)get_page(pager, leaves[i]);
        memset(leaf, 0, PAGE_SIZE);
        if (!tinydb_slotted_leaf_v2_init(leaf, PAGE_SIZE)) return false;
        leaf[IS_ROOT_OFFSET] = 0u;
        write_u32(leaf + PARENT_POINTER_OFFSET, parents[parent_index]);
        tinydb_slotted_split_write_u32(leaf + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
                                       i == 0u ? 0u : leaves[i - 1u]);
        tinydb_slotted_split_write_u32(leaf + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
                                       i + 1u == LEAF_COUNT ? 0u : leaves[i + 1u]);
        if (!raw_insert(schema, leaf, keys[i]) ||
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaves[i]);
    }

    if (borrow_from_right) {
        const uint32_t p0c[2] = {leaves[0], leaves[1]};
        const uint32_t p0k[1] = {10u};
        const uint32_t p1c[3] = {leaves[2], leaves[3], leaves[4]};
        const uint32_t p1k[2] = {30u,40u};
        const uint32_t p2c[2] = {leaves[5], leaves[6]};
        const uint32_t p2k[1] = {60u};
        if (!build_internal(pager, parents[0], grands[0], false, p0c,p0k,2u) ||
            !build_internal(pager, parents[1], grands[0], false, p1c,p1k,3u) ||
            !build_internal(pager, parents[2], grands[0], false, p2c,p2k,2u)) return false;
        const uint32_t g0c[3] = {parents[0],parents[1],parents[2]};
        const uint32_t g0k[2] = {20u,50u};
        if (!build_internal(pager, grands[0], root, false, g0c,g0k,3u)) return false;
    } else {
        const uint32_t p0c[3] = {leaves[0], leaves[1], leaves[2]};
        const uint32_t p0k[2] = {10u,20u};
        const uint32_t p1c[2] = {leaves[3], leaves[4]};
        const uint32_t p1k[1] = {40u};
        const uint32_t p2c[2] = {leaves[5], leaves[6]};
        const uint32_t p2k[1] = {60u};
        if (!build_internal(pager, parents[0], grands[0], false, p0c,p0k,3u) ||
            !build_internal(pager, parents[1], grands[0], false, p1c,p1k,2u) ||
            !build_internal(pager, parents[2], grands[0], false, p2c,p2k,2u)) return false;
        const uint32_t g0c[3] = {parents[0],parents[1],parents[2]};
        const uint32_t g0k[2] = {30u,50u};
        if (!build_internal(pager, grands[0], root, false, g0c,g0k,3u)) return false;
    }

    const uint32_t p3c[2] = {leaves[7], leaves[8]};
    const uint32_t p3k[1] = {80u};
    const uint32_t p4c[2] = {leaves[9], leaves[10]};
    const uint32_t p4k[1] = {100u};
    const uint32_t g1c[2] = {parents[3], parents[4]};
    const uint32_t g1k[1] = {90u};
    const uint32_t rootc[2] = {grands[0], grands[1]};
    const uint32_t rootk[1] = {70u};
    if (!build_internal(pager, parents[3], grands[1], false, p3c,p3k,2u) ||
        !build_internal(pager, parents[4], grands[1], false, p4c,p4k,2u) ||
        !build_internal(pager, grands[1], root, false, g1c,g1k,2u) ||
        !build_internal(pager, root, 0u, true, rootc,rootk,2u)) return false;

    for (uint32_t i = 0u; i < LEAF_PARENT_COUNT; i++) mark_page_dirty(pager, parents[i]);
    for (uint32_t i = 0u; i < GRAND_COUNT; i++) mark_page_dirty(pager, grands[i]);
    mark_page_dirty(pager, root);
    pager_commit(pager);
    return tinydb_record_scan(table, schema, NULL, NULL) == BASELINE_ROWS &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool internal_matches(Table* table, uint32_t page_num,
                             const uint32_t* children,
                             const uint32_t* separators,
                             uint32_t child_count) {
    unsigned char page[PAGE_SIZE];
    memcpy(page, get_page(table->pager, page_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(page, PAGE_SIZE) ||
        tinydb_parent_stage_read_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET) != child_count - 1u)
        return false;
    for (uint32_t i = 0u; i < child_count; i++) {
        if (tinydb_parent_stage_child_at(page, i) != children[i]) return false;
        if (i + 1u < child_count && tinydb_parent_stage_key_at(page, i) != separators[i])
            return false;
    }
    return true;
}

static bool leaf_state(Table* table, uint32_t page_num, uint32_t parent,
                       uint32_t prev_expected, uint32_t next_expected) {
    unsigned char page[PAGE_SIZE];
    uint32_t prev = INVALID_PAGE_NUM, next = INVALID_PAGE_NUM;
    memcpy(page, get_page(table->pager, page_num), PAGE_SIZE);
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           read_u32(page + PARENT_POINTER_OFFSET) == parent &&
           tinydb_leaf_page_prev(page, PAGE_SIZE, &prev) &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next) &&
           prev == prev_expected && next == next_expected;
}

static bool free_has(const Pager* pager, uint32_t page_num) {
    for (uint32_t i = 0u; i < pager->free_page_count; i++)
        if (pager->free_pages[i] == page_num) return true;
    return false;
}

static bool present(Table* table, const TableSchema* schema, uint32_t key) {
    TinyDBRecord record;
    return tinydb_record_find(table, schema, key, &record);
}

static bool delete_key(Table* table, const TableSchema* schema, uint32_t key,
                       char message[TINYDB_RECORD_MESSAGE_MAX]) {
    message[0] = '\0';
    return tinydb_record_delete(table, schema, key, message, TINYDB_RECORD_MESSAGE_MAX);
}

static bool control_state(Table* table, TableSchema* schema,
                          const uint32_t grands[GRAND_COUNT],
                          const uint32_t parents[LEAF_PARENT_COUNT],
                          const uint32_t leaves[LEAF_COUNT]) {
    const uint32_t g1c[2] = {parents[3], parents[4]};
    const uint32_t g1k[1] = {90u};
    const uint32_t p3c[2] = {leaves[7], leaves[8]};
    const uint32_t p3k[1] = {80u};
    const uint32_t p4c[2] = {leaves[9], leaves[10]};
    const uint32_t p4k[1] = {100u};
    const uint32_t rootc[2] = {grands[0], grands[1]};
    const uint32_t rootk[1] = {70u};
    return internal_matches(table, schema->root_page_num, rootc,rootk,2u) &&
           internal_matches(table, grands[1], g1c,g1k,2u) &&
           internal_matches(table, parents[3], p3c,p3k,2u) &&
           internal_matches(table, parents[4], p4c,p4k,2u) &&
           leaf_state(table, leaves[7], parents[3], leaves[6], leaves[8]) &&
           leaf_state(table, leaves[10], parents[4], leaves[9], 0u);
}

static bool borrowed_state(Table* table, TableSchema* schema, bool from_right,
                           const uint32_t grands[GRAND_COUNT],
                           const uint32_t parents[LEAF_PARENT_COUNT],
                           const uint32_t leaves[LEAF_COUNT], uint32_t free_before) {
    if (!control_state(table, schema, grands, parents, leaves)) return false;
    if (from_right) {
        const uint32_t p0c[2] = {leaves[0],leaves[2]}; const uint32_t p0k[1]={10u};
        const uint32_t p1c[2] = {leaves[3],leaves[4]}; const uint32_t p1k[1]={40u};
        const uint32_t p2c[2] = {leaves[5],leaves[6]}; const uint32_t p2k[1]={60u};
        const uint32_t g0c[3] = {parents[0],parents[1],parents[2]};
        const uint32_t g0k[2] = {30u,50u};
        return internal_matches(table, parents[0],p0c,p0k,2u) &&
               internal_matches(table, parents[1],p1c,p1k,2u) &&
               internal_matches(table, parents[2],p2c,p2k,2u) &&
               internal_matches(table, grands[0],g0c,g0k,3u) &&
               leaf_state(table, leaves[0],parents[0],0u,leaves[2]) &&
               leaf_state(table, leaves[2],parents[0],leaves[0],leaves[3]) &&
               leaf_state(table, leaves[3],parents[1],leaves[2],leaves[4]) &&
               table->pager->free_page_count == free_before + 1u &&
               free_has(table->pager, leaves[1]) && !present(table,schema,20u);
    }
    const uint32_t p0c[2] = {leaves[0],leaves[1]}; const uint32_t p0k[1]={10u};
    const uint32_t p1c[2] = {leaves[2],leaves[4]}; const uint32_t p1k[1]={30u};
    const uint32_t p2c[2] = {leaves[5],leaves[6]}; const uint32_t p2k[1]={60u};
    const uint32_t g0c[3] = {parents[0],parents[1],parents[2]};
    const uint32_t g0k[2] = {20u,50u};
    return internal_matches(table, parents[0],p0c,p0k,2u) &&
           internal_matches(table, parents[1],p1c,p1k,2u) &&
           internal_matches(table, parents[2],p2c,p2k,2u) &&
           internal_matches(table, grands[0],g0c,g0k,3u) &&
           leaf_state(table, leaves[1],parents[0],leaves[0],leaves[2]) &&
           leaf_state(table, leaves[2],parents[1],leaves[1],leaves[4]) &&
           leaf_state(table, leaves[4],parents[1],leaves[2],leaves[5]) &&
           table->pager->free_page_count == free_before + 1u &&
           free_has(table->pager, leaves[3]) && !present(table,schema,40u);
}

static bool original_data_state(Table* table, TableSchema* schema, bool from_right,
                                const uint32_t grands[GRAND_COUNT],
                                const uint32_t parents[LEAF_PARENT_COUNT],
                                const uint32_t leaves[LEAF_COUNT], uint32_t free_before) {
    if (!control_state(table, schema, grands, parents, leaves) ||
        table->pager->free_page_count != free_before) return false;
    if (from_right) {
        const uint32_t p0c[2]={leaves[0],leaves[1]}; const uint32_t p0k[1]={10u};
        const uint32_t p1c[3]={leaves[2],leaves[3],leaves[4]}; const uint32_t p1k[2]={30u,40u};
        const uint32_t p2c[2]={leaves[5],leaves[6]}; const uint32_t p2k[1]={60u};
        const uint32_t g0c[3]={parents[0],parents[1],parents[2]}; const uint32_t g0k[2]={20u,50u};
        return internal_matches(table,parents[0],p0c,p0k,2u) &&
               internal_matches(table,parents[1],p1c,p1k,3u) &&
               internal_matches(table,parents[2],p2c,p2k,2u) &&
               internal_matches(table,grands[0],g0c,g0k,3u) && present(table,schema,20u);
    }
    const uint32_t p0c[3]={leaves[0],leaves[1],leaves[2]}; const uint32_t p0k[2]={10u,20u};
    const uint32_t p1c[2]={leaves[3],leaves[4]}; const uint32_t p1k[1]={40u};
    const uint32_t p2c[2]={leaves[5],leaves[6]}; const uint32_t p2k[1]={60u};
    const uint32_t g0c[3]={parents[0],parents[1],parents[2]}; const uint32_t g0k[2]={30u,50u};
    return internal_matches(table,parents[0],p0c,p0k,3u) &&
           internal_matches(table,parents[1],p1c,p1k,2u) &&
           internal_matches(table,parents[2],p2c,p2k,2u) &&
           internal_matches(table,grands[0],g0c,g0k,3u) && present(table,schema,40u);
}

static bool run_case(const char* path, bool from_right) {
    remove(path);
    TinyDB* db = tinydb_open(path);
    if (db == NULL || !exec_ok(db,"CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return false;
    }
    Table* table=tinydb_table(db); TableSchema* schema=find_schema(table,"items");
    uint32_t grands[GRAND_COUNT]={0u,0u};
    uint32_t parents[LEAF_PARENT_COUNT]={0u,0u,0u,0u,0u};
    uint32_t leaves[LEAF_COUNT]={0u};
    if (schema==NULL || schema->row_size!=264u || !seed_case(db,schema,from_right,grands,parents,leaves)) {
        tinydb_close(db); return false;
    }
    uint32_t free_before=table->pager->free_page_count;
    uint32_t removed_key=from_right?20u:40u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!exec_ok(db,"BEGIN;") || !delete_key(table,schema,removed_key,message) ||
        !borrowed_state(table,schema,from_right,grands,parents,leaves,free_before) ||
        tinydb_record_scan(table,schema,NULL,NULL)!=10u || !exec_ok(db,"PRAGMA integrity_check;") ||
        !exec_ok(db,"ROLLBACK;") ||
        !original_data_state(table,schema,from_right,grands,parents,leaves,free_before) ||
        tinydb_record_scan(table,schema,NULL,NULL)!=BASELINE_ROWS || !exec_ok(db,"PRAGMA integrity_check;")) {
        fprintf(stderr,"local borrow rollback failed for %u: %s\n",removed_key,message);
        tinydb_close(db); return false;
    }
    if (!delete_key(table,schema,removed_key,message) ||
        !borrowed_state(table,schema,from_right,grands,parents,leaves,free_before) ||
        !exec_ok(db,"PRAGMA integrity_check;")) {
        fprintf(stderr,"local borrow commit failed for %u: %s\n",removed_key,message);
        tinydb_close(db); return false;
    }
    tinydb_close(db); db=tinydb_open(path); if (db==NULL) return false;
    table=tinydb_table(db); schema=find_schema(table,"items");
    bool ok=schema!=NULL && borrowed_state(table,schema,from_right,grands,parents,leaves,free_before) &&
            tinydb_record_scan(table,schema,NULL,NULL)==10u && exec_ok(db,"PRAGMA integrity_check;");
    tinydb_close(db); return ok;
}

int main(int argc, char** argv) {
    if (argc!=2) return EXIT_FAILURE;
    char right_path[1024],left_path[1024];
    if (snprintf(right_path,sizeof(right_path),"%s.right",argv[1])<0 ||
        snprintf(left_path,sizeof(left_path),"%s.left",argv[1])<0) return EXIT_FAILURE;
    if (!run_case(right_path,true) || !run_case(left_path,false)) return EXIT_FAILURE;
    printf("V2_INTERNAL_BORROW_LOCAL_OK right=yes left=yes height4=yes grandparent=yes "
           "ancestor_stable=yes control_subtree=yes rollback=yes allocator=yes "
           "reopen=yes integrity=yes wal=yes\n");
    return EXIT_SUCCESS;
}