#define main tinydb_legacy_recursive_underflow_probe_main
#include "v2_recursive_internal_underflow_guard_probe.c"
#undef main

static bool contracted_right_inner_right_state(
    Table* table,
    TableSchema* schema,
    const uint32_t parents[LEAF_PARENT_COUNT],
    const uint32_t leaves[LEAF_COUNT],
    uint32_t free_before) {
    const uint32_t root_children[3] = {parents[0], parents[1], parents[3]};
    const uint32_t root_keys[2] = {20u, 40u};
    const uint32_t p0_children[2] = {leaves[0], leaves[1]};
    const uint32_t p0_keys[1] = {10u};
    const uint32_t p1_children[2] = {leaves[2], leaves[3]};
    const uint32_t p1_keys[1] = {30u};
    const uint32_t kept_children[3] = {leaves[4], leaves[6], leaves[7]};
    const uint32_t kept_keys[2] = {50u, 70u};
    if (!internal_matches(table, schema->root_page_num,
                          root_children, root_keys, 3u) ||
        !internal_matches(table, parents[0], p0_children, p0_keys, 2u) ||
        !internal_matches(table, parents[1], p1_children, p1_keys, 2u) ||
        !internal_matches(table, parents[3], kept_children, kept_keys, 3u) ||
        table->pager->free_page_count != free_before + 4u ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        present(table, schema, 60u)) {
        return false;
    }

    const uint32_t survivor_indexes[7] = {0u, 1u, 2u, 3u, 4u, 6u, 7u};
    const uint32_t survivor_parents[7] = {
        parents[0], parents[0], parents[1], parents[1],
        parents[3], parents[3], parents[3]
    };
    for (uint32_t out = 0u; out < 7u; out++) {
        uint32_t i = survivor_indexes[out];
        uint32_t prev = out == 0u ? 0u : leaves[survivor_indexes[out - 1u]];
        uint32_t next = out + 1u == 7u ? 0u : leaves[survivor_indexes[out + 1u]];
        if (!leaf_state(table, leaves[i], survivor_parents[out], prev, next) ||
            !present(table, schema, 10u * (i + 1u))) {
            return false;
        }
    }
    return true;
}

static bool run_right_inner_right_case(const char* path) {
    remove(path);
    TinyDB* db = tinydb_open(path);
    if (db == NULL ||
        !exec_ok(db, "CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return false;
    }
    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t grands[GRAND_COUNT] = {0u, 0u};
    uint32_t parents[LEAF_PARENT_COUNT] = {0u, 0u, 0u, 0u};
    uint32_t leaves[LEAF_COUNT] = {0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_tree(db, schema, grands, parents, leaves)) {
        tinydb_close(db);
        return false;
    }

    uint32_t root_before = schema->root_page_num;
    uint32_t free_before = table->pager->free_page_count;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';

    if (!exec_ok(db, "BEGIN;") ||
        !tinydb_record_delete(table, schema, 60u, message, sizeof(message)) ||
        schema->root_page_num != root_before ||
        !contracted_right_inner_right_state(table, schema, parents, leaves,
                                            free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;") ||
        !unchanged_state(table, schema, grands, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "right-inner-right root-height rollback failed: %s\n", message);
        tinydb_close(db);
        return false;
    }

    message[0] = '\0';
    if (!tinydb_record_delete(table, schema, 60u, message, sizeof(message)) ||
        schema->root_page_num != root_before ||
        !contracted_right_inner_right_state(table, schema, parents, leaves,
                                            free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "right-inner-right root-height autocommit failed: %s\n", message);
        tinydb_close(db);
        return false;
    }
    tinydb_close(db);

    db = tinydb_open(path);
    if (db == NULL) return false;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    bool ok = schema != NULL && schema->root_page_num == root_before &&
              contracted_right_inner_right_state(table, schema, parents, leaves,
                                                 free_before) &&
              exec_ok(db, "PRAGMA integrity_check;");
    tinydb_close(db);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    if (!run_right_inner_right_case(argv[1])) return EXIT_FAILURE;
    printf("V2_RECURSIVE_ROOT_HEIGHT_DELETE_RIGHT_INNER_RIGHT_OK deleted=60 "
           "right_inner_right=yes height4_to_height3=yes root_identity=yes "
           "local_boundary_relink=yes bottom_merge=yes grandparents_reclaimed=yes "
           "rollback=yes allocator=yes wal=yes reopen=yes integrity=yes\n");
    return EXIT_SUCCESS;
}
