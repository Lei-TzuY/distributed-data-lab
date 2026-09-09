#include "engine.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_migration.h"
#include "leaf_mutation_policy.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool exec_ok(TinyDB* db, const char* sql) {
    TinyDBSqlResult result;
    TinyDBSqlStatus status = tinydb_execute_sql(db, sql, &result);
    if (status != TINYDB_SQL_SUCCESS) {
        fprintf(stderr, "SQL failed: %s (%s)\n", sql, result.message);
        return false;
    }
    return true;
}

static TinyDBValue int_value(uint32_t value) {
    TinyDBValue result;
    memset(&result, 0, sizeof(result));
    result.type = COL_TYPE_INT;
    result.int_value = value;
    return result;
}

static bool migrate_leaf(Table* table,
                         uint32_t page_num,
                         uint32_t logical_value_length) {
    void* page = get_page(table->pager, page_num);
    if (!tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE)) return false;

    unsigned char scratch[PAGE_SIZE];
    memset(scratch, 0, sizeof(scratch));
    if (!tinydb_leaf_migrate_v1_to_v2(page,
                                      PAGE_SIZE,
                                      logical_value_length,
                                      scratch,
                                      sizeof(scratch))) {
        return false;
    }

    memcpy(page, scratch, PAGE_USABLE_SIZE);
    mark_page_dirty(table->pager, page_num);
    return tinydb_leaf_format_detect_page(page, PAGE_SIZE) ==
           TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2;
}

static bool expect_row(Table* table,
                       TableSchema* schema,
                       uint32_t id,
                       uint32_t bucket,
                       uint32_t score) {
    TinyDBRecord record;
    if (!tinydb_record_find(table, schema, id, &record)) {
        fprintf(stderr, "record %u was not found\n", id);
        return false;
    }

    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_decode(schema,
                              &record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &count,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "record %u decode failed: %s\n", id, message);
        return false;
    }
    return count == 3u &&
           values[0].int_value == id &&
           values[1].int_value == bucket &&
           values[2].int_value == score;
}

static uint32_t forward_count(Table* table, const TableSchema* schema) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_start(table);
    uint32_t count = 0u;
    if (cursor != NULL) {
        while (!cursor->end_of_table) {
            count++;
            tinydb_leaf_read_advance(cursor);
        }
    }
    free(cursor);
    table->root_page_num = previous_root;
    return count;
}

static uint32_t backward_count(Table* table, const TableSchema* schema) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_end(table);
    uint32_t count = 0u;
    if (cursor != NULL) {
        while (!cursor->end_of_table) {
            count++;
            tinydb_leaf_read_retreat(cursor);
        }
    }
    free(cursor);
    table->root_page_num = previous_root;
    return count;
}

static bool verify_reads(Table* table, TableSchema* schema) {
    if (!expect_row(table, schema, 1u, 1u, 10u) ||
        !expect_row(table, schema, 20u, 0u, 200u) ||
        !expect_row(table, schema, 40u, 0u, 400u)) {
        return false;
    }
    if (tinydb_record_scan(table, schema, NULL, NULL) != 40u) {
        fprintf(stderr, "mixed-format generic scan did not return 40 rows\n");
        return false;
    }
    if (forward_count(table, schema) != 40u ||
        backward_count(table, schema) != 40u) {
        fprintf(stderr, "mixed-format cursor traversal count mismatch\n");
        return false;
    }
    return true;
}

static bool migrate_edge_leaves(Table* table, TableSchema* schema) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;

    Cursor* first = table_find(table, 1u);
    Cursor* last = table_find(table, 40u);
    if (first == NULL || last == NULL || first->page_num == last->page_num) {
        fprintf(stderr, "test table did not split into distinct edge leaves\n");
        free(first);
        free(last);
        table->root_page_num = previous_root;
        return false;
    }

    uint32_t first_page = first->page_num;
    uint32_t last_page = last->page_num;
    free(first);
    free(last);

    bool ok = migrate_leaf(table, first_page, schema->row_size) &&
              migrate_leaf(table, last_page, schema->row_size);
    table->root_page_num = previous_root;
    return ok;
}

static bool locate_v2_key(Table* table,
                          TableSchema* schema,
                          uint32_t key,
                          uint32_t* page_num,
                          uint32_t* cell_num,
                          const void** value,
                          uint32_t* value_length) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    if (cursor == NULL) {
        table->root_page_num = previous_root;
        return false;
    }
    void* page = get_page(table->pager, cursor->page_num);
    bool ok = tinydb_leaf_format_detect_page(page, PAGE_SIZE) ==
                  TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
              tinydb_leaf_page_key_at(page,
                                      PAGE_SIZE,
                                      cursor->cell_num,
                                      page_num) &&
              *page_num == key &&
              tinydb_leaf_page_value_at(page,
                                        PAGE_SIZE,
                                        cursor->cell_num,
                                        value,
                                        value_length);
    if (ok) {
        *page_num = cursor->page_num;
        *cell_num = cursor->cell_num;
    }
    free(cursor);
    table->root_page_num = previous_root;
    return ok;
}

static bool install_v2_envelope(Table* table, TableSchema* schema) {
    uint32_t page_num = 0u;
    uint32_t cell_num = 0u;
    const void* value = NULL;
    uint32_t value_length = 0u;
    if (!locate_v2_key(table,
                       schema,
                       1u,
                       &page_num,
                       &cell_num,
                       &value,
                       &value_length) ||
        value_length != schema->row_size) {
        fprintf(stderr, "unable to locate raw V2 row for envelope installation\n");
        return false;
    }

    TinyDBRecordPayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.length = schema->row_size;
    memcpy(payload.bytes, value, payload.length);

    unsigned char envelope[ROW_SIZE + TINYDB_ROW_ENVELOPE_HEADER_SIZE];
    uint32_t envelope_length = 0u;
    if (!tinydb_row_envelope_encode(schema,
                                    &payload,
                                    envelope,
                                    sizeof(envelope),
                                    &envelope_length)) {
        fprintf(stderr, "unable to encode V2 row envelope\n");
        return false;
    }

    void* page = get_page(table->pager, page_num);
    if (!tinydb_slotted_leaf_v2_update(page,
                                       PAGE_SIZE,
                                       1u,
                                       envelope,
                                       (uint16_t)envelope_length)) {
        fprintf(stderr, "unable to install V2 row envelope\n");
        return false;
    }
    mark_page_dirty(table->pager, page_num);
    pager_commit(table->pager);

    return expect_row(table, schema, 1u, 1u, 10u) &&
           tinydb_record_scan(table, schema, NULL, NULL) == 40u;
}

static bool expect_envelope_corruption_rejected(Table* table,
                                                 TableSchema* schema) {
    uint32_t page_num = 0u;
    uint32_t cell_num = 0u;
    const void* value = NULL;
    uint32_t value_length = 0u;
    if (!locate_v2_key(table,
                       schema,
                       1u,
                       &page_num,
                       &cell_num,
                       &value,
                       &value_length) ||
        value_length <= schema->row_size ||
        value_length > ROW_SIZE + TINYDB_ROW_ENVELOPE_HEADER_SIZE) {
        fprintf(stderr, "persisted V2 envelope was not found after reopen\n");
        return false;
    }

    unsigned char valid[ROW_SIZE + TINYDB_ROW_ENVELOPE_HEADER_SIZE];
    memcpy(valid, value, value_length);
    TinyDBRecordPayload payload;
    if (!tinydb_row_envelope_decode(schema, valid, value_length, &payload)) {
        fprintf(stderr, "persisted V2 envelope did not decode\n");
        return false;
    }

    void* page = get_page(table->pager, page_num);
    unsigned char corrupt[ROW_SIZE + TINYDB_ROW_ENVELOPE_HEADER_SIZE];
    memcpy(corrupt, valid, value_length);
    corrupt[TINYDB_ROW_ENVELOPE_VERSION_OFFSET] ^= 0x7fu;
    if (!tinydb_slotted_leaf_v2_update(page,
                                       PAGE_SIZE,
                                       1u,
                                       corrupt,
                                       (uint16_t)value_length)) {
        return false;
    }
    mark_page_dirty(table->pager, page_num);
    TinyDBRecord record;
    if (tinydb_record_find(table, schema, 1u, &record) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 0u) {
        fprintf(stderr, "bad row-envelope version was not rejected\n");
        return false;
    }

    page = get_page(table->pager, page_num);
    memcpy(corrupt, valid, value_length);
    corrupt[TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET] ^= 0x55u;
    if (!tinydb_slotted_leaf_v2_update(page,
                                       PAGE_SIZE,
                                       1u,
                                       corrupt,
                                       (uint16_t)value_length)) {
        return false;
    }
    mark_page_dirty(table->pager, page_num);
    if (tinydb_record_find(table, schema, 1u, &record) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 0u) {
        fprintf(stderr, "row-envelope schema mismatch was not rejected\n");
        return false;
    }

    page = get_page(table->pager, page_num);
    memcpy(corrupt, valid, value_length);
    corrupt[TINYDB_ROW_ENVELOPE_LOGICAL_LENGTH_OFFSET] ^= 0x01u;
    if (!tinydb_slotted_leaf_v2_update(page,
                                       PAGE_SIZE,
                                       1u,
                                       corrupt,
                                       (uint16_t)value_length)) {
        return false;
    }
    mark_page_dirty(table->pager, page_num);
    if (tinydb_record_find(table, schema, 1u, &record) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 0u) {
        fprintf(stderr, "row-envelope logical-length mismatch was not rejected\n");
        return false;
    }

    page = get_page(table->pager, page_num);
    if (!tinydb_slotted_leaf_v2_update(page,
                                       PAGE_SIZE,
                                       1u,
                                       payload.bytes,
                                       (uint16_t)payload.length)) {
        fprintf(stderr, "unable to restore raw V2 row after envelope corruption tests\n");
        return false;
    }
    mark_page_dirty(table->pager, page_num);
    return expect_row(table, schema, 1u, 1u, 10u) &&
           tinydb_record_scan(table, schema, NULL, NULL) == 40u;
}

static bool expect_v2_length_mismatch_rejected(Table* table,
                                                TableSchema* schema) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, 1u);
    if (cursor == NULL) {
        table->root_page_num = previous_root;
        return false;
    }

    uint32_t page_num = cursor->page_num;
    uint32_t cell_num = cursor->cell_num;
    free(cursor);
    void* page = get_page(table->pager, page_num);
    if (tinydb_leaf_format_detect_page(page, PAGE_SIZE) !=
        TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        table->root_page_num = previous_root;
        fprintf(stderr, "expected first leaf to be slotted V2\n");
        return false;
    }

    uint32_t key = 0u;
    const void* value = NULL;
    uint32_t value_length = 0u;
    if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, cell_num, &key) ||
        key != 1u ||
        !tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cell_num,
                                   &value,
                                   &value_length) ||
        value_length != schema->row_size ||
        value_length + 1u > ROW_SIZE) {
        table->root_page_num = previous_root;
        fprintf(stderr, "unable to capture canonical V2 payload\n");
        return false;
    }

    unsigned char canonical[ROW_SIZE];
    unsigned char oversized[ROW_SIZE];
    memset(canonical, 0, sizeof(canonical));
    memset(oversized, 0, sizeof(oversized));
    memcpy(canonical, value, value_length);
    memcpy(oversized, value, value_length);
    oversized[value_length] = 0xA5u;

    if (!tinydb_slotted_leaf_v2_update(page,
                                       PAGE_SIZE,
                                       key,
                                       oversized,
                                       (uint16_t)(value_length + 1u))) {
        table->root_page_num = previous_root;
        fprintf(stderr, "unable to create valid oversized V2 payload\n");
        return false;
    }
    mark_page_dirty(table->pager, page_num);
    table->root_page_num = previous_root;

    TinyDBRecord record;
    bool rejected = !tinydb_record_find(table, schema, key, &record) &&
                    tinydb_record_scan(table, schema, NULL, NULL) == 0u;

    page = get_page(table->pager, page_num);
    bool restored = tinydb_slotted_leaf_v2_update(page,
                                                  PAGE_SIZE,
                                                  key,
                                                  canonical,
                                                  (uint16_t)value_length);
    if (restored) mark_page_dirty(table->pager, page_num);

    if (!rejected || !restored) {
        fprintf(stderr,
                "V2 schema-length mismatch was not fail-closed or restore failed\n");
        return false;
    }
    return expect_row(table, schema, 1u, 1u, 10u) &&
           tinydb_record_scan(table, schema, NULL, NULL) == 40u;
}

static bool expect_out_of_range_sibling_rejected(Table* table,
                                                  TableSchema* schema) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* first = tinydb_leaf_read_start(table);
    if (first == NULL) {
        table->root_page_num = previous_root;
        return false;
    }

    void* first_page = get_page(table->pager, first->page_num);
    uint32_t middle_page_num = 0u;
    bool located = tinydb_leaf_page_next(first_page,
                                         PAGE_SIZE,
                                         &middle_page_num) &&
                   middle_page_num != 0u &&
                   middle_page_num < table->pager->num_pages;
    free(first);
    if (!located) {
        table->root_page_num = previous_root;
        fprintf(stderr, "unable to locate middle leaf for corruption probe\n");
        return false;
    }

    void* middle_page = get_page(table->pager, middle_page_num);
    if (!tinydb_leaf_page_is_fixed_v1(middle_page, PAGE_SIZE)) {
        table->root_page_num = previous_root;
        fprintf(stderr, "expected middle leaf to remain fixed V1\n");
        return false;
    }

    uint32_t original_next = 0u;
    memcpy(&original_next,
           (unsigned char*)middle_page + LEAF_NODE_NEXT_LEAF_OFFSET,
           sizeof(original_next));
    if (original_next == 0u) {
        table->root_page_num = previous_root;
        fprintf(stderr, "middle leaf unexpectedly has no successor\n");
        return false;
    }

    uint32_t pages_before = table->pager->num_pages;
    uint32_t corrupt_next = pages_before + 17u;
    memcpy((unsigned char*)middle_page + LEAF_NODE_NEXT_LEAF_OFFSET,
           &corrupt_next,
           sizeof(corrupt_next));
    mark_page_dirty(table->pager, middle_page_num);
    table->root_page_num = previous_root;

    uint32_t corrupt_scan = tinydb_record_scan(table, schema, NULL, NULL);
    bool rejected = corrupt_scan == 0u && table->pager->num_pages == pages_before;

    middle_page = get_page(table->pager, middle_page_num);
    memcpy((unsigned char*)middle_page + LEAF_NODE_NEXT_LEAF_OFFSET,
           &original_next,
           sizeof(original_next));
    mark_page_dirty(table->pager, middle_page_num);

    if (!rejected) {
        fprintf(stderr,
                "corrupt sibling scan was not fail-closed (rows=%u pages=%u/%u)\n",
                corrupt_scan,
                table->pager->num_pages,
                pages_before);
        return false;
    }
    return tinydb_record_scan(table, schema, NULL, NULL) == 40u;
}

static bool expect_mutation_rejected(Table* table, TableSchema* schema) {
    char policy_message[TINYDB_RECORD_MESSAGE_MAX];
    if (tinydb_leaf_tree_mutation_supported(table,
                                            schema->root_page_num,
                                            policy_message,
                                            sizeof(policy_message))) {
        fprintf(stderr, "shared mutation policy unexpectedly allows mixed V1/V2 tree\n");
        return false;
    }
    if (strstr(policy_message, "read-only") == NULL) {
        fprintf(stderr, "unexpected policy rejection: %s\n", policy_message);
        return false;
    }

    TinyDBValue values[3];
    values[0] = int_value(1u);
    values[1] = int_value(99u);
    values[2] = int_value(999u);
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (tinydb_record_update(table,
                             schema,
                             1u,
                             values,
                             3u,
                             message,
                             sizeof(message))) {
        fprintf(stderr, "mutation unexpectedly wrote a mixed V1/V2 tree\n");
        return false;
    }
    if (strstr(message, "read-only") == NULL &&
        strstr(message, "logical leaf value") == NULL &&
        strstr(message, "primary key not found") == NULL) {
        fprintf(stderr, "unexpected mutation rejection: %s\n", message);
        return false;
    }
    return expect_row(table, schema, 1u, 1u, 10u);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: dual_leaf_read_probe <db-path>\n");
        return 2;
    }

    TinyDB* db = tinydb_open(argv[1]);
    if (db == NULL) return 3;
    if (!exec_ok(db, "CREATE TABLE metrics (id INT, bucket INT, score INT);")) {
        tinydb_close(db);
        return 4;
    }

    for (uint32_t id = 1u; id <= 40u; id++) {
        char sql[128];
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO metrics VALUES (%u, %u, %u);",
                 id,
                 id % 4u,
                 id * 10u);
        if (!exec_ok(db, sql)) {
            tinydb_close(db);
            return 5;
        }
    }

    Table* table = tinydb_table(db);
    TableSchema* schema = table_get_schema(table, "metrics");
    if (schema == NULL || schema->row_size != 12u) {
        fprintf(stderr, "unexpected metrics schema\n");
        tinydb_close(db);
        return 6;
    }
    if (!migrate_edge_leaves(table, schema) ||
        !verify_reads(table, schema) ||
        !expect_v2_length_mismatch_rejected(table, schema) ||
        !expect_out_of_range_sibling_rejected(table, schema) ||
        !expect_mutation_rejected(table, schema) ||
        !install_v2_envelope(table, schema)) {
        tinydb_close(db);
        return 7;
    }

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return 8;
    table = tinydb_table(db);
    schema = table_get_schema(table, "metrics");
    if (schema == NULL || !verify_reads(table, schema) ||
        !expect_envelope_corruption_rejected(table, schema) ||
        !expect_v2_length_mismatch_rejected(table, schema) ||
        !expect_out_of_range_sibling_rejected(table, schema) ||
        !expect_mutation_rejected(table, schema)) {
        tinydb_close(db);
        return 9;
    }

    tinydb_close(db);
    printf("PASS: dual-format generic reads traverse mixed fixed-V1/slotted-V2 leaves across lookup, scan, reopen, accept a versioned schema-bound V2 row envelope, reject corrupt envelope version/schema/length metadata and raw schema-length mismatches, and fail closed on out-of-range sibling corruption without extending the pager; legacy mutation remains rejected.\n");
    return 0;
}