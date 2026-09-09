#define main tinydb_height4_merge_borrow_probe_main
#include "v2_recursive_internal_merge_borrow_probe.c"
#undef main

#define H5_LEAF_COUNT 18u
#define H5_PARENT_COUNT 9u
#define H5_GRAND_COUNT 4u
#define H5_GREAT_COUNT 2u
#define H5_BASELINE_ROWS 18u

static bool h5_allocate_pages(Pager* pager,
                              uint32_t greats[H5_GREAT_COUNT],
                              uint32_t grands[H5_GRAND_COUNT],
                              uint32_t parents[H5_PARENT_COUNT],
                              uint32_t leaves[H5_LEAF_COUNT]) {
    for (uint32_t i = 0u; i < H5_GREAT_COUNT; i++) {
        greats[i] = get_unused_page_num(pager);
        if (greats[i] == 0u || greats[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, greats[i]);
    }
    for (uint32_t i = 0u; i < H5_GRAND_COUNT; i++) {
        grands[i] = get_unused_page_num(pager);
        if (grands[i] == 0u || grands[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, grands[i]);
    }
    for (uint32_t i = 0u; i < H5_PARENT_COUNT; i++) {
        parents[i] = get_unused_page_num(pager);
        if (parents[i] == 0u || parents[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, parents[i]);
    }
    for (uint32_t i = 0u; i < H5_LEAF_COUNT; i++) {
        leaves[i] = get_unused_page_num(pager);
        if (leaves[i] == 0u || leaves[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaves[i]);
    }
    return true;
}

static bool h5_seed_tree(TinyDB* db,
                         TableSchema* schema,
                         uint32_t greats[H5_GREAT_COUNT],
                         uint32_t grands[H5_GRAND_COUNT],
                         uint32_t parents[H5_PARENT_COUNT],
                         uint32_t leaves[H5_LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    if (!h5_allocate_pages(pager, greats, grands, parents, leaves)) return false;

    for (uint32_t i = 0u; i < H5_LEAF_COUNT; i++) {
        uint32_t key = 10u * (i + 1u);
        unsigned char* leaf = (unsigned char*)get_page(pager, leaves[i]);
        memset(leaf, 0, PAGE_SIZE);
        if (!tinydb_slotted_leaf_v2_init(leaf, PAGE_SIZE)) return false;
        leaf[IS_ROOT_OFFSET] = 0u;
        write_u32(leaf + PARENT_POINTER_OFFSET, parents[i / 2u]);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            i == 0u ? 0u : leaves[i - 1u]);
        tinydb_slotted_split_write_u32(
            leaf + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
            i + 1u == H5_LEAF_COUNT ? 0u : leaves[i + 1u]);
        if (!raw_insert(schema, leaf, key) ||
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaves[i]);
    }

    const uint32_t parent_grand[H5_PARENT_COUNT] = {
        0u, 0u, 1u, 1u, 1u, 2u, 2u, 3u, 3u
    };
    for (uint32_t i = 0u; i < H5_PARENT_COUNT; i++) {
        const uint32_t children[2] = {leaves[2u * i], leaves[2u * i + 1u]};
        const uint32_t separators[1] = {10u * (2u * i + 1u)};
        uint32_t grand = grands[parent_grand[i]];
        if (!build_internal(pager, parents[i], grand, false,
                            children, separators, 2u)) return false;
        mark_page_dirty(pager, parents[i]);
    }

    const uint32_t g0_children[2] = {parents[0], parents[1]};
    const uint32_t g0_keys[1] = {20u};
    const uint32_t g1_children[3] = {parents[2], parents[3], parents[4]};
    const uint32_t g1_keys[2] = {60u, 80u};
    const uint32_t g2_children[2] = {parents[5], parents[6]};
    const uint32_t g2_keys[1] = {120u};
    const uint32_t g3_children[2] = {parents[7], parents[8]};
    const uint32_t g3_keys[1] = {160u};
    if (!build_internal(pager, grands[0], greats[0], false,
                        g0_children, g0_keys, 2u) ||
        !build_internal(pager, grands[1], greats[0], false,
                        g1_children, g1_keys, 3u) ||
        !build_internal(pager, grands[2], greats[1], false,
                        g2_children, g2_keys, 2u) ||
        !build_internal(pager, grands[3], greats[1], false,
                        g3_children, g3_keys, 2u)) return false;
    for (uint32_t i = 0u; i < H5_GRAND_COUNT; i++) mark_page_dirty(pager, grands[i]);

    const uint32_t left_great_children[2] = {grands[0], grands[1]};
    const uint32_t left_great_keys[1] = {40u};
    const uint32_t right_great_children[2] = {grands[2], grands[3]};
    const uint32_t right_great_keys[1] = {140u};
    const uint32_t root_children[2] = {greats[0], greats[1]};
    const uint32_t root_keys[1] = {100u};
    if (!build_internal(pager, greats[0], schema->root_page_num, false,
                        left_great_children, left_great_keys, 2u) ||
        !build_internal(pager, greats[1], schema->root_page_num, false,
                        right_great_children, right_great_keys, 2u) ||
        !build_internal(pager, schema->root_page_num, 0u, true,
                        root_children, root_keys, 2u)) return false;
    mark_page_dirty(pager, greats[0]);
    mark_page_dirty(pager, greats[1]);
    mark_page_dirty(pager, schema->root_page_num);
    pager_commit(pager);
    return tinydb_record_scan(table, schema, NULL, NULL) == H5_BASELINE_ROWS &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool h5_original_state(Table* table,
                              TableSchema* schema,
                              const uint32_t greats[H5_GREAT_COUNT],
                              const uint32_t grands[H5_GRAND_COUNT],
                              const uint32_t parents[H5_PARENT_COUNT],
                              const uint32_t leaves[H5_LEAF_COUNT],
                              uint32_t free_before) {
    const uint32_t root_children[2] = {greats[0], greats[1]};
    const uint32_t root_keys[1] = {100u};
    const uint32_t left_great_children[2] = {grands[0], grands[1]};
    const uint32_t left_great_keys[1] = {40u};
    const uint32_t right_great_children[2] = {grands[2], grands[3]};
    const uint32_t right_great_keys[1] = {140u};
    const uint32_t g0_children[2] = {parents[0], parents[1]};
    const uint32_t g0_keys[1] = {20u};
    const uint32_t g1_children[3] = {parents[2], parents[3], parents[4]};
    const uint32_t g1_keys[2] = {60u, 80u};
    const uint32_t g2_children[2] = {parents[5], parents[6]};
    const uint32_t g2_keys[1] = {120u};
    const uint32_t g3_children[2] = {parents[7], parents[8]};
    const uint32_t g3_keys[1] = {160u};
    if (!internal_matches(table, schema->root_page_num, 0u, true,
                          root_children, root_keys, 2u) ||
        !internal_matches(table, greats[0], schema->root_page_num, false,
                          left_great_children, left_great_keys, 2u) ||
        !internal_matches(table, greats[1], schema->root_page_num, false,
                          right_great_children, right_great_keys, 2u) ||
        !internal_matches(table, grands[0], greats[0], false,
                          g0_children, g0_keys, 2u) ||
        !internal_matches(table, grands[1], greats[0], false,
                          g1_children, g1_keys, 3u) ||
        !internal_matches(table, grands[2], greats[1], false,
                          g2_children, g2_keys, 2u) ||
        !internal_matches(table, grands[3], greats[1], false,
                          g3_children, g3_keys, 2u) ||
        table->pager->free_page_count != free_before ||
        tinydb_record_scan(table, schema, NULL, NULL) != H5_BASELINE_ROWS ||
        !present(table, schema, 30u)) return false;

    for (uint32_t i = 0u; i < H5_LEAF_COUNT; i++) {
        uint32_t expected_prev = i == 0u ? 0u : leaves[i - 1u];
        uint32_t expected_next =
            i + 1u == H5_LEAF_COUNT ? 0u : leaves[i + 1u];
        uint32_t expected_parent = parents[i / 2u];
        if (!leaf_state(table, leaves[i], expected_parent,
                        expected_prev, expected_next) ||
            !present(table, schema, 10u * (i + 1u))) {
            return false;
        }
    }
    return true;
}

static bool h5_cascaded_state(Table* table,
                              TableSchema* schema,
                              const uint32_t greats[H5_GREAT_COUNT],
                              const uint32_t grands[H5_GRAND_COUNT],
                              const uint32_t parents[H5_PARENT_COUNT],
                              const uint32_t leaves[H5_LEAF_COUNT],
                              uint32_t free_before) {
    const uint32_t root_children[2] = {greats[0], greats[1]};
    const uint32_t root_keys[1] = {100u};
    const uint32_t left_great_children[2] = {grands[0], grands[1]};
    const uint32_t left_great_keys[1] = {60u};
    const uint32_t right_great_children[2] = {grands[2], grands[3]};
    const uint32_t right_great_keys[1] = {140u};
    const uint32_t left_grand_children[2] = {parents[1], parents[2]};
    const uint32_t left_grand_keys[1] = {40u};
    const uint32_t donor_grand_children[2] = {parents[3], parents[4]};
    const uint32_t donor_grand_keys[1] = {80u};
    const uint32_t kept_children[3] = {leaves[0], leaves[1], leaves[3]};
    const uint32_t kept_keys[2] = {10u, 20u};
    const uint32_t moved_children[2] = {leaves[4], leaves[5]};
    const uint32_t moved_keys[1] = {50u};
    const uint32_t control_g2_children[2] = {parents[5], parents[6]};
    const uint32_t control_g2_keys[1] = {120u};
    const uint32_t control_g3_children[2] = {parents[7], parents[8]};
    const uint32_t control_g3_keys[1] = {160u};

    if (!internal_matches(table, schema->root_page_num, 0u, true,
                          root_children, root_keys, 2u) ||
        !internal_matches(table, greats[0], schema->root_page_num, false,
                          left_great_children, left_great_keys, 2u) ||
        !internal_matches(table, greats[1], schema->root_page_num, false,
                          right_great_children, right_great_keys, 2u) ||
        !internal_matches(table, grands[0], greats[0], false,
                          left_grand_children, left_grand_keys, 2u) ||
        !internal_matches(table, grands[1], greats[0], false,
                          donor_grand_children, donor_grand_keys, 2u) ||
        !internal_matches(table, parents[1], grands[0], false,
                          kept_children, kept_keys, 3u) ||
        !internal_matches(table, parents[2], grands[0], false,
                          moved_children, moved_keys, 2u) ||
        !internal_matches(table, grands[2], greats[1], false,
                          control_g2_children, control_g2_keys, 2u) ||
        !internal_matches(table, grands[3], greats[1], false,
                          control_g3_children, control_g3_keys, 2u) ||
        table->pager->free_page_count != free_before + 2u ||
        !free_has(table->pager, leaves[2]) ||
        !free_has(table->pager, parents[0]) ||
        present(table, schema, 30u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != H5_BASELINE_ROWS - 1u) {
        return false;
    }

    if (!leaf_state(table, leaves[0], parents[1], 0u, leaves[1]) ||
        !leaf_state(table, leaves[1], parents[1], leaves[0], leaves[3]) ||
        !leaf_state(table, leaves[3], parents[1], leaves[1], leaves[4]) ||
        !leaf_state(table, leaves[4], parents[2], leaves[3], leaves[5]) ||
        !leaf_state(table, leaves[5], parents[2], leaves[4], leaves[6])) {
        return false;
    }
    for (uint32_t i = 6u; i < H5_LEAF_COUNT; i++) {
        uint32_t expected_parent = parents[i / 2u];
        uint32_t expected_prev = i == 6u ? leaves[5] : leaves[i - 1u];
        uint32_t expected_next =
            i + 1u == H5_LEAF_COUNT ? 0u : leaves[i + 1u];
        if (!leaf_state(table, leaves[i], expected_parent,
                        expected_prev, expected_next)) return false;
    }
    return true;
}

static bool run_height5_case(const char* path) {
    remove(path);
    TinyDB* db = tinydb_open(path);
    if (db == NULL ||
        !exec_ok(db, "CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return false;
    }
    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t greats[H5_GREAT_COUNT] = {0u};
    uint32_t grands[H5_GRAND_COUNT] = {0u};
    uint32_t parents[H5_PARENT_COUNT] = {0u};
    uint32_t leaves[H5_LEAF_COUNT] = {0u};
    if (schema == NULL || schema->row_size != 264u ||
        !h5_seed_tree(db, schema, greats, grands, parents, leaves)) {
        tinydb_close(db);
        return false;
    }

    uint32_t free_before = table->pager->free_page_count;
    uint32_t root_before = schema->root_page_num;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    if (!exec_ok(db, "BEGIN;") ||
        !tinydb_record_delete(table, schema, 30u, message, sizeof(message)) ||
        schema->root_page_num != root_before ||
        !h5_cascaded_state(table, schema, greats, grands, parents, leaves,
                           free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;") ||
        !h5_original_state(table, schema, greats, grands, parents, leaves,
                           free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "height-five rollback failed: %s\n", message);
        tinydb_close(db);
        return false;
    }

    message[0] = '\0';
    if (!tinydb_record_delete(table, schema, 30u, message, sizeof(message)) ||
        schema->root_page_num != root_before ||
        !h5_cascaded_state(table, schema, greats, grands, parents, leaves,
                           free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "height-five autocommit failed: %s\n", message);
        tinydb_close(db);
        return false;
    }
    tinydb_close(db);

    db = tinydb_open(path);
    if (db == NULL) return false;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    bool ok = schema != NULL && schema->root_page_num == root_before &&
              h5_cascaded_state(table, schema, greats, grands, parents, leaves,
                                free_before) &&
              exec_ok(db, "PRAGMA integrity_check;");
    tinydb_close(db);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    if (!run_height5_case(argv[1])) return EXIT_FAILURE;
    printf("V2_RECURSIVE_NONROOT_MERGE_BORROW_OK height5=yes deleted=30 "
           "nonroot_ancestor=yes local_separator=yes root_unchanged=yes "
           "control_subtree=yes bottom_merge=yes grand_borrow=yes "
           "rollback=yes allocator=yes wal=yes reopen=yes integrity=yes\n");
    return EXIT_SUCCESS;
}
