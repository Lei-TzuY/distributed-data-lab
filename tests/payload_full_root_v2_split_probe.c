#include "leaf_page_access.h"
#include "record_payload.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHILD_COUNT (INTERNAL_NODE_MAX_KEYS + 1u)
#define RANGE_STEP 100000u
#define TARGET_CHILD_INDEX (INTERNAL_NODE_MAX_KEYS / 2u)
#define LONG_TEXT_LENGTH 250u

static TableSchema wide_schema(uint32_t root_page_num) {
    TableSchema schema;
    memset(&schema, 0, sizeof(schema));
    snprintf(schema.name, sizeof(schema.name), "%s", "wide_full_root");
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

static uint32_t read_u32(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void write_u32(unsigned char* bytes, uint32_t value) {
    memcpy(bytes, &value, sizeof(value));
}

static void make_long_text(char text[TINYDB_RECORD_TEXT_MAX + 1u], char marker) {
    for (uint32_t i = 0u; i < LONG_TEXT_LENGTH; i++) {
        text[i] = (char)(marker + (char)(i % 3u));
    }
    text[LONG_TEXT_LENGTH] = '\0';
}

static bool make_payload(const TableSchema* schema,
                         uint32_t id,
                         const char* description,
                         TinyDBRecordPayload* payload) {
    TinyDBValue values[3];
    char message[TINYDB_RECORD_MESSAGE_MAX];
    memset(values, 0, sizeof(values));
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "%s", description);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 123u;
    return tinydb_record_payload_encode_values(schema,
                                               values,
                                               3u,
                                               payload,
                                               message,
                                               sizeof(message));
}

static bool encode_envelope(const TableSchema* schema,
                            uint32_t id,
                            const char* description,
                            unsigned char envelope[PAGE_SIZE],
                            uint32_t* envelope_length) {
    TinyDBRecordPayload payload;
    return make_payload(schema, id, description, &payload) &&
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
                       const char* description) {
    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    return encode_envelope(schema,
                           id,
                           description,
                           envelope,
                           &envelope_length) &&
           tinydb_slotted_leaf_v2_insert(page,
                                         PAGE_SIZE,
                                         id,
                                         envelope,
                                         (uint16_t)envelope_length);
}

static uint32_t payload_count(Table* table, const TableSchema* schema) {
    bool complete = false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    uint32_t count = tinydb_record_payload_scan(table,
                                                schema,
                                                NULL,
                                                NULL,
                                                &complete,
                                                message,
                                                sizeof(message));
    if (!complete) {
        fprintf(stderr, "payload scan failed: %s\n", message);
        return UINT32_MAX;
    }
    return count;
}

static bool payload_present(Table* table,
                            const TableSchema* schema,
                            uint32_t id) {
    TinyDBRecordPayload payload;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_find(table,
                                      schema,
                                      id,
                                      &payload,
                                      message,
                                      sizeof(message));
}

static bool seed_full_root(Table* table,
                           const TableSchema* schema,
                           uint32_t* candidate_key,
                           uint32_t* baseline_rows) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num != 0u) {
        return false;
    }

    Pager* pager = table->pager;
    uint32_t leaf_pages[CHILD_COUNT];
    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        leaf_pages[i] = get_unused_page_num(pager);
        if (leaf_pages[i] == 0u || leaf_pages[i] == INVALID_PAGE_NUM) {
            return false;
        }
        (void)get_page(pager, leaf_pages[i]);
    }

    char long_text[TINYDB_RECORD_TEXT_MAX + 1u];
    make_long_text(long_text, 'a');
    uint32_t previous_max = TARGET_CHILD_INDEX * RANGE_STEP;
    *candidate_key = previous_max + 1u;

    unsigned char candidate_envelope[PAGE_SIZE];
    uint32_t candidate_length = 0u;
    if (!encode_envelope(schema,
                         *candidate_key,
                         long_text,
                         candidate_envelope,
                         &candidate_length)) {
        return false;
    }
    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + candidate_length;

    for (uint32_t i = 0u; i < CHILD_COUNT; i++) {
        unsigned char* leaf =
            (unsigned char*)get_page(pager, leaf_pages[i]);
        memset(leaf, 0, PAGE_SIZE);
        if (!tinydb_slotted_leaf_v2_init(leaf, PAGE_SIZE)) return false;
        leaf[IS_ROOT_OFFSET] = 0u;
        write_u32(leaf + PARENT_POINTER_OFFSET, schema->root_page_num);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            i == 0u ? 0u : leaf_pages[i - 1u]);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
            i + 1u == CHILD_COUNT ? 0u : leaf_pages[i + 1u]);

        uint32_t max_key = (i + 1u) * RANGE_STEP;
        if (i == TARGET_CHILD_INDEX) {
            if (!raw_insert(schema, leaf, max_key, long_text)) return false;
            uint32_t key = previous_max + 1000u;
            while (tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) >= required) {
                if (key >= max_key) return false;
                if (!raw_insert(schema, leaf, key, long_text)) return false;
                key += 1000u;
            }
            if (tinydb_slotted_leaf_v2_count(leaf, PAGE_SIZE) < 2u ||
                tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) >= required) {
                return false;
            }
        } else {
            if (!raw_insert(schema, leaf, max_key, "seed")) return false;
        }

        if (!tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaf_pages[i]);
    }

    unsigned char* root =
        (unsigned char*)get_page(pager, schema->root_page_num);
    memset(root, 0, PAGE_USABLE_SIZE);
    root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root[IS_ROOT_OFFSET] = 1u;
    write_u32(root + PARENT_POINTER_OFFSET, 0u);
    write_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET, INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        unsigned char* cell = root + INTERNAL_NODE_HEADER_SIZE +
                              i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, leaf_pages[i]);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, (i + 1u) * RANGE_STEP);
    }
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              leaf_pages[CHILD_COUNT - 1u]);
    mark_page_dirty(pager, schema->root_page_num);
    pager_commit(pager);

    *baseline_rows = payload_count(table, schema);
    return *baseline_rows != UINT32_MAX && *baseline_rows >= CHILD_COUNT;
}

static bool root_is_full_one_level(Table* table,
                                   const TableSchema* schema) {
    unsigned char* root =
        (unsigned char*)get_page(table->pager, schema->root_page_num);
    if (get_node_type(root) != NODE_INTERNAL || root[IS_ROOT_OFFSET] == 0u ||
        read_u32(root + PARENT_POINTER_OFFSET) != 0u ||
        read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS) {
        return false;
    }

    uint32_t first = read_u32(root + INTERNAL_NODE_HEADER_SIZE);
    uint32_t last = read_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
    if (first == 0u || last == 0u || first >= table->pager->num_pages ||
        last >= table->pager->num_pages) {
        return false;
    }
    return get_node_type(get_page(table->pager, first)) == NODE_LEAF &&
           get_node_type(get_page(table->pager, last)) == NODE_LEAF;
}

static bool root_has_grown(Table* table,
                           const TableSchema* schema) {
    unsigned char* root =
        (unsigned char*)get_page(table->pager, schema->root_page_num);
    if (get_node_type(root) != NODE_INTERNAL || root[IS_ROOT_OFFSET] == 0u ||
        read_u32(root + PARENT_POINTER_OFFSET) != 0u ||
        read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return false;
    }

    uint32_t left_page_num = read_u32(root + INTERNAL_NODE_HEADER_SIZE);
    uint32_t right_page_num = read_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
    if (left_page_num == 0u || right_page_num == 0u ||
        left_page_num == right_page_num ||
        left_page_num >= table->pager->num_pages ||
        right_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char* left =
        (unsigned char*)get_page(table->pager, left_page_num);
    unsigned char* right =
        (unsigned char*)get_page(table->pager, right_page_num);
    return get_node_type(left) == NODE_INTERNAL &&
           get_node_type(right) == NODE_INTERNAL &&
           left[IS_ROOT_OFFSET] == 0u && right[IS_ROOT_OFFSET] == 0u &&
           read_u32(left + PARENT_POINTER_OFFSET) == schema->root_page_num &&
           read_u32(right + PARENT_POINTER_OFFSET) == schema->root_page_num;
}

static bool insert_candidate(Table* table,
                             const TableSchema* schema,
                             uint32_t key) {
    TinyDBRecordPayload payload;
    char text[TINYDB_RECORD_TEXT_MAX + 1u];
    char message[TINYDB_RECORD_MESSAGE_MAX];
    make_long_text(text, 'x');
    if (!make_payload(schema, key, text, &payload) ||
        !tinydb_record_payload_insert(table,
                                      schema,
                                      &payload,
                                      message,
                                      sizeof(message))) {
        fprintf(stderr, "payload candidate insert failed: %s\n", message);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_full_root_v2_split_probe <db>\n");
        return 1;
    }

    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL || table->root_page_num != 0u) {
        if (table != NULL) db_close(table);
        return 1;
    }
    TableSchema schema = wide_schema(table->root_page_num);
    if (schema.row_size != 308u) {
        db_close(table);
        return 1;
    }

    uint32_t candidate_key = 0u;
    uint32_t baseline_rows = 0u;
    if (!seed_full_root(table,
                        &schema,
                        &candidate_key,
                        &baseline_rows) ||
        !root_is_full_one_level(table, &schema) ||
        payload_present(table, &schema, candidate_key)) {
        fprintf(stderr, "unable to seed full page-zero payload root\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    uint32_t baseline_pages = table->pager->num_pages;
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    if (!insert_candidate(table, &schema, candidate_key) ||
        !root_has_grown(table, &schema) ||
        !payload_present(table, &schema, candidate_key) ||
        payload_count(table, &schema) != baseline_rows + 1u) {
        fprintf(stderr, "transactional payload full-root growth failed\n");
        table->in_transaction = false;
        pager_rollback(table->pager);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    pager_rollback(table->pager);
    table->in_transaction = false;
    if (table->pager->num_pages != baseline_pages ||
        !root_is_full_one_level(table, &schema) ||
        payload_present(table, &schema, candidate_key) ||
        payload_count(table, &schema) != baseline_rows) {
        fprintf(stderr, "payload full-root rollback leaked topology/allocation\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!insert_candidate(table, &schema, candidate_key) ||
        !root_has_grown(table, &schema) ||
        !payload_present(table, &schema, candidate_key) ||
        payload_count(table, &schema) != baseline_rows + 1u) {
        fprintf(stderr, "committed payload full-root growth failed\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    uint32_t committed_pages = table->pager->num_pages;
    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL || table->root_page_num != 0u) {
        if (table != NULL) db_close(table);
        remove(argv[1]);
        return 1;
    }
    schema = wide_schema(table->root_page_num);
    if (table->pager->num_pages != committed_pages ||
        !root_has_grown(table, &schema) ||
        !payload_present(table, &schema, candidate_key) ||
        payload_count(table, &schema) != baseline_rows + 1u) {
        fprintf(stderr, "payload full-root growth did not survive reopen\n");
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    db_close(table);
    remove(argv[1]);
    printf("PAYLOAD_FULL_ROOT_SPLIT_OK row_size=308 root_page=0 rollback=1 commit=1 reopen=1 baseline_rows=%u candidate=%u\n",
           baseline_rows,
           candidate_key);
    return 0;
}
