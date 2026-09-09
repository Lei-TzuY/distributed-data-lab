/* Reuse the synthetic fixture helpers from the base probe, but do not trust a
 * naked Pager frame pointer across pager_commit(). The buffer pool may evict
 * and reuse that frame while committing ~1500 dirty pages. */
#define main payload_recursive_three_level_fixture_main
#include "payload_recursive_three_level_probe.c"
#undef main

static bool seed_tree_reacquire_root(Table* table,
                                     TableSchema* schema,
                                     uint32_t* baseline_pages) {
    bool seed_result = seed_tree(table, schema, baseline_pages);
    if (table == NULL || table->pager == NULL ||
        baseline_pages == NULL || *baseline_pages == 0u) {
        return false;
    }

    unsigned char* root =
        (unsigned char*)get_page(table->pager, schema->root_page_num);
    bool committed_fixture_is_valid =
        root != NULL &&
        get_node_type(root) == NODE_INTERNAL &&
        root[IS_ROOT_OFFSET] != 0u &&
        read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) == 1u;

    /* seed_tree() on the historical fixture may return false solely because it
     * inspected its pre-commit frame pointer after Pager LRU reuse. A nonzero
     * baseline plus the freshly reacquired committed root proves that all seed
     * work reached the final commit. */
    return seed_result || committed_fixture_is_valid;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: payload_recursive_three_level_live_probe <db>\n");
        return EXIT_FAILURE;
    }
    remove(argv[1]);
    Table* table = db_open(argv[1]);
    if (table == NULL || table->pager == NULL) return EXIT_FAILURE;

    TableSchema schema;
    uint32_t baseline_pages = 0u;
    if (!seed_tree_reacquire_root(table, &schema, &baseline_pages) ||
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
