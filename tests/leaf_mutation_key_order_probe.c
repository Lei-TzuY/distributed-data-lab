#include "engine.h"
#include "leaf_mutation_policy.h"
#include "leaf_page_access.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool expect_policy(Table* table,
                          uint32_t root_page_num,
                          bool expected,
                          const char* expected_message) {
    char message[256];
    memset(message, 0, sizeof(message));
    bool supported = tinydb_leaf_tree_mutation_supported(table,
                                                         root_page_num,
                                                         message,
                                                         sizeof(message));
    if (supported != expected) {
        fprintf(stderr,
                "unexpected mutation policy result: expected=%d actual=%d message=%s\n",
                expected ? 1 : 0,
                supported ? 1 : 0,
                message);
        return false;
    }
    if (expected_message != NULL && strstr(message, expected_message) == NULL) {
        fprintf(stderr,
                "unexpected mutation policy diagnostic: wanted '%s', got '%s'\n",
                expected_message,
                message);
        return false;
    }
    return true;
}

static void install_keys(void* page, const uint32_t* keys, uint32_t count) {
    *leaf_node_num_cells(page) = count;
    for (uint32_t i = 0u; i < count; i++) {
        *leaf_node_key(page, i) = keys[i];
    }
}

static bool policy_is_read_only(Table* table,
                                uint32_t root_page_num,
                                const unsigned char* before) {
    void* page = get_page(table->pager, root_page_num);
    return memcmp(page, before, PAGE_SIZE) == 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: leaf_mutation_key_order_probe <db>\n");
        return 1;
    }

    remove(argv[1]);
    TinyDB* db = tinydb_open(argv[1]);
    if (db == NULL) return 1;

    Table* table = tinydb_table(db);
    if (table == NULL || table->pager == NULL) {
        tinydb_close(db);
        return 1;
    }

    uint32_t root_page_num = table->root_page_num;
    void* page = get_page(table->pager, root_page_num);
    if (page == NULL || get_node_type(page) != NODE_LEAF ||
        !tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE)) {
        fprintf(stderr, "fresh database root is not a fixed V1 leaf\n");
        tinydb_close(db);
        return 1;
    }

    *leaf_node_prev_leaf(page) = 0u;
    *leaf_node_next_leaf(page) = 0u;

    const uint32_t sorted[] = {10u, 20u, 30u, 40u};
    install_keys(page, sorted, 4u);
    unsigned char before[PAGE_SIZE];
    memcpy(before, page, sizeof(before));
    if (!expect_policy(table, root_page_num, true, NULL) ||
        !policy_is_read_only(table, root_page_num, before)) {
        fprintf(stderr, "valid fixed V1 leaf was rejected or modified\n");
        tinydb_close(db);
        return 1;
    }

    const uint32_t internally_unsorted[] = {10u, 30u, 20u, 40u};
    install_keys(page, internally_unsorted, 4u);
    memcpy(before, page, sizeof(before));
    if (!expect_policy(table,
                       root_page_num,
                       false,
                       "non-monotonic leaf key order blocks mutation") ||
        !policy_is_read_only(table, root_page_num, before)) {
        fprintf(stderr, "interior fixed-leaf key disorder was not rejected atomically\n");
        tinydb_close(db);
        return 1;
    }

    const uint32_t duplicate_interior[] = {10u, 20u, 20u, 40u};
    install_keys(page, duplicate_interior, 4u);
    memcpy(before, page, sizeof(before));
    if (!expect_policy(table,
                       root_page_num,
                       false,
                       "non-monotonic leaf key order blocks mutation") ||
        !policy_is_read_only(table, root_page_num, before)) {
        fprintf(stderr, "duplicate fixed-leaf key was not rejected atomically\n");
        tinydb_close(db);
        return 1;
    }

    install_keys(page, sorted, 4u);
    if (!expect_policy(table, root_page_num, true, NULL)) {
        fprintf(stderr, "restored fixed V1 ordering did not recover mutation eligibility\n");
        tinydb_close(db);
        return 1;
    }

    tinydb_close(db);
    remove(argv[1]);
    printf("LEAF_MUTATION_KEY_ORDER_OK sorted=yes interior_disorder=yes duplicate=yes unchanged=yes\n");
    return 0;
}
