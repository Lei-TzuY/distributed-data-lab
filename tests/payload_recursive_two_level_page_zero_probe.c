#define main payload_recursive_three_level_fixture_main
#include "payload_recursive_three_level_probe.c"
#undef main

static bool seed_two_level_page_zero(Table* table,
                                     TableSchema* schema,
                                     uint32_t* baseline_pages) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        baseline_pages == NULL || table->root_page_num != 0u) {
        return false;
    }

    Pager* pager = table->pager;
    const uint32_t root_page_num = table->root_page_num;
    *schema = wide_schema(root_page_num);

    uint32_t parent0 = allocate_page(pager);
    uint32_t parent1 = allocate_page(pager);
    if (parent0 == INVALID_PAGE_NUM || parent1 == INVALID_PAGE_NUM) {
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
    for (uint32_t i = 0u; i < DUMMY_CHILD_COUNT; i++) {
        parent1_dummies[i] = allocate_page(pager);
        if (parent1_dummies[i] == INVALID_PAGE_NUM) return false;
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
        if (!initialize_dummy(pager, parent1_dummies[i], parent1)) return false;
    }

    unsigned char* p1 = (unsigned char*)get_page(pager, parent1);
    initialize_internal(p1, root_page_num, false, INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        set_internal_cell(p1,
                          i,
                          parent1_dummies[i],
                          (i + 1u) * 100u);
    }
    write_u32(p1 + INTERNAL_NODE_RIGHT_CHILD_OFFSET, parent0);
    mark_page_dirty(pager, parent1);

    if (!initialize_dummy(pager, root_control, root_page_num)) return false;
    unsigned char* root = (unsigned char*)get_page(pager, root_page_num);
    initialize_internal(root, 0u, true, 1u);
    set_internal_cell(root, 0u, parent1, TARGET_MAX);
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET, root_control);
    mark_page_dirty(pager, root_page_num);

    pager_commit(pager);
    *baseline_pages = pager->num_pages;
    root = (unsigned char*)get_page(pager, root_page_num);
    return root != NULL && get_node_type(root) == NODE_INTERNAL &&
           root[IS_ROOT_OFFSET] != 0u &&
           read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) == 1u &&
           !candidate_present(table, schema);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr,
                "usage: payload_recursive_two_level_page_zero_probe <db>\n");
        return EXIT_FAILURE;
    }
    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return EXIT_FAILURE;

    TableSchema schema;
    uint32_t baseline_pages = 0u;
    if (!seed_two_level_page_zero(table, &schema, &baseline_pages) ||
        schema.row_size != 308u || schema.root_page_num != 0u) {
        fprintf(stderr, "unable to seed page-zero two-level payload tree\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    if (!insert_candidate(table, &schema, message, sizeof(message)) ||
        table->pager->num_pages != baseline_pages + 3u ||
        read_u32((unsigned char*)get_page(table->pager, 0u) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        !candidate_present(table, &schema)) {
        fprintf(stderr,
                "transactional page-zero two-level cascade failed: %s\n",
                message);
        table->in_transaction = false;
        pager_rollback(table->pager);
        db_close(table);
        return EXIT_FAILURE;
    }
    pager_rollback(table->pager);
    table->in_transaction = false;

    if (table->pager->num_pages != baseline_pages ||
        read_u32((unsigned char*)get_page(table->pager, 0u) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        candidate_present(table, &schema)) {
        fprintf(stderr,
                "page-zero two-level rollback did not restore topology\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    if (!insert_candidate(table, &schema, message, sizeof(message)) ||
        table->pager->num_pages != baseline_pages + 3u ||
        read_u32((unsigned char*)get_page(table->pager, 0u) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        !candidate_present(table, &schema)) {
        fprintf(stderr,
                "autocommit page-zero two-level cascade failed: %s\n",
                message);
        db_close(table);
        return EXIT_FAILURE;
    }

    db_close(table);
    table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return EXIT_FAILURE;
    schema = wide_schema(0u);
    if (read_u32((unsigned char*)get_page(table->pager, 0u) +
                 INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        !candidate_present(table, &schema)) {
        fprintf(stderr,
                "page-zero two-level cascade did not survive reopen\n");
        db_close(table);
        return EXIT_FAILURE;
    }

    printf("PAYLOAD_RECURSIVE_TWO_LEVEL_PAGE_ZERO_OK row_size=%u levels=2 pages=3 root0=1 rollback=1 commit=1 reopen=1\n",
           schema.row_size);
    db_close(table);
    return EXIT_SUCCESS;
}
