#include "slotted_leaf_v2_split.h"

#define main payload_full_root_original_main
#include "payload_full_root_v2_split_probe.c"
#undef main

static void dump_root_state(Table* table, const TableSchema* schema) {
    unsigned char* root =
        (unsigned char*)get_page(table->pager, schema->root_page_num);
    uint32_t key_count = get_node_type(root) == NODE_INTERNAL
        ? read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET)
        : 0u;
    uint32_t left_page = key_count > 0u
        ? read_u32(root + INTERNAL_NODE_HEADER_SIZE)
        : INVALID_PAGE_NUM;
    uint32_t right_page = get_node_type(root) == NODE_INTERNAL
        ? read_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET)
        : INVALID_PAGE_NUM;
    int left_type = left_page < table->pager->num_pages
        ? (int)get_node_type(get_page(table->pager, left_page))
        : -1;
    int right_type = right_page < table->pager->num_pages
        ? (int)get_node_type(get_page(table->pager, right_page))
        : -1;
    fprintf(stderr,
            "root_state type=%d root=%u keys=%u left=%u left_type=%d right=%u right_type=%d pages=%u\n",
            (int)get_node_type(root),
            schema->root_page_num,
            key_count,
            left_page,
            left_type,
            right_page,
            right_type,
            table->pager->num_pages);
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

    uint32_t candidate_key = 0u;
    uint32_t baseline_rows = 0u;
    if (!seed_full_root(table,
                        &schema,
                        &candidate_key,
                        &baseline_rows) ||
        !root_is_full_one_level(table, &schema) ||
        payload_present(table, &schema, candidate_key)) {
        fprintf(stderr, "unable to seed full page-zero payload root\n");
        dump_root_state(table, &schema);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    uint32_t baseline_pages = table->pager->num_pages;
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    bool inserted = insert_candidate(table, &schema, candidate_key);
    bool grown = root_has_grown(table, &schema);
    bool present = payload_present(table, &schema, candidate_key);
    uint32_t transactional_rows = payload_count(table, &schema);
    if (!inserted || !grown || !present ||
        transactional_rows != baseline_rows + 1u) {
        fprintf(stderr,
                "transactional payload full-root growth failed inserted=%d grown=%d present=%d rows=%u expected=%u\n",
                inserted ? 1 : 0,
                grown ? 1 : 0,
                present ? 1 : 0,
                transactional_rows,
                baseline_rows + 1u);
        dump_root_state(table, &schema);
        table->in_transaction = false;
        pager_rollback(table->pager);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    pager_rollback(table->pager);
    table->in_transaction = false;
    uint32_t rollback_rows = payload_count(table, &schema);
    if (table->pager->num_pages != baseline_pages ||
        !root_is_full_one_level(table, &schema) ||
        payload_present(table, &schema, candidate_key) ||
        rollback_rows != baseline_rows) {
        fprintf(stderr,
                "payload full-root rollback leaked topology/allocation pages=%u expected_pages=%u rows=%u expected_rows=%u\n",
                table->pager->num_pages,
                baseline_pages,
                rollback_rows,
                baseline_rows);
        dump_root_state(table, &schema);
        db_close(table);
        remove(argv[1]);
        return 1;
    }

    if (!insert_candidate(table, &schema, candidate_key) ||
        !root_has_grown(table, &schema) ||
        !payload_present(table, &schema, candidate_key) ||
        payload_count(table, &schema) != baseline_rows + 1u) {
        fprintf(stderr, "committed payload full-root growth failed\n");
        dump_root_state(table, &schema);
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
        dump_root_state(table, &schema);
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
