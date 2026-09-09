#include "leaf_format.h"
#include "leaf_value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_PAYLOAD_SIZE 13u
#define PROBE_ROWS 48u

static void make_payload(uint32_t key,
                         const char* label,
                         unsigned char output[PROBE_PAYLOAD_SIZE]) {
    memset(output, 0, PROBE_PAYLOAD_SIZE);
    memcpy(output, &key, sizeof(key));
    snprintf((char*)output + sizeof(key),
             PROBE_PAYLOAD_SIZE - sizeof(key),
             "%s",
             label);
}

static bool cursor_has_key(Table* table, Cursor* cursor, uint32_t key) {
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t count = *leaf_node_num_cells(node);
    return cursor->cell_num < count &&
           *leaf_node_key(node, cursor->cell_num) == key;
}

static bool expect_payload(Table* table,
                           uint32_t key,
                           const unsigned char expected[PROBE_PAYLOAD_SIZE]) {
    Cursor* cursor = table_find(table, key);
    if (cursor == NULL || !cursor_has_key(table, cursor, key)) {
        fprintf(stderr, "key %u was not found\n", key);
        free(cursor);
        return false;
    }

    unsigned char slot[ROW_SIZE];
    memset(slot, 0xA5, sizeof(slot));
    bool ok = tinydb_leaf_value_read(cursor,
                                     slot,
                                     sizeof(slot),
                                     ROW_SIZE);
    free(cursor);
    if (!ok) {
        fprintf(stderr, "key %u could not be read through leaf_value\n", key);
        return false;
    }
    if (memcmp(slot, expected, PROBE_PAYLOAD_SIZE) != 0) {
        fprintf(stderr, "key %u logical payload does not match\n", key);
        return false;
    }
    for (uint32_t i = PROBE_PAYLOAD_SIZE; i < ROW_SIZE; i++) {
        if (slot[i] != 0u) {
            fprintf(stderr,
                    "key %u fixed-slot padding is nonzero at byte %u\n",
                    key,
                    i);
            return false;
        }
    }
    return true;
}

static uint32_t count_rows(Table* table) {
    Cursor* cursor = table_start(table);
    if (cursor == NULL) return 0u;
    uint32_t count = 0u;
    while (!cursor->end_of_table) {
        count++;
        cursor_advance(cursor);
    }
    free(cursor);
    return count;
}

static bool expect_fixed_v1_format(void) {
    const TinyDBLeafFormatDescriptor* format = tinydb_leaf_format_current();
    if (format == NULL || !tinydb_leaf_format_validate_current()) return false;
    if (format->format_version != TINYDB_LEAF_FORMAT_FIXED_V1 ||
        format->variable_length_values ||
        format->page_size != PAGE_SIZE ||
        format->usable_size != PAGE_USABLE_SIZE ||
        format->header_size != LEAF_NODE_HEADER_SIZE ||
        format->key_size != LEAF_NODE_KEY_SIZE ||
        format->value_capacity != ROW_SIZE ||
        format->cell_size != LEAF_NODE_CELL_SIZE ||
        format->max_cells != LEAF_NODE_MAX_CELLS) {
        return false;
    }
    return tinydb_leaf_format_can_store_value(PROBE_PAYLOAD_SIZE) &&
           tinydb_leaf_format_can_store_value(ROW_SIZE) &&
           !tinydb_leaf_format_can_store_value(0u) &&
           !tinydb_leaf_format_can_store_value(ROW_SIZE + 1u);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s DATABASE\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (!expect_fixed_v1_format()) {
        fprintf(stderr, "current leaf format descriptor is inconsistent\n");
        return EXIT_FAILURE;
    }
    if (!tinydb_leaf_value_legacy_layout_compatible()) {
        fprintf(stderr,
                "legacy Row ABI is incompatible with the canonical leaf value layout\n");
        return EXIT_FAILURE;
    }

    Table* table = db_open(argv[1]);
    if (table == NULL) return EXIT_FAILURE;

    for (uint32_t key = 1u; key <= PROBE_ROWS; key++) {
        unsigned char payload[PROBE_PAYLOAD_SIZE];
        char label[9];
        snprintf(label, sizeof(label), "v%06u", key);
        make_payload(key, label, payload);

        Cursor* cursor = table_find(table, key);
        if (cursor == NULL ||
            !tinydb_leaf_value_insert(cursor,
                                      key,
                                      payload,
                                      PROBE_PAYLOAD_SIZE)) {
            fprintf(stderr, "insert %u failed\n", key);
            free(cursor);
            db_close(table);
            return EXIT_FAILURE;
        }
        free(cursor);
    }

    if (count_rows(table) != PROBE_ROWS) {
        fprintf(stderr, "unexpected row count before reopen\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    void* root = get_page(table->pager, table->root_page_num);
    if (get_node_type(root) != NODE_INTERNAL) {
        fprintf(stderr, "probe did not force a leaf split\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    unsigned char first[PROBE_PAYLOAD_SIZE];
    unsigned char middle[PROBE_PAYLOAD_SIZE];
    unsigned char last[PROBE_PAYLOAD_SIZE];
    make_payload(1u, "v000001", first);
    make_payload(17u, "updated", middle);
    make_payload(PROBE_ROWS, "v000048", last);

    Cursor* update_cursor = table_find(table, 17u);
    if (update_cursor == NULL ||
        !cursor_has_key(table, update_cursor, 17u) ||
        !tinydb_leaf_value_write(update_cursor,
                                 middle,
                                 PROBE_PAYLOAD_SIZE)) {
        fprintf(stderr, "update through leaf_value failed\n");
        free(update_cursor);
        db_close(table);
        return EXIT_FAILURE;
    }

    unsigned char small_buffer[4];
    unsigned char dummy[ROW_SIZE + 1u];
    if (tinydb_leaf_value_read(update_cursor,
                               small_buffer,
                               sizeof(small_buffer),
                               PROBE_PAYLOAD_SIZE) ||
        tinydb_leaf_value_write(update_cursor, dummy, ROW_SIZE + 1u) ||
        tinydb_leaf_value_write(update_cursor, dummy, 0u)) {
        fprintf(stderr, "leaf_value accepted invalid length/capacity arguments\n");
        free(update_cursor);
        db_close(table);
        return EXIT_FAILURE;
    }
    free(update_cursor);

    if (!expect_payload(table, 1u, first) ||
        !expect_payload(table, 17u, middle) ||
        !expect_payload(table, PROBE_ROWS, last) ||
        !db_integrity_check(table)) {
        db_close(table);
        return EXIT_FAILURE;
    }

    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL) return EXIT_FAILURE;

    if (count_rows(table) != PROBE_ROWS ||
        !expect_payload(table, 1u, first) ||
        !expect_payload(table, 17u, middle) ||
        !expect_payload(table, PROBE_ROWS, last) ||
        !db_integrity_check(table)) {
        db_close(table);
        return EXIT_FAILURE;
    }

    printf("LEAF_VALUE_OK rows=%u split=yes reopen=yes legacy_layout=yes padding=canonical format=v1\n",
           PROBE_ROWS);
    db_close(table);
    return EXIT_SUCCESS;
}
