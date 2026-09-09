#include "engine.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_migration.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool exec_ok(TinyDB* db, const char* sql) {
    TinyDBSqlResult result;
    if (tinydb_execute_sql(db, sql, &result) != TINYDB_SQL_SUCCESS) {
        fprintf(stderr, "SQL failed: %s (%s)\n", sql, result.message);
        return false;
    }
    return true;
}

static bool exec_fails(TinyDB* db, const char* sql) {
    TinyDBSqlResult result;
    return tinydb_execute_sql(db, sql, &result) != TINYDB_SQL_SUCCESS;
}

static TableSchema* find_schema(Table* table, const char* name) {
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool expect_item(Table* table,
                        TableSchema* schema,
                        uint32_t id,
                        const char* name,
                        uint32_t price) {
    TinyDBRecord record;
    if (!tinydb_record_find(table, schema, id, &record)) return false;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_decode(schema,
                                &record,
                                values,
                                MAX_COLUMNS_PER_TABLE,
                                &count,
                                message,
                                sizeof(message)) &&
           count == 3u &&
           values[0].int_value == id &&
           strcmp(values[1].text, name) == 0 &&
           values[2].int_value == price;
}

static bool expect_original_item(Table* table,
                                 TableSchema* schema,
                                 uint32_t id) {
    char name[64];
    snprintf(name, sizeof(name), "item-%u", id);
    return expect_item(table, schema, id, name, id * 10u);
}

static bool item_absent(Table* table, TableSchema* schema, uint32_t id) {
    TinyDBRecord record;
    return !tinydb_record_find(table, schema, id, &record);
}

static bool install_compact_row(Table* table, TableSchema* schema, uint32_t key) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = table_find(table, key);
    if (cursor == NULL) {
        table->root_page_num = previous_root;
        return false;
    }
    uint32_t page_num = cursor->page_num;
    free(cursor);

    void* page = get_page(table->pager, page_num);
    if (!tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE)) {
        table->root_page_num = previous_root;
        return false;
    }
    unsigned char migrated[PAGE_SIZE];
    memset(migrated, 0, sizeof(migrated));
    if (!tinydb_leaf_migrate_v1_to_v2(page,
                                      PAGE_SIZE,
                                      schema->row_size,
                                      migrated,
                                      sizeof(migrated))) {
        table->root_page_num = previous_root;
        return false;
    }
    memcpy(page, migrated, PAGE_USABLE_SIZE);
    mark_page_dirty(table->pager, page_num);

    Cursor* v2_cursor = tinydb_leaf_read_find(table, key);
    if (v2_cursor == NULL || v2_cursor->page_num != page_num) {
        free(v2_cursor);
        table->root_page_num = previous_root;
        return false;
    }
    const void* raw = NULL;
    uint32_t raw_length = 0u;
    if (!tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   v2_cursor->cell_num,
                                   &raw,
                                   &raw_length) ||
        raw_length != schema->row_size) {
        free(v2_cursor);
        table->root_page_num = previous_root;
        return false;
    }

    TinyDBRecordPayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.length = raw_length;
    memcpy(payload.bytes, raw, raw_length);
    unsigned char compact[512];
    uint32_t compact_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               &payload,
                                               compact,
                                               sizeof(compact),
                                               &compact_length) ||
        compact_length >= TINYDB_ROW_ENVELOPE_HEADER_SIZE + schema->row_size) {
        free(v2_cursor);
        table->root_page_num = previous_root;
        return false;
    }

    bool updated = tinydb_slotted_leaf_v2_update(page,
                                                 PAGE_SIZE,
                                                 key,
                                                 compact,
                                                 (uint16_t)compact_length);
    free(v2_cursor);
    if (updated) {
        mark_page_dirty(table->pager, page_num);
        pager_commit(table->pager);
    }
    table->root_page_num = previous_root;
    return updated;
}

static bool compact_row_metadata(Table* table,
                                 TableSchema* schema,
                                 uint32_t key,
                                 uint32_t* stored_length) {
    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    if (cursor == NULL) {
        table->root_page_num = previous_root;
        return false;
    }
    void* page = get_page(table->pager, cursor->page_num);
    const void* value = NULL;
    uint32_t length = 0u;
    bool ok = tinydb_leaf_format_detect_page(page, PAGE_SIZE) ==
                  TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
              tinydb_leaf_page_value_at(page,
                                        PAGE_SIZE,
                                        cursor->cell_num,
                                        &value,
                                        &length) &&
              value != NULL && length >= TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE &&
              tinydb_row_envelope_read_u32_le(
                  (const unsigned char*)value + TINYDB_ROW_ENVELOPE_MAGIC_OFFSET) ==
                  TINYDB_ROW_ENVELOPE_MAGIC &&
              tinydb_row_envelope_read_u16_le(
                  (const unsigned char*)value + TINYDB_ROW_ENVELOPE_VERSION_OFFSET) ==
                  TINYDB_ROW_ENVELOPE_VERSION_COMPACT_V2;
    if (ok && stored_length != NULL) *stored_length = length;
    free(cursor);
    table->root_page_num = previous_root;
    return ok;
}

static bool v2_leaf_delete_keys(Table* table,
                                TableSchema* schema,
                                uint32_t probe_key,
                                uint32_t* max_key,
                                uint32_t* rollback_key) {
    if (table == NULL || schema == NULL || max_key == NULL ||
        rollback_key == NULL) {
        return false;
    }

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, probe_key);
    if (cursor == NULL || cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    void* page = get_page(table->pager, cursor->page_num);
    uint32_t count = 0u;
    bool ok = tinydb_leaf_format_detect_page(page, PAGE_SIZE) ==
                  TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
              tinydb_leaf_page_count(page, PAGE_SIZE, &count) && count >= 3u &&
              tinydb_leaf_page_key_at(page,
                                      PAGE_SIZE,
                                      count - 1u,
                                      max_key);

    uint32_t candidate = 0u;
    bool candidate_found = false;
    if (ok) {
        for (uint32_t i = 0u; i + 1u < count; i++) {
            uint32_t key = 0u;
            if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, i, &key)) {
                ok = false;
                break;
            }
            /* Keys >=5 are untouched seed rows, so their expected payload can
             * be reconstructed deterministically after rollback/reopen. */
            if (key >= 5u && key != *max_key) {
                candidate = key;
                candidate_found = true;
                break;
            }
        }
    }

    free(cursor);
    table->root_page_num = previous_root;
    if (!ok || !candidate_found || *max_key < 5u) return false;
    *rollback_key = candidate;
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: compact_v2_read_probe <db>\n");
        return 1;
    }
    remove(argv[1]);

    TinyDB* db = tinydb_open(argv[1]);
    if (db == NULL) return 1;
    if (!exec_ok(db, "CREATE TABLE items (id INT, name VARCHAR(64), price INT);")) {
        tinydb_close(db);
        return 1;
    }
    for (uint32_t i = 1u; i <= 24u; i++) {
        char sql[192];
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO items VALUES (%u, 'item-%u', %u);",
                 i,
                 i,
                 i * 10u);
        if (!exec_ok(db, sql)) {
            tinydb_close(db);
            return 1;
        }
    }

    /* Create two interior gaps while the tree is still entirely fixed V1. The
     * first gap will be filled by a committed V2 insert; the second exercises
     * transactional rollback of the same fast path. */
    if (!exec_ok(db, "DELETE FROM items WHERE id = 2;") ||
        !exec_ok(db, "DELETE FROM items WHERE id = 4;")) {
        tinydb_close(db);
        return 1;
    }

    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t initial_compact_length = 0u;
    if (schema == NULL || schema->row_size != 73u ||
        !install_compact_row(table, schema, 1u) ||
        !expect_item(table, schema, 1u, "item-1", 10u) ||
        !compact_row_metadata(table, schema, 1u, &initial_compact_length) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 22u) {
        fprintf(stderr, "compact V2 row was not readable before mutation\n");
        tinydb_close(db);
        return 1;
    }

    /* A missing interior key whose target is already V2 may be inserted when
     * the existing leaf has free space. The inserted value must itself be a
     * compact V2 envelope and duplicate insertion must remain rejected. */
    if (!exec_ok(db, "INSERT INTO items VALUES (2, 'v2-inserted', 202);") ||
        !expect_item(table, schema, 2u, "v2-inserted", 202u) ||
        !compact_row_metadata(table, schema, 2u, NULL) ||
        !exec_fails(db, "INSERT INTO items VALUES (2, 'duplicate', 999);") ||
        tinydb_record_scan(table, schema, NULL, NULL) != 23u) {
        fprintf(stderr, "production non-split compact V2 INSERT failed\n");
        tinydb_close(db);
        return 1;
    }

    /* Internal separators track the maximum key of their left child. The
     * leftmost V2 leaf therefore may safely accept a new lower boundary key:
     * its maximum key, parent separator, and sibling topology do not change. */
    if (!exec_ok(db, "INSERT INTO items VALUES (0, 'lower-boundary', 0);") ||
        !expect_item(table, schema, 0u, "lower-boundary", 0u) ||
        !compact_row_metadata(table, schema, 0u, NULL) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 24u) {
        fprintf(stderr, "safe compact V2 lower-boundary INSERT failed\n");
        tinydb_close(db);
        return 1;
    }

    /* Existing-row UPDATE may change the physical V2 payload length because it
     * does not change tree topology. Test both shrink and growth. */
    if (!exec_ok(db, "UPDATE items SET name = 'a', price = 111 WHERE id = 1;") ||
        !expect_item(table, schema, 1u, "a", 111u)) {
        fprintf(stderr, "production UPDATE could not shrink a compact V2 row\n");
        tinydb_close(db);
        return 1;
    }
    uint32_t short_length = 0u;
    if (!compact_row_metadata(table, schema, 1u, &short_length) ||
        short_length >= initial_compact_length) {
        fprintf(stderr, "compact V2 UPDATE did not shrink physical payload\n");
        tinydb_close(db);
        return 1;
    }

    if (!exec_ok(db,
                 "UPDATE items SET name = 'a-significantly-longer-item-name', price = 222 WHERE id = 1;") ||
        !expect_item(table,
                     schema,
                     1u,
                     "a-significantly-longer-item-name",
                     222u)) {
        fprintf(stderr, "production UPDATE could not grow a compact V2 row\n");
        tinydb_close(db);
        return 1;
    }
    uint32_t long_length = 0u;
    if (!compact_row_metadata(table, schema, 1u, &long_length) ||
        long_length <= short_length) {
        fprintf(stderr, "compact V2 UPDATE did not grow physical payload\n");
        tinydb_close(db);
        return 1;
    }

    /* Updating an untouched fixed leaf must also remain possible even though a
     * different leaf in the same tree is already V2. */
    if (!exec_ok(db,
                 "UPDATE items SET name = 'fixed-updated', price = 2424 WHERE id = 24;") ||
        !expect_item(table, schema, 24u, "fixed-updated", 2424u)) {
        fprintf(stderr, "mixed tree blocked an existing-row V1 update\n");
        tinydb_close(db);
        return 1;
    }

    uint32_t v2_max_key = 0u;
    uint32_t rollback_delete_key = 0u;
    if (!v2_leaf_delete_keys(table,
                             schema,
                             1u,
                             &v2_max_key,
                             &rollback_delete_key)) {
        fprintf(stderr, "unable to derive V2 delete keys\n");
        tinydb_close(db);
        return 1;
    }

    /* Structural growth at the untouched fixed right edge remains blocked.
     * Deleting an interior V2 key is topology-neutral, while deleting the
     * maximum of a non-empty V2 leaf now atomically lowers the parent separator.
     * Both mutations must preserve mixed V1/V2 routing and row counts. */
    char max_delete_sql[96];
    snprintf(max_delete_sql,
             sizeof(max_delete_sql),
             "DELETE FROM items WHERE id = %u;",
             v2_max_key);
    if (!exec_fails(db, "INSERT INTO items VALUES (25, 'blocked', 250);") ||
        !exec_ok(db, "DELETE FROM items WHERE id = 3;") ||
        !item_absent(table, schema, 3u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 23u ||
        !exec_ok(db, max_delete_sql) ||
        !item_absent(table, schema, v2_max_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 22u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "slotted V2 topology-neutral/max-key DELETE policy failed\n");
        tinydb_close(db);
        return 1;
    }

    /* V2 UPDATE, INSERT, and topology-neutral DELETE all participate in the
     * Pager transaction rollback path. id=4 is an existing interior gap; the
     * dynamically chosen delete key is an untouched non-maximum V2 row. */
    char rollback_delete_sql[96];
    snprintf(rollback_delete_sql,
             sizeof(rollback_delete_sql),
             "DELETE FROM items WHERE id = %u;",
             rollback_delete_key);
    if (!exec_ok(db, "BEGIN;") ||
        !exec_ok(db,
                 "UPDATE items SET name = 'rolled-back', price = 999 WHERE id = 1;") ||
        !exec_ok(db, "INSERT INTO items VALUES (4, 'rolled-back-insert', 404);") ||
        !exec_ok(db, rollback_delete_sql) ||
        !expect_item(table, schema, 1u, "rolled-back", 999u) ||
        !expect_item(table, schema, 4u, "rolled-back-insert", 404u) ||
        !item_absent(table, schema, rollback_delete_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 22u ||
        !exec_ok(db, "ROLLBACK;") ||
        !expect_item(table,
                     schema,
                     1u,
                     "a-significantly-longer-item-name",
                     222u) ||
        !item_absent(table, schema, 4u) ||
        !expect_original_item(table, schema, rollback_delete_key) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 22u) {
        fprintf(stderr, "compact V2 mutation did not roll back atomically\n");
        tinydb_close(db);
        return 1;
    }

    if (!exec_ok(db, "PRAGMA integrity_check;")) {
        tinydb_close(db);
        return 1;
    }
    tinydb_close(db);

    db = tinydb_open(argv[1]);
    if (db == NULL) return 1;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    if (schema == NULL ||
        !expect_item(table, schema, 0u, "lower-boundary", 0u) ||
        !expect_item(table,
                     schema,
                     1u,
                     "a-significantly-longer-item-name",
                     222u) ||
        !expect_item(table, schema, 2u, "v2-inserted", 202u) ||
        !item_absent(table, schema, 3u) ||
        !item_absent(table, schema, 4u) ||
        !expect_original_item(table, schema, rollback_delete_key) ||
        !item_absent(table, schema, v2_max_key) ||
        !expect_item(table, schema, 12u, "item-12", 120u) ||
        !expect_item(table, schema, 24u, "fixed-updated", 2424u) ||
        !compact_row_metadata(table, schema, 0u, NULL) ||
        !compact_row_metadata(table, schema, 1u, NULL) ||
        !compact_row_metadata(table, schema, 2u, NULL) ||
        tinydb_record_scan(table, schema, NULL, NULL) != 22u ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "mixed V1/V2 INSERT/UPDATE/DELETE state did not survive reopen\n");
        tinydb_close(db);
        return 1;
    }
    tinydb_close(db);
    remove(argv[1]);
    printf("PASS: production compact V2 non-split INSERT supports safe lower-boundary growth; UPDATE, interior DELETE, and safe max-key separator updates commit/rollback/reopen correctly; fixed-target/split-requiring structural inserts stay fail-closed; mixed V1 updates and integrity remain valid.\n");
    return 0;
}
