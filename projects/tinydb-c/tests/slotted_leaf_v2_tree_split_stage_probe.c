#include "slotted_leaf_v2_tree_split_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_u32(const unsigned char* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static bool seed_leaf(unsigned char page[PAGE_SIZE],
                      unsigned char trailer,
                      uint32_t parent,
                      uint32_t prev,
                      uint32_t next) {
    memset(page, trailer, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent);
    tinydb_slotted_split_write_u32(page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
                                    prev);
    tinydb_slotted_split_write_u32(page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
                                    next);
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

static bool seed_tree(unsigned char left[PAGE_SIZE],
                      unsigned char right[PAGE_SIZE],
                      unsigned char old_next[PAGE_SIZE],
                      unsigned char parent[PAGE_SIZE],
                      uint32_t first_parent_key) {
    const unsigned char a[700] = {1u};
    const unsigned char b[900] = {2u};
    const unsigned char c[600] = {3u};
    const unsigned char d[1000] = {4u};
    const uint32_t children[] = {50u, 60u, 70u};
    const uint32_t keys[] = {first_parent_key, 50u};

    if (!seed_leaf(left, 0xA1u, 80u, 40u, 60u) ||
        !seed_leaf(old_next, 0xC3u, 80u, 50u, 70u)) {
        return false;
    }
    memset(right, 0xB2, PAGE_SIZE);
    seed_parent(parent, 0xD4u, children, keys, 2u);

    return tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 10u, a, sizeof(a)) &&
           tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 20u, b, sizeof(b)) &&
           tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 30u, c, sizeof(c)) &&
           tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 40u, d, sizeof(d)) &&
           tinydb_slotted_leaf_v2_insert(old_next,
                                         PAGE_SIZE,
                                         50u,
                                         a,
                                         sizeof(a));
}

static bool test_success(void) {
    unsigned char left[PAGE_SIZE];
    unsigned char right[PAGE_SIZE];
    unsigned char old_next[PAGE_SIZE];
    unsigned char parent[PAGE_SIZE];
    if (!seed_tree(left, right, old_next, parent, 40u)) return false;

    uint16_t split_index = 0u;
    uint32_t inserted_index = UINT32_MAX;
    if (!tinydb_slotted_leaf_v2_stage_tree_split_nonroot_with_next(
            left,
            PAGE_SIZE,
            50u,
            right,
            PAGE_SIZE,
            51u,
            old_next,
            PAGE_SIZE,
            60u,
            parent,
            PAGE_SIZE,
            &split_index,
            &inserted_index)) {
        fprintf(stderr, "tree split staging rejected valid split\n");
        return false;
    }

    uint16_t left_count = tinydb_slotted_leaf_v2_count(left, PAGE_SIZE);
    uint16_t right_count = tinydb_slotted_leaf_v2_count(right, PAGE_SIZE);
    if (split_index == 0u || left_count == 0u || right_count == 0u ||
        left_count + right_count != 4u || inserted_index != 1u ||
        !tinydb_slotted_leaf_v2_validate(left, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(old_next, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(parent, PAGE_SIZE) ||
        read_u32(left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != 51u ||
        read_u32(right + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) != 50u ||
        read_u32(right + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != 60u ||
        read_u32(old_next + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) != 51u ||
        tinydb_parent_stage_read_u32(parent + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            3u ||
        tinydb_parent_stage_child_at(parent, 0u) != 50u ||
        tinydb_parent_stage_child_at(parent, 1u) != 51u ||
        tinydb_parent_stage_key_at(parent, 1u) != 40u ||
        tinydb_parent_stage_child_at(parent, 2u) != 60u ||
        tinydb_parent_stage_key_at(parent, 2u) != 50u ||
        !trailer_is(left, 0xA1u) || !trailer_is(right, 0xB2u) ||
        !trailer_is(old_next, 0xC3u) || !trailer_is(parent, 0xD4u)) {
        fprintf(stderr, "tree split staging produced inconsistent topology\n");
        return false;
    }
    return true;
}

static bool test_parent_failure_is_atomic(void) {
    unsigned char left[PAGE_SIZE];
    unsigned char right[PAGE_SIZE];
    unsigned char old_next[PAGE_SIZE];
    unsigned char parent[PAGE_SIZE];
    unsigned char before_left[PAGE_SIZE];
    unsigned char before_right[PAGE_SIZE];
    unsigned char before_next[PAGE_SIZE];
    unsigned char before_parent[PAGE_SIZE];

    if (!seed_tree(left, right, old_next, parent, 41u)) return false;
    memcpy(before_left, left, PAGE_SIZE);
    memcpy(before_right, right, PAGE_SIZE);
    memcpy(before_next, old_next, PAGE_SIZE);
    memcpy(before_parent, parent, PAGE_SIZE);

    if (tinydb_slotted_leaf_v2_stage_tree_split_nonroot_with_next(
            left,
            PAGE_SIZE,
            50u,
            right,
            PAGE_SIZE,
            51u,
            old_next,
            PAGE_SIZE,
            60u,
            parent,
            PAGE_SIZE,
            NULL,
            NULL) ||
        memcmp(left, before_left, PAGE_SIZE) != 0 ||
        memcmp(right, before_right, PAGE_SIZE) != 0 ||
        memcmp(old_next, before_next, PAGE_SIZE) != 0 ||
        memcmp(parent, before_parent, PAGE_SIZE) != 0) {
        fprintf(stderr, "parent-stage rejection leaked partial page writes\n");
        return false;
    }
    return true;
}

int main(void) {
    if (!test_success() || !test_parent_failure_is_atomic()) {
        return EXIT_FAILURE;
    }

    printf("SLOTTED_TREE_SPLIT_STAGE_OK leaf_split=yes backlink_repair=yes parent_stage=yes atomic_parent_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
