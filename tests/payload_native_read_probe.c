#include "record.h"
#include "record_payload.h"
#include "record_payload_try_find.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t seen;
    uint32_t id_sum;
} ScanState;

static TableSchema wide_schema(uint32_t root_page_num) {
    TableSchema schema;
    memset(&schema, 0, sizeof(schema));
    snprintf(schema.name, sizeof(schema.name), "%s", "wide_payloads");
    schema.root_page_num = root_page_num;
    schema.num_columns = 3u;

    snprintf(schema.columns[0].name, sizeof(schema.columns[0].name), "%s", "id");
    schema.columns[0].type = COL_TYPE_INT;
    schema.columns[0].offset = 0u;
    schema.columns[0].size = 4u;

    snprintf(schema.columns[1].name, sizeof(schema.columns[1].name), "%s", "description");
    schema.columns[1].type = COL_TYPE_VARCHAR;
    schema.columns[1].offset = 4u;
    schema.columns[1].size = 300u;

    snprintf(schema.columns[2].name, sizeof(schema.columns[2].name), "%s", "score");
    schema.columns[2].type = COL_TYPE_INT;
    schema.columns[2].offset = 304u;
    schema.columns[2].size = 4u;
    schema.row_size = 308u;
    return schema;
}

static bool release_handles(PagerPageHandle* handles, uint32_t count) {
    bool ok = true;
    for (uint32_t i = 0u; i < count; i++) {
        if (handles[i].pinned && !pager_release_page_handle(&handles[i])) ok = false;
    }
    return ok;
}

static bool insert_wide(void* page,
                        const TableSchema* schema,
                        uint32_t id,
                        const char* description,
                        uint32_t score) {
    TinyDBValue values[3];
    memset(values, 0, sizeof(values));
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "%s", description);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = score;

    TinyDBRecordPayload payload;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_encode_values(schema,
                                             values,
                                             3u,
                                             &payload,
                                             message,
                                             sizeof(message))) {
        fprintf(stderr, "payload encode failed: %s\n", message);
        return false;
    }
    if (payload.length != 308u || payload.length <= ROW_SIZE) return false;

    unsigned char envelope[512];
    uint32_t envelope_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               &payload,
                                               envelope,
                                               sizeof(envelope),
                                               &envelope_length)) {
        return false;
    }
    if (envelope_length >= payload.length || envelope_length > UINT16_MAX) return false;
    return tinydb_slotted_leaf_v2_insert(page,
                                         PAGE_SIZE,
                                         id,
                                         envelope,
                                         (uint16_t)envelope_length);
}

static bool expect_payload(const TableSchema* schema,
                           const TinyDBRecordPayload* payload,
                           uint32_t id,
                           const char* description,
                           uint32_t score) {
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_decode_values(schema,
                                               payload,
                                               values,
                                               MAX_COLUMNS_PER_TABLE,
                                               &count,
                                               message,
                                               sizeof(message)) &&
           count == 3u &&
           values[0].int_value == id &&
           strcmp(values[1].text, description) == 0 &&
           values[2].int_value == score;
}

static bool visit_payload(const TableSchema* schema,
                          const TinyDBRecordPayload* payload,
                          void* raw_context) {
    ScanState* state = (ScanState*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_decode_values(schema,
                                             payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &count,
                                             message,
                                             sizeof(message)) ||
        count != 3u) {
        return false;
    }
    state->seen++;
    state->id_sum += values[0].int_value;
    return true;
}

static bool expect_range(Table* table,
                         const TableSchema* schema,
                         uint32_t min_id,
                         uint32_t max_id,
                         uint32_t expected_count,
                         uint32_t expected_sum) {
    ScanState state;
    memset(&state, 0, sizeof(state));
    bool scan_complete = false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    uint32_t count = tinydb_record_payload_scan_range(table,
                                                      schema,
                                                      min_id,
                                                      max_id,
                                                      visit_payload,
                                                      &state,
                                                      &scan_complete,
                                                      message,
                                                      sizeof(message));
    if (!scan_complete || count != expected_count ||
        state.seen != expected_count || state.id_sum != expected_sum) {
        fprintf(stderr,
                "payload range [%u,%u] failed: complete=%d count=%u seen=%u sum=%u message=%s\n",
                min_id,
                max_id,
                scan_complete ? 1 : 0,
                count,
                state.seen,
                state.id_sum,
                message);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_native_read_probe <db>\n");
        return 1;
    }
    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return 1;

    uint32_t root = table->root_page_num;
    void* page = get_page(table->pager, root);
    TableSchema schema = wide_schema(root);
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(&schema, message, sizeof(message)) ||
        schema.row_size <= ROW_SIZE ||
        !tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) {
        fprintf(stderr, "wide schema/V2 initialization failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }
    set_node_root(page, true);

    if (!insert_wide(page, &schema, 7u, "alpha", 70u) ||
        !insert_wide(page, &schema, 19u, "a much longer beta payload", 190u) ||
        !insert_wide(page, &schema, 31u, "gamma", 310u)) {
        fprintf(stderr, "wide V2 insert setup failed\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    TinyDBRecordPayload found;
    if (!tinydb_record_payload_find(table,
                                    &schema,
                                    19u,
                                    &found,
                                    message,
                                    sizeof(message)) ||
        found.length != 308u ||
        !expect_payload(&schema,
                        &found,
                        19u,
                        "a much longer beta payload",
                        190u)) {
        fprintf(stderr, "payload-native wide point lookup failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!tinydb_record_payload_try_find(table,
                                        &schema,
                                        19u,
                                        &found,
                                        message,
                                        sizeof(message)) ||
        found.length != 308u ||
        !expect_payload(&schema,
                        &found,
                        19u,
                        "a much longer beta payload",
                        190u)) {
        fprintf(stderr, "non-fatal payload-native point lookup failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    memset(&found, 0xA5, sizeof(found));
    memset(message, 0, sizeof(message));
    if (tinydb_record_payload_try_find(table,
                                       &schema,
                                       20u,
                                       &found,
                                       message,
                                       sizeof(message)) ||
        found.length != 0u || found.bytes[0] != 0u ||
        strstr(message, "primary key not found") == NULL) {
        fprintf(stderr,
                "non-fatal payload missing-key semantics failed: length=%u message=%s\n",
                found.length,
                message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    TinyDBRecord legacy;
    if (tinydb_record_find(table, &schema, 19u, &legacy)) {
        fprintf(stderr, "legacy TinyDBRecord unexpectedly accepted a 308-byte row\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!expect_range(table, &schema, 8u, 30u, 1u, 19u) ||
        !expect_range(table, &schema, 7u, 19u, 2u, 26u) ||
        !expect_range(table, &schema, 8u, 18u, 0u, 0u) ||
        !expect_range(table, &schema, 30u, 20u, 0u, 0u)) {
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    ScanState state;
    memset(&state, 0, sizeof(state));
    bool scan_complete = false;
    uint32_t count = tinydb_record_payload_scan(table,
                                                &schema,
                                                visit_payload,
                                                &state,
                                                &scan_complete,
                                                message,
                                                sizeof(message));
    if (!scan_complete || count != 3u || state.seen != 3u || state.id_sum != 57u) {
        fprintf(stderr,
                "payload-native wide scan failed: complete=%d count=%u seen=%u sum=%u message=%s\n",
                scan_complete ? 1 : 0,
                count,
                state.seen,
                state.id_sum,
                message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    mark_page_dirty(table->pager, root);
    pager_checkpoint(table->pager);
    for (uint32_t page_num = 1u; page_num <= MAX_BUFFER_POOL_SIZE; page_num++) {
        (void)get_page(table->pager, page_num);
    }

    PagerPageHandle owners[MAX_BUFFER_POOL_SIZE];
    memset(owners, 0, sizeof(owners));
    uint32_t owner_count = 0u;
    for (uint32_t page_num = 1u; page_num <= MAX_BUFFER_POOL_SIZE; page_num++) {
        if (!pager_pin_page_handle(table->pager, page_num, &owners[owner_count])) {
            (void)release_handles(owners, owner_count);
            fprintf(stderr, "unable to pin complete payload-read pressure fixture\n");
            db_close(table);
            remove(argv[1]);
            return 1;
        }
        owner_count++;
    }

    memset(&found, 0xA5, sizeof(found));
    memset(message, 0, sizeof(message));
    if (tinydb_record_payload_try_find(table,
                                       &schema,
                                       19u,
                                       &found,
                                       message,
                                       sizeof(message)) ||
        found.length != 0u || found.bytes[0] != 0u ||
        strstr(message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        fprintf(stderr,
                "non-fatal payload lookup did not fail closed under 16/16 pressure: length=%u message=%s\n",
                found.length,
                message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!pager_release_page_handle(&owners[MAX_BUFFER_POOL_SIZE - 1u])) {
        (void)release_handles(owners, owner_count);
        fprintf(stderr, "unable to free one payload-read frame\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!tinydb_record_payload_try_find(table,
                                        &schema,
                                        19u,
                                        &found,
                                        NULL,
                                        0u) ||
        !expect_payload(&schema,
                        &found,
                        19u,
                        "a much longer beta payload",
                        190u)) {
        (void)release_handles(owners, owner_count);
        fprintf(stderr, "non-fatal payload lookup did not recover with one free frame\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!release_handles(owners, owner_count)) {
        fprintf(stderr, "unable to release payload-read pressure owners\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    db_close(table);
    remove(argv[1]);
    printf("PAYLOAD_NATIVE_READ_OK row_size=308 rows=3 range=1 try_find_missing_key=yes try_find_busy_nonfatal=yes try_find_zero_on_failure=yes try_find_one_free_frame_success=yes optional_message=yes\n");
    return 0;
}
