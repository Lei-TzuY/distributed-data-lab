#include "slotted_leaf_v2_parent_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static void seed_parent(unsigned char page[PAGE_SIZE],
                        unsigned char trailer,
                        const uint32_t* children,
                        const uint32_t* keys,
                        uint32_t num_keys) {
    memset(page, trailer, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 1u;
    write_u32(page + PARENT_POINTER_OFFSET, 0u);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, num_keys);
    for (uint32_t i = 0u; i < num_keys; i++) {
        unsigned char* cell =
            page + INTERNAL_NODE_HEADER_SIZE + i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, children[i]);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, keys[i]);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, children[num_keys]);
}

static bool expect_child_key(const unsigned char* page,
                             uint32_t index,
                             uint32_t child,
                             uint32_t key) {
    return tinydb_parent_stage_child_at(page, index) == child &&
           tinydb_parent_stage_key_at(page, index) == key;
}

static bool test_interior_split(void) {
    unsigned char parent[PAGE_SIZE];
    const uint32_t children[] = {10u, 20u, 30u, 40u};
    const uint32_t keys[] = {100u, 200u, 300u};
    seed_parent(parent, 0xA5u, children, keys, 3u);

    uint32_t inserted_index = UINT32_MAX;
    if (!tinydb_slotted_leaf_v2_stage_parent_split(parent,
                                                    PAGE_SIZE,
                                                    20u,
                                                    21u,
                                                    200u,
                                                    150u,
                                                    200u,
                                                    &inserted_index) ||
        inserted_index != 2u ||
        !tinydb_parent_stage_validate(parent, PAGE_SIZE) ||
        tinydb_parent_stage_read_u32(parent + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            4u ||
        !expect_child_key(parent, 0u, 10u, 100u) ||
        !expect_child_key(parent, 1u, 20u, 150u) ||
        !expect_child_key(parent, 2u, 21u, 200u) ||
        !expect_child_key(parent, 3u, 30u, 300u) ||
        tinydb_parent_stage_child_at(parent, 4u) != 40u ||
        !trailer_is(parent, 0xA5u)) {
        fprintf(stderr, "interior parent staging failed\n");
        return false;
    }
    return true;
}

static bool test_rightmost_split(void) {
    unsigned char parent[PAGE_SIZE];
    const uint32_t children[] = {10u, 20u, 30u};
    const uint32_t keys[] = {100u, 200u};
    seed_parent(parent, 0xB6u, children, keys, 2u);

    uint32_t inserted_index = UINT32_MAX;
    if (!tinydb_slotted_leaf_v2_stage_parent_split(parent,
                                                    PAGE_SIZE,
                                                    30u,
                                                    31u,
                                                    300u,
                                                    250u,
                                                    350u,
                                                    &inserted_index) ||
        inserted_index != 3u ||
        !tinydb_parent_stage_validate(parent, PAGE_SIZE) ||
        tinydb_parent_stage_read_u32(parent + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            3u ||
        !expect_child_key(parent, 0u, 10u, 100u) ||
        !expect_child_key(parent, 1u, 20u, 200u) ||
        !expect_child_key(parent, 2u, 30u, 250u) ||
        tinydb_parent_stage_child_at(parent, 3u) != 31u ||
        !trailer_is(parent, 0xB6u)) {
        fprintf(stderr, "rightmost parent staging failed\n");
        return false;
    }
    return true;
}

static bool expect_atomic_rejection(unsigned char parent[PAGE_SIZE],
                                    uint32_t left_page_num,
                                    uint32_t right_page_num,
                                    uint32_t old_left_max,
                                    uint32_t new_left_max,
                                    uint32_t new_right_max) {
    unsigned char before[PAGE_SIZE];
    memcpy(before, parent, PAGE_SIZE);
    return !tinydb_slotted_leaf_v2_stage_parent_split(parent,
                                                       PAGE_SIZE,
                                                       left_page_num,
                                                       right_page_num,
                                                       old_left_max,
                                                       new_left_max,
                                                       new_right_max,
                                                       NULL) &&
           memcmp(parent, before, PAGE_SIZE) == 0;
}

static bool test_fail_closed_guards(void) {
    unsigned char parent[PAGE_SIZE];
    const uint32_t children[] = {10u, 20u, 30u, 40u};
    const uint32_t keys[] = {100u, 200u, 300u};

    seed_parent(parent, 0xC7u, children, keys, 3u);
    if (!expect_atomic_rejection(parent, 20u, 21u, 201u, 150u, 200u)) {
        fprintf(stderr, "old separator mismatch was not atomic\n");
        return false;
    }

    seed_parent(parent, 0xC7u, children, keys, 3u);
    if (!expect_atomic_rejection(parent, 20u, 21u, 200u, 150u, 199u)) {
        fprintf(stderr, "interior upper-boundary drift was not rejected\n");
        return false;
    }

    seed_parent(parent, 0xC7u, children, keys, 3u);
    if (!expect_atomic_rejection(parent, 20u, 30u, 200u, 150u, 200u)) {
        fprintf(stderr, "duplicate right child was not rejected\n");
        return false;
    }

    seed_parent(parent, 0xC7u, children, keys, 3u);
    write_u32(parent + INTERNAL_NODE_HEADER_SIZE + INTERNAL_NODE_CHILD_SIZE,
              250u);
    if (!expect_atomic_rejection(parent, 20u, 21u, 200u, 150u, 200u)) {
        fprintf(stderr, "unsorted parent was not rejected atomically\n");
        return false;
    }

    uint32_t full_children[INTERNAL_NODE_MAX_KEYS + 1u];
    uint32_t full_keys[INTERNAL_NODE_MAX_KEYS];
    for (uint32_t i = 0u; i <= INTERNAL_NODE_MAX_KEYS; i++) {
        full_children[i] = 100u + i;
        if (i < INTERNAL_NODE_MAX_KEYS) full_keys[i] = 10u + i * 10u;
    }
    seed_parent(parent,
                0xD8u,
                full_children,
                full_keys,
                INTERNAL_NODE_MAX_KEYS);
    if (!expect_atomic_rejection(parent,
                                 full_children[0],
                                 999u,
                                 full_keys[0],
                                 5u,
                                 full_keys[0])) {
        fprintf(stderr, "full parent overflow was not rejected atomically\n");
        return false;
    }

    return true;
}

int main(void) {
    if (!test_interior_split() ||
        !test_rightmost_split() ||
        !test_fail_closed_guards()) {
        return EXIT_FAILURE;
    }

    printf("SLOTTED_PARENT_STAGE_OK interior=yes rightmost=yes boundary_guard=yes overflow_guard=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
