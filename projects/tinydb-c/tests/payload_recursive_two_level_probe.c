#include "record_payload.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_split.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FULL_CHILD_COUNT (INTERNAL_NODE_MAX_KEYS + 1u)
#define TARGET_EXTRA_PARENT_COUNT INTERNAL_NODE_MAX_KEYS
#define SIBLING_PARENT_COUNT 2u
#define MINIMAL_PARENT_COUNT \
    (TARGET_EXTRA_PARENT_COUNT + SIBLING_PARENT_COUNT)
#define TARGET_GRAND_LEAF_COUNT \
    (FULL_CHILD_COUNT + TARGET_EXTRA_PARENT_COUNT * 2u)
#define TOTAL_LEAF_COUNT \
    (TARGET_GRAND_LEAF_COUNT + SIBLING_PARENT_COUNT * 2u)
#define RANGE_STEP 100000u
#define TARGET_LEAF_INDEX (INTERNAL_NODE_MAX_KEYS / 2u)
#define LONG_TEXT_LENGTH 250u

static uint32_t read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static TableSchema wide_schema(uint32_t root_page_num) {
    TableSchema schema;
    memset(&schema, 0, sizeof(schema));
    snprintf(schema.name, sizeof(schema.name), "%s", "wide_recursive");
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

static void make_long_text(char text[TINYDB_RECORD_TEXT_MAX + 1u], char marker) {
    for (uint32_t i = 0u; i < LONG_TEXT_LENGTH; i++) {
        text[i] = (char)(marker + (char)(i % 3u));
    }
    text[LONG_TEXT_LENGTH] = '\0';
}

static bool make_payload(const TableSchema* schema,
                         uint32_t id,
                         const char* text,
                         TinyDBRecordPayload* payload) {
    TinyDBValue values[3];
    char message[TINYDB_RECORD_MESSAGE_MAX];
    memset(values, 0, sizeof(values));
    values[0].type = COL_TYPE_INT;
    values[0].int_value = id;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "%s", text);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = id + 777u;
    return tinydb_record_payload_encode_values(schema,
                                               values,
                                               3u,
                                               payload,
                                               message,
                                               sizeof(message));
}

static bool encode_envelope(const TableSchema* schema,
                            uint32_t id,
                            const char* text,
                            unsigned char envelope[PAGE_SIZE],
                            uint32_t* envelope_length) {
    TinyDBRecordPayload payload;
    return make_payload(schema, id, text, &payload) &&
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

static bool build_minimal_parent(Pager* pager,
                                 uint32_t parent_page_num,
                                 uint32_t grandparent_page_num,
                                 uint32_t left_leaf_page_num,
                                 uint32_t right_leaf_page_num,
                                 uint32_t left_max) {
    unsigned char* parent = (unsigned char*)get_page(pager, parent_page_num);
    initialize_internal(parent, grandparent_page_num, false, 1u);
    set_internal_cell(parent, 0u, left_leaf_page_num, left_max);
    write_u32(parent + INTERNAL_NODE_RIGHT_CHILD_OFFSET, right_leaf_page_num);
    mark_page_dirty(pager, parent_page_num);
    return true;
}

static bool seed_tree(Table* table,
                      TableSchema* schema,
                      uint32_t* candidate_key,
                      uint32_t* baseline_rows,
                      uint32_t* baseline_pages) {
    Pager* pager = table->pager;
    uint32_t root_page_num = get_unused_page_num(pager);
    if (root_page_num == 0u || root_page_num == INVALID_PAGE_NUM) return false;
    (void)get_page(pager, root_page_num);
    *schema = wide_schema(root_page_num);

    uint32_t full_parent_page_num = get_unused_page_num(pager);
    if (full_parent_page_num == 0u ||
        full_parent_page_num == INVALID_PAGE_NUM) {
        return false;
    }
    (void)get_page(pager, full_parent_page_num);

    uint32_t full_grandparent_page_num = get_unused_page_num(pager);
    if (full_grandparent_page_num == 0u ||
        full_grandparent_page_num == INVALID_PAGE_NUM) {
        return false;
    }
    (void)get_page(pager, full_grandparent_page_num);

    uint32_t sibling_grandparent_page_num = get_unused_page_num(pager);
    if (sibling_grandparent_page_num == 0u ||
        sibling_grandparent_page_num == INVALID_PAGE_NUM) {
        return false;
    }
    (void)get_page(pager, sibling_grandparent_page_num);

    uint32_t minimal_parents[MINIMAL_PARENT_COUNT];
    for (uint32_t i = 0u; i < MINIMAL_PARENT_COUNT; i++) {
        minimal_parents[i] = get_unused_page_num(pager);
        if (minimal_parents[i] == 0u ||
            minimal_parents[i] == INVALID_PAGE_NUM) {
            return false;
        }
        (void)get_page(pager, minimal_parents[i]);
    }

    uint32_t leaves[TOTAL_LEAF_COUNT];
    for (uint32_t i = 0u; i < TOTAL_LEAF_COUNT; i++) {
        leaves[i] = get_unused_page_num(pager);
        if (leaves[i] == 0u || leaves[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaves[i]);
    }

    char long_text[TINYDB_RECORD_TEXT_MAX + 1u];
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
            parent_page_num = full_parent_page_num;
        } else if (i < TARGET_GRAND_LEAF_COUNT) {
            uint32_t offset = i - FULL_CHILD_COUNT;
            parent_page_num = minimal_parents[offset / 2u];
        } else {
            uint32_t offset = i - TARGET_GRAND_LEAF_COUNT;
            parent_page_num =
                minimal_parents[TARGET_EXTRA_PARENT_COUNT + offset / 2u];
        }

        if (!initialize_leaf(pager,
                             leaves[i],
                             parent_page_num,
                             i == 0u ? 0u : leaves[i - 1u],
                             i + 1u == TOTAL_LEAF_COUNT ? 0u : leaves[i + 1u])) {
            return false;
        }
        unsigned char* leaf = (unsigned char*)get_page(pager, leaves[i]);
        uint32_t max_key = leaf_max_for_index(i);
        if (i == TARGET_LEAF_INDEX) {
            if (!raw_insert(schema, leaf, max_key, long_text)) return false;
            uint32_t filler = previous_max + 1000u;
            while (tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) >=
                   candidate_required) {
                if (filler >= max_key || !raw_insert(schema, leaf, filler, long_text)) {
                    return false;
                }
                filler += 1000u;
            }
            if (tinydb_slotted_leaf_v2_count(leaf, PAGE_SIZE) < 2u ||
                tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) >=
                    candidate_required) {
                return false;
            }
        } else if (!raw_insert(schema, leaf, max_key, "s")) {
            return false;
        }
        if (!tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaves[i]);
    }

    unsigned char* full_parent =
        (unsigned char*)get_page(pager, full_parent_page_num);
    initialize_internal(full_parent,
                        full_grandparent_page_num,
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
    mark_page_dirty(pager, full_parent_page_num);

    for (uint32_t i = 0u; i < MINIMAL_PARENT_COUNT; i++) {
        uint32_t leaf_offset = FULL_CHILD_COUNT + i * 2u;
        uint32_t grandparent = i < TARGET_EXTRA_PARENT_COUNT
            ? full_grandparent_page_num
            : sibling_grandparent_page_num;
        if (!build_minimal_parent(pager,
                                  minimal_parents[i],
                                  grandparent,
                                  leaves[leaf_offset],
                                  leaves[leaf_offset + 1u],
                                  leaf_max_for_index(leaf_offset))) {
            return false;
        }
    }

    unsigned char* full_grandparent =
        (unsigned char*)get_page(pager, full_grandparent_page_num);
    initialize_internal(full_grandparent,
                        root_page_num,
                        false,
                        INTERNAL_NODE_MAX_KEYS);
    set_internal_cell(full_grandparent,
                      0u,
                      full_parent_page_num,
                      leaf_max_for_index(FULL_CHILD_COUNT - 1u));
    for (uint32_t i = 1u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        uint32_t parent_index = i - 1u;
        uint32_t leaf_index = FULL_CHILD_COUNT + parent_index * 2u + 1u;
        set_internal_cell(full_grandparent,
                          i,
                          minimal_parents[parent_index],
                          leaf_max_for_index(leaf_index));
    }
    write_u32(full_grandparent + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              minimal_parents[TARGET_EXTRA_PARENT_COUNT - 1u]);
    mark_page_dirty(pager, full_grandparent_page_num);

    unsigned char* sibling_grandparent =
        (unsigned char*)get_page(pager, sibling_grandparent_page_num);
    initialize_internal(sibling_grandparent, root_page_num, false, 1u);
    uint32_t sibling_left_parent = minimal_parents[TARGET_EXTRA_PARENT_COUNT];
    uint32_t sibling_right_parent =
        minimal_parents[TARGET_EXTRA_PARENT_COUNT + 1u];
    uint32_t sibling_left_last_leaf = TARGET_GRAND_LEAF_COUNT + 1u;
    set_internal_cell(sibling_grandparent,
                      0u,
                      sibling_left_parent,
                      leaf_max_for_index(sibling_left_last_leaf));
    write_u32(sibling_grandparent + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              sibling_right_parent);
    mark_page_dirty(pager, sibling_grandparent_page_num);

    unsigned char* root = (unsigned char*)get_page(pager, root_page_num);
    initialize_internal(root, 0u, true, 1u);
    set_internal_cell(root,
                      0u,
                      full_grandparent_page_num,
                      leaf_max_for_index(TARGET_GRAND_LEAF_COUNT - 1u));
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              sibling_grandparent_page_num);
    mark_page_dirty(pager, root_page_num);

    pager_commit(pager);
    bool complete = false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    *baseline_rows = tinydb_record_payload_scan(table,
                                                schema,
                                                NULL,
                                                NULL,
                                                &complete,
                                                message,
                                                sizeof(message));
    *baseline_pages = pager->num_pages;
    unsigned char* root_after_scan =
        (unsigned char*)get_page(pager, root_page_num);
    return complete && *baseline_rows != UINT32_MAX &&
           root_after_scan != NULL &&
           read_u32(root_after_scan + INTERNAL_NODE_NUM_KEYS_OFFSET) == 1u;
}

static bool candidate_present(Table* table,
                              const TableSchema* schema,
                              uint32_t candidate_key) {
    TinyDBRecordPayload payload;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_find(table,
                                      schema,
                                      candidate_key,
                                      &payload,
                                      message,
                                      sizeof(message));
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
    return complete ? count : UINT32_MAX;
}

static bool insert_candidate(Table* table,
                             const TableSchema* schema,
                             uint32_t candidate_key,
                             char* message,
                             size_t message_size) {
    char long_text[TINYDB_RECORD_TEXT_MAX + 1u];
    TinyDBRecordPayload payload;
    make_long_text(long_text, 'x');
    return make_payload(schema, candidate_key, long_text, &payload) &&
           tinydb_record_payload_insert(table,
                                        schema,
                                        &payload,
                                        message,
                                        message_size);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_recursive_two_level_probe <db>\n");
        return EXIT_FAILURE;
    }
    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return EXIT_FAILURE;

    TableSchema schema;
    uint32_t candidate_key = 0u;
    uint32_t baseline_rows = 0u;
    uint32_t baseline_pages = 0u;
    if (!seed_tree(table,
                   &schema,
                   &candidate_key,
                   &baseline_rows,
                   &baseline_pages) ||
        schema.row_size != 308u || candidate_present(table, &schema, candidate_key)) {
        fprintf(stderr, "unable to seed recursive wide-payload tree\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    if (!insert_candidate(table,
                          &schema,
                          candidate_key,
                          message,
                          sizeof(message)) ||
        read_u32((unsigned char*)get_page(table->pager, schema.root_page_num) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        table->pager->num_pages != baseline_pages + 3u ||
        payload_count(table, &schema) != baseline_rows + 1u ||
        !candidate_present(table, &schema, candidate_key)) {
        fprintf(stderr, "transactional recursive payload split failed: %s\n", message);
        table->in_transaction = false;
        pager_rollback(table->pager);
        db_close(table);
        return EXIT_FAILURE;
    }
    pager_rollback(table->pager);
    table->in_transaction = false;

    if (table->pager->num_pages != baseline_pages ||
        read_u32((unsigned char*)get_page(table->pager, schema.root_page_num) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        payload_count(table, &schema) != baseline_rows ||
        candidate_present(table, &schema, candidate_key)) {
        fprintf(stderr, "recursive payload rollback did not restore topology\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    if (!insert_candidate(table,
                          &schema,
                          candidate_key,
                          message,
                          sizeof(message)) ||
        table->pager->num_pages != baseline_pages + 3u ||
        read_u32((unsigned char*)get_page(table->pager, schema.root_page_num) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        payload_count(table, &schema) != baseline_rows + 1u ||
        !candidate_present(table, &schema, candidate_key)) {
        fprintf(stderr, "autocommit recursive payload split failed: %s\n", message);
        db_close(table);
        return EXIT_FAILURE;
    }

    uint32_t root_page_num = schema.root_page_num;
    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return EXIT_FAILURE;
    schema = wide_schema(root_page_num);
    if (read_u32((unsigned char*)get_page(table->pager, root_page_num) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        payload_count(table, &schema) != baseline_rows + 1u ||
        !candidate_present(table, &schema, candidate_key)) {
        fprintf(stderr, "recursive payload split did not survive reopen\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    printf("PAYLOAD_RECURSIVE_TWO_LEVEL_OK row_size=%u pages=3 rollback=1 commit=1 reopen=1\n",
           schema.row_size);
    db_close(table);
    return EXIT_SUCCESS;
}
