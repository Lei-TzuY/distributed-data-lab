/*
 * The minimum height-4 eight-leaf matrix now has explicit production routes
 * for every leaf position. Keep the fail-closed sentinel by moving one level
 * deeper instead of deleting safety coverage.
 *
 * This probe builds a minimum height-5 tree with 16 singleton compact-V2
 * leaves. Deleting key 70 would require propagating internal underflow through
 * one more ancestor level than the bounded height-4 contraction routes support.
 * The operation must therefore fail without changing any database page,
 * allocator state, row visibility, root identity, or reopen integrity.
 */
#define main tinydb_legacy_recursive_underflow_probe_main
#include "v2_recursive_internal_underflow_guard_probe.c"
#undef main

#define H5_LEAF_COUNT 16u
#define H5_PARENT_COUNT 8u
#define H5_GRAND_COUNT 4u
#define H5_GREAT_COUNT 2u

static bool h5_alloc_page(Pager* pager, uint32_t* page_num) {
    if (pager == NULL || page_num == NULL) return false;
    *page_num = get_unused_page_num(pager);
    if (*page_num == 0u || *page_num == INVALID_PAGE_NUM) return false;
    return get_page(pager, *page_num) != NULL;
}

static bool h5_shape(Table* table,
                     TableSchema* schema,
                     const uint32_t greats[H5_GREAT_COUNT],
                     const uint32_t grands[H5_GRAND_COUNT],
                     const uint32_t parents[H5_PARENT_COUNT],
                     const uint32_t leaves[H5_LEAF_COUNT]) {
    if (table == NULL || schema == NULL) return false;

    const uint32_t root_children[2] = {greats[0], greats[1]};
    const uint32_t root_keys[1] = {80u};
    if (!internal_matches(table, schema->root_page_num,
                          root_children, root_keys, 2u)) {
        return false;
    }

    for (uint32_t i = 0u; i < H5_GREAT_COUNT; i++) {
        const uint32_t children[2] = {grands[2u * i], grands[2u * i + 1u]};
        const uint32_t separators[1] = {40u + 80u * i};
        if (!internal_matches(table, greats[i], children, separators, 2u)) {
            return false;
        }
    }

    for (uint32_t i = 0u; i < H5_GRAND_COUNT; i++) {
        const uint32_t children[2] = {parents[2u * i], parents[2u * i + 1u]};
        const uint32_t separators[1] = {20u + 40u * i};
        if (!internal_matches(table, grands[i], children, separators, 2u)) {
            return false;
        }
    }

    for (uint32_t i = 0u; i < H5_PARENT_COUNT; i++) {
        const uint32_t children[2] = {leaves[2u * i], leaves[2u * i + 1u]};
        const uint32_t separators[1] = {10u + 20u * i};
        if (!internal_matches(table, parents[i], children, separators, 2u)) {
            return false;
        }
    }

    for (uint32_t i = 0u; i < H5_LEAF_COUNT; i++) {
        uint32_t expected_prev = i == 0u ? 0u : leaves[i - 1u];
        uint32_t expected_next = i + 1u == H5_LEAF_COUNT ? 0u : leaves[i + 1u];
        uint32_t key = 10u * (i + 1u);
        if (!leaf_state(table,
                        leaves[i],
                        parents[i / 2u],
                        expected_prev,
                        expected_next) ||
            !present(table, schema, key)) {
            return false;
        }
    }
    return tinydb_record_scan(table, schema, NULL, NULL) == H5_LEAF_COUNT;
}

static bool seed_height5(TinyDB* db,
                         TableSchema* schema,
                         uint32_t greats[H5_GREAT_COUNT],
                         uint32_t grands[H5_GRAND_COUNT],
                         uint32_t parents[H5_PARENT_COUNT],
                         uint32_t leaves[H5_LEAF_COUNT]) {
    Table* table = tinydb_table(db);
    Pager* pager = table == NULL ? NULL : table->pager;
    if (pager == NULL || schema == NULL) return false;

    for (uint32_t i = 0u; i < H5_GREAT_COUNT; i++) {
        if (!h5_alloc_page(pager, &greats[i])) return false;
    }
    for (uint32_t i = 0u; i < H5_GRAND_COUNT; i++) {
        if (!h5_alloc_page(pager, &grands[i])) return false;
    }
    for (uint32_t i = 0u; i < H5_PARENT_COUNT; i++) {
        if (!h5_alloc_page(pager, &parents[i])) return false;
    }
    for (uint32_t i = 0u; i < H5_LEAF_COUNT; i++) {
        if (!h5_alloc_page(pager, &leaves[i])) return false;
    }

    for (uint32_t i = 0u; i < H5_LEAF_COUNT; i++) {
        unsigned char* leaf = (unsigned char*)get_page(pager, leaves[i]);
        uint32_t key = 10u * (i + 1u);
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
            !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE)) {
            return false;
        }
        mark_page_dirty(pager, leaves[i]);
    }

    for (uint32_t i = 0u; i < H5_PARENT_COUNT; i++) {
        const uint32_t children[2] = {leaves[2u * i], leaves[2u * i + 1u]};
        const uint32_t separators[1] = {10u + 20u * i};
        if (!build_internal(pager,
                            parents[i],
                            grands[i / 2u],
                            false,
                            children,
                            separators,
                            2u)) {
            return false;
        }
        mark_page_dirty(pager, parents[i]);
    }

    for (uint32_t i = 0u; i < H5_GRAND_COUNT; i++) {
        const uint32_t children[2] = {parents[2u * i], parents[2u * i + 1u]};
        const uint32_t separators[1] = {20u + 40u * i};
        if (!build_internal(pager,
                            grands[i],
                            greats[i / 2u],
                            false,
                            children,
                            separators,
                            2u)) {
            return false;
        }
        mark_page_dirty(pager, grands[i]);
    }

    for (uint32_t i = 0u; i < H5_GREAT_COUNT; i++) {
        const uint32_t children[2] = {grands[2u * i], grands[2u * i + 1u]};
        const uint32_t separators[1] = {40u + 80u * i};
        if (!build_internal(pager,
                            greats[i],
                            schema->root_page_num,
                            false,
                            children,
                            separators,
                            2u)) {
            return false;
        }
        mark_page_dirty(pager, greats[i]);
    }

    const uint32_t root_children[2] = {greats[0], greats[1]};
    const uint32_t root_keys[1] = {80u};
    if (!build_internal(pager,
                        schema->root_page_num,
                        0u,
                        true,
                        root_children,
                        root_keys,
                        2u)) {
        return false;
    }
    mark_page_dirty(pager, schema->root_page_num);
    pager_commit(pager);
    return h5_shape(table, schema, greats, grands, parents, leaves) &&
           exec_ok(db, "PRAGMA integrity_check;");
}

static unsigned char* snapshot_pages(Pager* pager,
                                     uint32_t* num_pages_out) {
    if (pager == NULL || num_pages_out == NULL || pager->num_pages == 0u) {
        return NULL;
    }
    size_t bytes = (size_t)pager->num_pages * PAGE_SIZE;
    unsigned char* snapshot = (unsigned char*)malloc(bytes);
    if (snapshot == NULL) return NULL;
    *num_pages_out = pager->num_pages;
    for (uint32_t i = 0u; i < pager->num_pages; i++) {
        memcpy(snapshot + (size_t)i * PAGE_SIZE,
               get_page(pager, i),
               PAGE_SIZE);
    }
    return snapshot;
}

static bool pages_unchanged(Pager* pager,
                            const unsigned char* snapshot,
                            uint32_t snapshot_num_pages,
                            uint32_t free_before) {
    if (pager == NULL || snapshot == NULL ||
        pager->num_pages != snapshot_num_pages ||
        pager->free_page_count != free_before) {
        return false;
    }
    for (uint32_t i = 0u; i < snapshot_num_pages; i++) {
        if (memcmp(get_page(pager, i),
                   snapshot + (size_t)i * PAGE_SIZE,
                   PAGE_SIZE) != 0) {
            fprintf(stderr, "height5 guard page drifted: %u\n", i);
            return false;
        }
    }
    return true;
}

static bool run_height5_guard(const char* path) {
    remove(path);
    TinyDB* db = tinydb_open(path);
    if (db == NULL ||
        !exec_ok(db,
                 "CREATE TABLE items (id INT, name VARCHAR(255), price INT);")) {
        if (db != NULL) tinydb_close(db);
        return false;
    }

    Table* table = tinydb_table(db);
    TableSchema* schema = find_schema(table, "items");
    uint32_t greats[H5_GREAT_COUNT] = {0u, 0u};
    uint32_t grands[H5_GRAND_COUNT] = {0u, 0u, 0u, 0u};
    uint32_t parents[H5_PARENT_COUNT] = {0u};
    uint32_t leaves[H5_LEAF_COUNT] = {0u};
    if (schema == NULL || schema->row_size != 264u ||
        !seed_height5(db, schema, greats, grands, parents, leaves)) {
        tinydb_close(db);
        return false;
    }

    uint32_t root_before = schema->root_page_num;
    uint32_t free_before = table->pager->free_page_count;
    uint32_t snapshot_num_pages = 0u;
    unsigned char* snapshot = snapshot_pages(table->pager, &snapshot_num_pages);
    if (snapshot == NULL) {
        tinydb_close(db);
        return false;
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    bool deleted = tinydb_record_delete(table,
                                        schema,
                                        70u,
                                        message,
                                        sizeof(message));
    bool unchanged = !deleted && schema->root_page_num == root_before &&
                     pages_unchanged(table->pager,
                                     snapshot,
                                     snapshot_num_pages,
                                     free_before) &&
                     h5_shape(table, schema, greats, grands, parents, leaves) &&
                     exec_ok(db, "PRAGMA integrity_check;");
    free(snapshot);
    if (!unchanged) {
        fprintf(stderr,
                "height5 recursive underflow guard mutated state: deleted=%d message=%s\n",
                deleted ? 1 : 0,
                message);
        tinydb_close(db);
        return false;
    }

    tinydb_close(db);
    db = tinydb_open(path);
    if (db == NULL) return false;
    table = tinydb_table(db);
    schema = find_schema(table, "items");
    bool reopened = schema != NULL && schema->root_page_num == root_before &&
                    table->pager->free_page_count == free_before &&
                    h5_shape(table, schema, greats, grands, parents, leaves) &&
                    present(table, schema, 70u) &&
                    exec_ok(db, "PRAGMA integrity_check;");
    tinydb_close(db);
    return reopened;
}

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    char path[1024];
    if (snprintf(path, sizeof(path), "%s.height5", argv[1]) < 0) {
        return EXIT_FAILURE;
    }
    if (!run_height5_guard(path)) return EXIT_FAILURE;
    printf("V2_RECURSIVE_INTERNAL_UNDERFLOW_GUARD_OK height5=yes "
           "deeper_cascade_unsupported=yes fail_closed=yes page_snapshot=yes "
           "root_stable=yes ancestor_stable=yes leaf_chain=yes allocator=yes "
           "reopen=yes integrity=yes\n");
    return EXIT_SUCCESS;
}
