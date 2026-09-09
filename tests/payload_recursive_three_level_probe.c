#include "record_payload.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_split.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FULL_CHILD_COUNT (INTERNAL_NODE_MAX_KEYS + 1u)
#define DUMMY_CHILD_COUNT INTERNAL_NODE_MAX_KEYS
#define PREVIOUS_MAX 1000000u
#define TARGET_MAX 2000000u
#define NEXT_MIN 3000000u
#define CANDIDATE_KEY (PREVIOUS_MAX + 1u)
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
    snprintf(schema.name, sizeof(schema.name), "%s", "wide_recursive_three");
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

static uint32_t allocate_page(Pager* pager) {
    uint32_t page_num = get_unused_page_num(pager);
    if (page_num == INVALID_PAGE_NUM) return INVALID_PAGE_NUM;
    if (get_page(pager, page_num) == NULL) return INVALID_PAGE_NUM;
    return page_num;
}

static bool initialize_dummy(Pager* pager,
                             uint32_t page_num,
                             uint32_t parent_page_num) {
    unsigned char* page = (unsigned char*)get_page(pager, page_num);
    if (page == NULL) return false;
    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    mark_page_dirty(pager, page_num);
    return true;
}

static bool initialize_leaf(Pager* pager,
                            uint32_t page_num,
                            uint32_t parent_page_num,
                            uint32_t previous_page_num,
                            uint32_t next_page_num) {
    unsigned char* page = (unsigned char*)get_page(pager, page_num);
    if (page == NULL) return false;
    memset(page, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    tinydb_slotted_split_write_u32(
        page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
        previous_page_num);
    tinydb_slotted_split_write_u32(
        page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
        next_page_num);
    mark_page_dirty(pager, page_num);
    return true;
}

static bool fill_target_leaf(const TableSchema* schema,
                             unsigned char* leaf,
                             uint32_t required) {
    char long_text[TINYDB_RECORD_TEXT_MAX + 1u];
    make_long_text(long_text, 'a');
    if (!raw_insert(schema, leaf, TARGET_MAX, long_text)) return false;

    uint32_t filler = PREVIOUS_MAX + 10000u;
    while (tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) >= required) {
        if (filler >= TARGET_MAX || filler == CANDIDATE_KEY ||
            !raw_insert(schema, leaf, filler, long_text)) {
            return false;
        }
        filler += 10000u;
    }
    return tinydb_slotted_leaf_v2_count(leaf, PAGE_SIZE) >= 2u &&
           tinydb_slotted_leaf_v2_free_bytes(leaf, PAGE_SIZE) < required;
}

static bool seed_tree(Table* table,
                      TableSchema* schema,
                      uint32_t* baseline_pages) {
    Pager* pager = table->pager;
    const uint32_t root_page_num = table->root_page_num;
    if (root_page_num != 0u) return false;
    *schema = wide_schema(root_page_num);

    uint32_t parent0 = allocate_page(pager);
    uint32_t parent1 = allocate_page(pager);
    uint32_t parent2 = allocate_page(pager);
    if (parent0 == INVALID_PAGE_NUM || parent1 == INVALID_PAGE_NUM ||
        parent2 == INVALID_PAGE_NUM) {
        return false;
    }

    uint32_t parent0_children[FULL_CHILD_COUNT];
    for (uint32_t i = 0u; i < FULL_CHILD_COUNT; i++) {
        parent0_children[i] = allocate_page(pager);
        if (parent0_children[i] == INVALID_PAGE_NUM) return false;
    }
    uint32_t previous_leaf = parent0_children[INTERNAL_NODE_MAX_KEYS - 1u];
    uint32_t target_leaf = parent0_children[INTERNAL_NODE_MAX_KEYS];
    uint32_t next_leaf = allocate_page(pager);
    if (next_leaf == INVALID_PAGE_NUM) return false;

    uint32_t parent1_dummies[DUMMY_CHILD_COUNT];
    uint32_t parent2_dummies[DUMMY_CHILD_COUNT];
    for (uint32_t i = 0u; i < DUMMY_CHILD_COUNT; i++) {
        parent1_dummies[i] = allocate_page(pager);
        parent2_dummies[i] = allocate_page(pager);
        if (parent1_dummies[i] == INVALID_PAGE_NUM ||
            parent2_dummies[i] == INVALID_PAGE_NUM) {
            return false;
        }
    }
    uint32_t root_control = allocate_page(pager);
    if (root_control == INVALID_PAGE_NUM) return false;

    for (uint32_t i = 0u; i + 2u < FULL_CHILD_COUNT; i++) {
        if (!initialize_dummy(pager, parent0_children[i], parent0)) return false;
    }
    if (!initialize_leaf(pager,
                         previous_leaf,
                         parent0,
                         0u,
                         target_leaf) ||
        !initialize_leaf(pager,
                         target_leaf,
                         parent0,
                         previous_leaf,
                         next_leaf) ||
        !initialize_leaf(pager,
                         next_leaf,
                         root_control,
                         target_leaf,
                         0u)) {
        return false;
    }

    unsigned char* previous =
        (unsigned char*)get_page(pager, previous_leaf);
    unsigned char* target =
        (unsigned char*)get_page(pager, target_leaf);
    unsigned char* next =
        (unsigned char*)get_page(pager, next_leaf);
    if (!raw_insert(schema, previous, PREVIOUS_MAX, "previous") ||
        !raw_insert(schema, next, NEXT_MIN, "next")) {
        return false;
    }

    char long_text[TINYDB_RECORD_TEXT_MAX + 1u];
    unsigned char candidate_envelope[PAGE_SIZE];
    uint32_t candidate_length = 0u;
    make_long_text(long_text, 'x');
    if (!encode_envelope(schema,
                         CANDIDATE_KEY,
                         long_text,
                         candidate_envelope,
                         &candidate_length) ||
        !fill_target_leaf(schema,
                          target,
                          TINYDB_SLOTTED_V2_SLOT_SIZE + candidate_length) ||
        !tinydb_slotted_leaf_v2_validate(previous, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(target, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(next, PAGE_SIZE)) {
        return false;
    }
    mark_page_dirty(pager, previous_leaf);
    mark_page_dirty(pager, target_leaf);
    mark_page_dirty(pager, next_leaf);

    unsigned char* p0 = (unsigned char*)get_page(pager, parent0);
    initialize_internal(p0, parent1, false, INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        uint32_t separator = (i + 1u) * 1000u;
        if (i + 1u == INTERNAL_NODE_MAX_KEYS) separator = PREVIOUS_MAX;
        set_internal_cell(p0, i, parent0_children[i], separator);
    }
    write_u32(p0 + INTERNAL_NODE_RIGHT_CHILD_OFFSET, target_leaf);
    mark_page_dirty(pager, parent0);

    for (uint32_t i = 0u; i < DUMMY_CHILD_COUNT; i++) {
        if (!initialize_dummy(pager, parent1_dummies[i], parent1) ||
            !initialize_dummy(pager, parent2_dummies[i], parent2)) {
            return false;
        }
    }

    unsigned char* p1 = (unsigned char*)get_page(pager, parent1);
    initialize_internal(p1, parent2, false, INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        set_internal_cell(p1,
                          i,
                          parent1_dummies[i],
                          (i + 1u) * 100u);
    }
    write_u32(p1 + INTERNAL_NODE_RIGHT_CHILD_OFFSET, parent0);
    mark_page_dirty(pager, parent1);

    unsigned char* p2 = (unsigned char*)get_page(pager, parent2);
    initialize_internal(p2, root_page_num, false, INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        set_internal_cell(p2,
                          i,
                          parent2_dummies[i],
                          (i + 1u) * 10u);
    }
    write_u32(p2 + INTERNAL_NODE_RIGHT_CHILD_OFFSET, parent1);
    mark_page_dirty(pager, parent2);

    if (!initialize_dummy(pager, root_control, root_page_num)) return false;
    unsigned char* root = (unsigned char*)get_page(pager, root_page_num);
    initialize_internal(root, 0u, true, 1u);
    set_internal_cell(root, 0u, parent2, TARGET_MAX);
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET, root_control);
    mark_page_dirty(pager, root_page_num);

    pager_commit(pager);
    *baseline_pages = pager->num_pages;
    return read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) == 1u;
}

static bool candidate_present(Table* table, const TableSchema* schema) {
    TinyDBRecordPayload payload;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_find(table,
                                      schema,
                                      CANDIDATE_KEY,
                                      &payload,
                                      message,
                                      sizeof(message));
}

static bool insert_candidate(Table* table,
                             const TableSchema* schema,
                             char* message,
                             size_t message_size) {
    char long_text[TINYDB_RECORD_TEXT_MAX + 1u];
    TinyDBRecordPayload payload;
    make_long_text(long_text, 'x');
    return make_payload(schema, CANDIDATE_KEY, long_text, &payload) &&
           tinydb_record_payload_insert(table,
                                        schema,
                                        &payload,
                                        message,
                                        message_size);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_recursive_three_level_probe <db>\n");
        return EXIT_FAILURE;
    }
    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return EXIT_FAILURE;

    TableSchema schema;
    uint32_t baseline_pages = 0u;
    if (!seed_tree(table, &schema, &baseline_pages) ||
        schema.row_size != 308u || candidate_present(table, &schema)) {
        fprintf(stderr, "unable to seed three-level recursive payload tree\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    if (!insert_candidate(table, &schema, message, sizeof(message)) ||
        table->pager->num_pages != baseline_pages + 4u ||
        read_u32((unsigned char*)get_page(table->pager, schema.root_page_num) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        !candidate_present(table, &schema)) {
        fprintf(stderr, "transactional three-level payload cascade failed: %s\n", message);
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
        candidate_present(table, &schema)) {
        fprintf(stderr, "three-level payload rollback did not restore topology\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    if (!insert_candidate(table, &schema, message, sizeof(message)) ||
        table->pager->num_pages != baseline_pages + 4u ||
        read_u32((unsigned char*)get_page(table->pager, schema.root_page_num) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        !candidate_present(table, &schema)) {
        fprintf(stderr, "autocommit three-level payload cascade failed: %s\n", message);
        db_close(table);
        return EXIT_FAILURE;
    }

    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return EXIT_FAILURE;
    schema = wide_schema(0u);
    if (table->pager->num_pages != baseline_pages + 4u ||
        read_u32((unsigned char*)get_page(table->pager, schema.root_page_num) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        !candidate_present(table, &schema)) {
        fprintf(stderr, "three-level payload cascade did not survive reopen\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    printf("PAYLOAD_RECURSIVE_THREE_LEVEL_OK row_size=%u levels=3 pages=4 root0=1 rollback=1 commit=1 reopen=1\n",
           schema.row_size);
    db_close(table);
    return EXIT_SUCCESS;
}
