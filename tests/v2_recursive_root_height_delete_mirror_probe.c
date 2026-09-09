#define main tinydb_legacy_recursive_underflow_probe_main
#include "v2_recursive_internal_underflow_guard_probe.c"
#undef main

static bool contracted_mirror_state(
    Table* table,
    TableSchema* schema,
    const uint32_t parents[LEAF_PARENT_COUNT],
    const uint32_t leaves[LEAF_COUNT],
    uint32_t free_before) {
    const uint32_t root_children[3] = {parents[1], parents[2], parents[3]};
    const uint32_t root_keys[2] = {40u, 60u};
    const uint32_t kept_children[3] = {leaves[0], leaves[2], leaves[3]};
    const uint32_t kept_keys[2] = {10u, 30u};
    const uint32_t p2_children[2] = {leaves[4], leaves[5]};
    const uint32_t p2_keys[1] = {50u};
    const uint32_t p3_children[2] = {leaves[6], leaves[7]};
    const uint32_t p3_keys[1] = {70u};
    if (!internal_matches(table, schema->root_page_num,
                          root_children, root_keys, 3u) ||
        !internal_matches(table, parents[1], kept_children, kept_keys, 3u) ||
        !internal_matches(table, parents[2], p2_children, p2_keys, 2u) ||
        !internal_matches(table, parents[3], p3_children, p3_keys, 2u) ||
        table->pager->free_page_count != free_before + 4u ||
        tinydb_record_scan(table, schema, NULL, NULL) != BASELINE_ROWS - 1u ||
        present(table, schema, 20u)) {
        return false;
    }

    const uint32_t survivor_indexes[7] = {0u, 2u, 3u, 4u, 5u, 6u, 7u};
    const uint32_t survivor_parents[7] = {
        parents[1], parents[1], parents[1],
        parents[2], parents[2], parents[3], parents[3]
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

static bool run_mirror_case(const char* path) {
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
        !tinydb_record_delete(table, schema, 20u, message, sizeof(message)) ||
        schema->root_page_num != root_before ||
        !contracted_mirror_state(table, schema, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;") ||
        !exec_ok(db, "ROLLBACK;") ||
        !unchanged_state(table, schema, grands, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "mirrored root-height rollback failed: %s\n", message);
        tinydb_close(db);
        return false;
    }

    message[0] = '\0';
    if (!tinydb_record_delete(table, schema, 20u, message, sizeof(message)) ||
        schema->root_page_num != root_before ||
        !contracted_mirror_state(table, schema, parents, leaves, free_before) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        fprintf(stderr, "mirrored root-height autocommit failed: %s\n", message);
        tinydb_close(db);
        return false;
    }
    tinydb_close(db);

    db = tinydb_open(path);
    if (db == NULL) return false;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    bool ok = schema != NULL && schema->root_page_num == root_before &&
              contracted_mirror_state(table, schema, parents, leaves, free_before) &&
              exec_ok(db, "PRAGMA integrity_check;");
    tinydb_close(db);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    if (!run_mirror_case(argv[1])) return EXIT_FAILURE;
    printf("V2_RECURSIVE_ROOT_HEIGHT_DELETE_MIRROR_OK deleted=20 mirror=yes "
           "height4_to_height3=yes root_identity=yes bottom_merge=yes "
           "grandparents_reclaimed=yes rollback=yes allocator=yes wal=yes "
           "leaf_chain=yes reopen=yes integrity=yes\n");
    return EXIT_SUCCESS;
}
