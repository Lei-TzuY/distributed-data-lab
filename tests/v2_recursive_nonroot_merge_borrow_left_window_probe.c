#define main tinydb_height4_merge_borrow_window_probe_main
#include "v2_recursive_internal_merge_borrow_probe.c"
#undef main

#define H5W_LEAF_COUNT 26u
#define H5W_PARENT_COUNT 13u
#define H5W_GRAND_COUNT 6u
#define H5W_GREAT_COUNT 2u
#define H5W_BASELINE_ROWS 26u

static bool h5w_allocate_pages(Pager* pager,
                               uint32_t greats[H5W_GREAT_COUNT],
                               uint32_t grands[H5W_GRAND_COUNT],
                               uint32_t parents[H5W_PARENT_COUNT],
                               uint32_t leaves[H5W_LEAF_COUNT]) {
    for (uint32_t i = 0u; i < H5W_GREAT_COUNT; i++) {
        greats[i] = get_unused_page_num(pager);
        if (greats[i] == 0u || greats[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, greats[i]);
    }
    for (uint32_t i = 0u; i < H5W_GRAND_COUNT; i++) {
        grands[i] = get_unused_page_num(pager);
        if (grands[i] == 0u || grands[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, grands[i]);
    }
    for (uint32_t i = 0u; i < H5W_PARENT_COUNT; i++) {
        parents[i] = get_unused_page_num(pager);
        if (parents[i] == 0u || parents[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, parents[i]);
    }
    for (uint32_t i = 0u; i < H5W_LEAF_COUNT; i++) {
        leaves[i] = get_unused_page_num(pager);
        if (leaves[i] == 0u || leaves[i] == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, leaves[i]);
    }
    return true;
}

static bool h5w_seed_tree(TinyDB* db,
                          TableSchema* schema,
                          uint32_t greats[H5W_GREAT_COUNT],
                          uint32_t grands[H5W_GRAND_COUNT],
                          uint32_t parents[H5W_PARENT_COUNT],
                          uint32_t leaves[H5W_LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table->pager;
    if (!h5w_allocate_pages(pager, greats, grands, parents, leaves)) return false;

    for (uint32_t i = 0u; i < H5W_LEAF_COUNT; i++) {
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
            i + 1u == H5W_LEAF_COUNT ? 0u : leaves[i + 1u]);
        if (!raw_insert(schema, leaf, key) ||
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) return false;
        mark_page_dirty(pager, leaves[i]);
    }

    const uint32_t parent_grand[H5W_PARENT_COUNT] = {
        0u, 0u,
        1u, 1u, 1u,
        2u, 2u,
        3u, 3u,
        4u, 4u,
        5u, 5u
    };
    for (uint32_t i = 0u; i < H5W_PARENT_COUNT; i++) {
        const uint32_t children[2] = {leaves[2u * i], leaves[2u * i + 1u]};
        const uint32_t separators[1] = {10u * (2u * i + 1u)};
        if (!build_internal(pager, parents[i], grands[parent_grand[i]], false,
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
    const uint32_t g4_children[2] = {parents[9], parents[10]};
    const uint32_t g4_keys[1] = {200u};
    const uint32_t g5_children[2] = {parents[11], parents[12]};
    const uint32_t g5_keys[1] = {240u};
    if (!build_internal(pager, grands[0], greats[0], false,
                        g0_children, g0_keys, 2u) ||
        !build_internal(pager, grands[1], greats[0], false,
                        g1_children, g1_keys, 3u) ||
        !build_internal(pager, grands[2], greats[0], false,
                        g2_children, g2_keys, 2u) ||
        !build_internal(pager, grands[3], greats[0], false,
                        g3_children, g3_keys, 2u) ||
        !build_internal(pager, grands[4], greats[1], false,
                        g4_children, g4_keys, 2u) ||
        !build_internal(pager, grands[5], greats[1], false,
                        g5_children, g5_keys, 2u)) return false;
    for (uint32_t i = 0u; i < H5W_GRAND_COUNT; i++) mark_page_dirty(pager, grands[i]);

    const uint32_t affected_children[4] = {
        grands[0], grands[1], grands[2], grands[3]
    };
    const uint32_t affected_keys[3] = {40u, 100u, 140u};
    const uint32_t control_children[2] = {grands[4], grands[5]};
    const uint32_t control_keys[1] = {220u};
    const uint32_t root_children[2] = {greats[0], greats[1]};
    const uint32_t root_keys[1] = {180u};
    if (!build_internal(pager, greats[0], schema->root_page_num, false,
                        affected_children, affected_keys, 4u) ||
        !build_internal(pager, greats[1], schema->root_page_num, false,
                        control_children, control_keys, 2u) ||
        !build_internal(pager, schema->root_page_num, 0u, true,
                        root_children, root_keys, 2u)) return false;
    mark_page_dirty(pager, greats[0]);
    mark_page_dirty(pager, greats[1]);
    mark_page_dirty(pager, schema->root_page_num);
    pager_commit(pager);
    return tinydb_record_scan(table, schema, NULL, NULL) == H5W_BASELINE_ROWS &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static bool h5w_original_state(Table* table,
                               TableSchema* schema,
                               const uint32_t greats[H5W_GREAT_COUNT],
                               const uint32_t grands[H5W_GRAND_COUNT],
                               const uint32_t parents[H5W_PARENT_COUNT],
                               const uint32_t leaves[H5W_LEAF_COUNT],
                               uint32_t free_before) {
    const uint32_t root_children[2] = {greats[0], greats[1]};
    const uint32_t root_keys[1] = {180u};
    const uint32_t affected_children[4] = {grands[0], grands[1], grands[2], grands[3]};
    const uint32_t affected_keys[3] = {40u, 100u, 140u};
    const uint32_t control_children[2] = {grands[4], grands[5]};
    const uint32_t control_keys[1] = {220u};
    const uint32_t grand_child_counts[H5W_GRAND_COUNT] = {2u, 3u, 2u, 2u, 2u, 2u};
    const uint32_t grand_parent_offsets[H5W_GRAND_COUNT] = {0u, 2u, 5u, 7u, 9u, 11u};
    const uint32_t grand_key_sets[H5W_GRAND_COUNT][2] = {
        {20u, 0u}, {60u, 80u}, {120u, 0u},
        {160u, 0u}, {200u, 0u}, {240u, 0u}
    };

    if (!internal_matches(table, schema->root_page_num, 0u, true,
                          root_children, root_keys, 2u) ||
        !internal_matches(table, greats[0], schema->root_page_num, false,
                          affected_children, affected_keys, 4u) ||
        !internal_matches(table, greats[1], schema->root_page_num, false,
                          control_children, control_keys, 2u) ||
        table->pager->free_page_count != free_before ||
        tinydb_record_scan(table, schema, NULL, NULL) != H5W_BASELINE_ROWS ||
        !present(table, schema, 120u)) return false;

    for (uint32_t g = 0u; g < H5W_GRAND_COUNT; g++) {
        uint32_t count = grand_child_counts[g];
        uint32_t children[3] = {0u, 0u, 0u};
        for (uint32_t i = 0u; i < count; i++) {
            children[i] = parents[grand_parent_offsets[g] + i];
        }
        uint32_t great = g < 4u ? greats[0] : greats[1];
        if (!internal_matches(table, grands[g], great, false,
                              children, grand_key_sets[g], count)) return false;
    }

    for (uint32_t i = 0u; i < H5W_LEAF_COUNT; i++) {
        uint32_t expected_prev = i == 0u ? 0u : leaves[i - 1u];
        uint32_t expected_next = i + 1u == H5W_LEAF_COUNT ? 0u : leaves[i + 1u];
        if (!leaf_state(table, leaves[i], parents[i / 2u],
                        expected_prev, expected_next) ||
            !present(table, schema, 10u * (i + 1u))) return false;
    }
    return true;
}

static bool h5w_cascaded_state(Table* table,
                               TableSchema* schema,
                               const uint32_t greats[H5W_GREAT_COUNT],
                               const uint32_t grands[H5W_GRAND_COUNT],
                               const uint32_t parents[H5W_PARENT_COUNT],
                               const uint32_t leaves[H5W_LEAF_COUNT],
                               uint32_t free_before) {
    const uint32_t root_children[2] = {greats[0], greats[1]};
    const uint32_t root_keys[1] = {180u};
    const uint32_t affected_children[4] = {grands[0], grands[1], grands[2], grands[3]};
    const uint32_t affected_keys[3] = {40u, 80u, 140u};
    const uint32_t control_children[2] = {grands[4], grands[5]};
    const uint32_t control_keys[1] = {220u};
    const uint32_t g0_children[2] = {parents[0], parents[1]};
    const uint32_t g0_keys[1] = {20u};
    const uint32_t donor_children[2] = {parents[2], parents[3]};
    const uint32_t donor_keys[1] = {60u};
    const uint32_t target_children[2] = {parents[4], parents[6]};
    const uint32_t target_keys[1] = {100u};
    const uint32_t g3_children[2] = {parents[7], parents[8]};
    const uint32_t g3_keys[1] = {160u};
    const uint32_t g4_children[2] = {parents[9], parents[10]};
    const uint32_t g4_keys[1] = {200u};
    const uint32_t g5_children[2] = {parents[11], parents[12]};
    const uint32_t g5_keys[1] = {240u};
    const uint32_t moved_children[2] = {leaves[8], leaves[9]};
    const uint32_t moved_keys[1] = {90u};
    const uint32_t kept_children[3] = {leaves[10], leaves[12], leaves[13]};
    const uint32_t kept_keys[2] = {110u, 130u};

    if (!internal_matches(table, schema->root_page_num, 0u, true,
                          root_children, root_keys, 2u) ||
        !internal_matches(table, greats[0], schema->root_page_num, false,
                          affected_children, affected_keys, 4u) ||
        !internal_matches(table, greats[1], schema->root_page_num, false,
                          control_children, control_keys, 2u) ||
        !internal_matches(table, grands[0], greats[0], false,
                          g0_children, g0_keys, 2u) ||
        !internal_matches(table, grands[1], greats[0], false,
                          donor_children, donor_keys, 2u) ||
        !internal_matches(table, grands[2], greats[0], false,
                          target_children, target_keys, 2u) ||
        !internal_matches(table, grands[3], greats[0], false,
                          g3_children, g3_keys, 2u) ||
        !internal_matches(table, grands[4], greats[1], false,
                          g4_children, g4_keys, 2u) ||
        !internal_matches(table, grands[5], greats[1], false,
                          g5_children, g5_keys, 2u) ||
        !internal_matches(table, parents[4], grands[2], false,
                          moved_children, moved_keys, 2u) ||
        !internal_matches(table, parents[6], grands[2], false,
                          kept_children, kept_keys, 3u) ||
        table->pager->free_page_count != free_before + 2u ||
        !free_has(table->pager, leaves[11]) ||
        !free_has(table->pager, parents[5]) ||
        present(table, schema, 120u) ||
        tinydb_record_scan(table, schema, NULL, NULL) != H5W_BASELINE_ROWS - 1u) {
        return false;
    }

    for (uint32_t i = 0u; i < H5W_LEAF_COUNT; i++) {
        if (i == 11u) continue;
        uint32_t expected_parent = parents[i / 2u];
        uint32_t expected_prev = i == 0u ? 0u : leaves[i - 1u];
        uint32_t expected_next = i + 1u == H5W_LEAF_COUNT ? 0u : leaves[i + 1u];
        if (i == 10u) {
            expected_parent = parents[6];
            expected_next = leaves[12];
        } else if (i == 12u) {
            expected_prev = leaves[10];
        }
        if (!leaf_state(table, leaves[i], expected_parent,
                        expected_prev, expected_next)) return false;
    }
    for (uint32_t i = 0u; i < H5W_LEAF_COUNT; i++) {
        uint32_t key = 10u * (i + 1u);
        if ((key == 120u && present(table, schema, key)) ||
            (key != 120u && !present(table, schema, key))) return false;
    }
    return true;
}

static bool h5w_run_case(const char* path) {
    remove(path);
    TinyDB* db = tinydb_open(path);
    if (db == NULL ||
        !exec_ok(db, "CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return false;
    }
    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t greats[H5W_GREAT_COUNT] = {0u};
    uint32_t grands[H5W_GRAND_COUNT] = {0u};
    uint32_t parents[H5W_PARENT_COUNT] = {0u};
    uint32_t leaves[H5W_LEAF_COUNT] = {0u};
    if (schema == NULL || schema->row_size != 264u ||
        !h5w_seed_tree(db, schema, greats, grands, parents, leaves)) {
        tinydb_close(db);
        return false;
    }

    uint32_t free_before = table->pager->free_page_count;
    uint32_t root_before = schema->root_page_num;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    if (!exec_ok(db, "BEGIN;") ||
        !tinydb_record_delete(table, schema, 120u, message, sizeof(message)) ||
        schema->root_page_num != root_before ||
        !h5w_cascaded_state(table, schema, greats, grands, parents, leaves,
                            free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;") ||
        !h5w_original_state(table, schema, greats, grands, parents, leaves,
                            free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "mirrored inner-window rollback failed: %s\n", message);
        tinydb_close(db);
        return false;
    }

    message[0] = '\0';
    if (!tinydb_record_delete(table, schema, 120u, message, sizeof(message)) ||
        schema->root_page_num != root_before ||
        !h5w_cascaded_state(table, schema, greats, grands, parents, leaves,
                            free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "mirrored inner-window autocommit failed: %s\n", message);
        tinydb_close(db);
        return false;
    }
    tinydb_close(db);

    db = tinydb_open(path);
    if (db == NULL) return false;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    bool ok = schema != NULL && schema->root_page_num == root_before &&
              h5w_cascaded_state(table, schema, greats, grands, parents, leaves,
                                 free_before) &&
              exec_ok(db, "PRAGMA integrity_check;");
    tinydb_close(db);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    if (!h5w_run_case(argv[1])) return EXIT_FAILURE;
    printf("V2_RECURSIVE_NONROOT_MERGE_BORROW_LEFT_WINDOW_OK height5=yes "
           "deleted=120 left_donor=yes wider_ancestor=yes pair_index=1 "
           "inner_pair=yes controls=yes local_separator=yes root_unchanged=yes "
           "bottom_merge=yes grand_borrow=yes rollback=yes allocator=yes "
           "wal=yes reopen=yes integrity=yes\n");
    return EXIT_SUCCESS;
}
