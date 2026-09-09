#include "record_payload.h"
#include "record_payload_try_scan.h"
#include "slotted_leaf_v2.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t seen;
    uint32_t id_sum;
    uint32_t stop_after;
} ScanState;

static TableSchema wide_schema(uint32_t root_page_num) {
    TableSchema schema;
    memset(&schema, 0, sizeof(schema));
    snprintf(schema.name, sizeof(schema.name), "%s", "try_scan_wide");
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

static bool make_payload(const TableSchema* schema,
                         uint32_t id,
                         TinyDBRecordPayload* payload) {
    TinyDBValue values[3];
    memset(values, 0, sizeof(values));
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    memset(values[1].text, (int)('a' + (id / 10u) % 20u), 220u);
    values[1].text[220] = '\0';
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id * 3u;

    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_encode_values(schema,
                                               values,
                                               3u,
                                               payload,
                                               message,
                                               sizeof(message));
}

static bool visit_payload(const TableSchema* schema,
                          const TinyDBRecordPayload* payload,
                          void* raw_context) {
    ScanState* state = (ScanState*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_decode_values(schema,
                                             payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &value_count,
                                             message,
                                             sizeof(message)) ||
        value_count != 3u) {
        return false;
    }
    state->seen++;
    state->id_sum += values[0].int_value;
    return state->stop_after == 0u || state->seen < state->stop_after;
}

static bool release_handles(PagerPageHandle* handles, uint32_t count) {
    bool ok = true;
    for (uint32_t i = 0u; i < count; i++) {
        if (handles[i].pinned && !pager_release_page_handle(&handles[i])) ok = false;
    }
    return ok;
}

static bool expect_scan(Table* table,
                        const TableSchema* schema,
                        bool range,
                        uint32_t min_id,
                        uint32_t max_id,
                        uint32_t expected_count,
                        uint32_t expected_sum,
                        uint32_t stop_after,
                        char* message,
                        size_t message_size) {
    ScanState state;
    memset(&state, 0, sizeof(state));
    state.stop_after = stop_after;
    bool complete = false;
    uint32_t count = range
        ? tinydb_record_payload_try_scan_range(table,
                                               schema,
                                               min_id,
                                               max_id,
                                               visit_payload,
                                               &state,
                                               &complete,
                                               message,
                                               message_size)
        : tinydb_record_payload_try_scan(table,
                                         schema,
                                         visit_payload,
                                         &state,
                                         &complete,
                                         message,
                                         message_size);
    return complete && count == expected_count &&
           state.seen == expected_count && state.id_sum == expected_sum;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_try_scan_probe <db>\n");
        return 1;
    }

    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return 1;

    uint32_t root = table->root_page_num;
    void* root_page = get_page(table->pager, root);
    TableSchema schema = wide_schema(root);
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(&schema, message, sizeof(message)) ||
        !tinydb_slotted_leaf_v2_init(root_page, PAGE_SIZE)) {
        fprintf(stderr, "unable to initialize wide V2 root: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }
    set_node_root(root_page, true);

    for (uint32_t id = 10u; id <= 400u; id += 10u) {
        TinyDBRecordPayload payload;
        if (!make_payload(&schema, id, &payload) ||
            !tinydb_record_payload_insert(table,
                                          &schema,
                                          &payload,
                                          message,
                                          sizeof(message))) {
            fprintf(stderr, "payload insert %u failed: %s\n", id, message);
            db_close(table);
            remove(argv[1]);
            return 1;
        }
    }

    root_page = get_page(table->pager, root);
    if (get_node_type(root_page) != NODE_INTERNAL) {
        fprintf(stderr, "pressure fixture did not produce a multi-leaf V2 tree\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!expect_scan(table,
                     &schema,
                     false,
                     0u,
                     0u,
                     40u,
                     8200u,
                     0u,
                     message,
                     sizeof(message)) ||
        !expect_scan(table,
                     &schema,
                     true,
                     105u,
                     305u,
                     20u,
                     4100u,
                     0u,
                     message,
                     sizeof(message)) ||
        !expect_scan(table,
                     &schema,
                     false,
                     0u,
                     0u,
                     5u,
                     150u,
                     5u,
                     message,
                     sizeof(message))) {
        fprintf(stderr, "normal non-fatal payload scan semantics failed: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    bool inverted_complete = false;
    if (tinydb_record_payload_try_scan_range(table,
                                             &schema,
                                             300u,
                                             200u,
                                             NULL,
                                             NULL,
                                             &inverted_complete,
                                             message,
                                             sizeof(message)) != 0u ||
        !inverted_complete) {
        fprintf(stderr, "inverted non-fatal payload range was not a valid empty scan\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    pager_checkpoint(table->pager);
    uint32_t first_dummy = table->pager->num_pages;
    for (uint32_t i = 0u; i < MAX_BUFFER_POOL_SIZE; i++) {
        (void)get_page(table->pager, first_dummy + i);
    }

    PagerPageHandle owners[MAX_BUFFER_POOL_SIZE];
    memset(owners, 0, sizeof(owners));
    uint32_t owner_count = 0u;
    for (uint32_t i = 0u; i < MAX_BUFFER_POOL_SIZE; i++) {
        if (!pager_pin_page_handle(table->pager,
                                   first_dummy + i,
                                   &owners[owner_count])) {
            (void)release_handles(owners, owner_count);
            fprintf(stderr, "unable to pin complete payload-scan pressure fixture\n");
            db_close(table);
            remove(argv[1]);
            return 1;
        }
        owner_count++;
    }

    ScanState busy_state;
    memset(&busy_state, 0, sizeof(busy_state));
    bool busy_complete = true;
    memset(message, 0, sizeof(message));
    uint32_t busy_count = tinydb_record_payload_try_scan(table,
                                                         &schema,
                                                         visit_payload,
                                                         &busy_state,
                                                         &busy_complete,
                                                         message,
                                                         sizeof(message));
    if (busy_count != 0u || busy_complete || busy_state.seen != 0u ||
        strstr(message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        fprintf(stderr,
                "non-fatal payload scan did not fail closed under 16/16 pressure: count=%u complete=%d seen=%u message=%s\n",
                busy_count,
                busy_complete ? 1 : 0,
                busy_state.seen,
                message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!pager_release_page_handle(&owners[MAX_BUFFER_POOL_SIZE - 1u])) {
        (void)release_handles(owners, owner_count);
        fprintf(stderr, "unable to free one payload-scan frame\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!expect_scan(table,
                     &schema,
                     false,
                     0u,
                     0u,
                     40u,
                     8200u,
                     0u,
                     NULL,
                     0u) ||
        !expect_scan(table,
                     &schema,
                     true,
                     105u,
                     305u,
                     20u,
                     4100u,
                     0u,
                     message,
                     sizeof(message))) {
        (void)release_handles(owners, owner_count);
        fprintf(stderr, "payload scans did not recover with one free frame: %s\n", message);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!release_handles(owners, owner_count)) {
        fprintf(stderr, "unable to release payload-scan pressure owners\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    db_close(table);
    remove(argv[1]);
    printf("PAYLOAD_TRY_SCAN_OK rows=40 multileaf=yes range=20 early_stop=yes busy_nonfatal=yes busy_zero_callback=yes one_free_frame_full=yes one_free_frame_range=yes optional_message=yes\n");
    return 0;
}
