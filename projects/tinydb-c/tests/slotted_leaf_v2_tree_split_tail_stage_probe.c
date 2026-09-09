#include "slotted_leaf_v2_tree_split_tail_stage.h"

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

static bool seed_leaf(unsigned char page[PAGE_SIZE],
                      unsigned char trailer,
                      uint32_t parent,
                      uint32_t prev) {
    memset(page, trailer, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent);
    tinydb_slotted_split_write_u32(page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, prev);
    tinydb_slotted_split_write_u32(page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, 0u);
    return true;
}

static void seed_parent(unsigned char page[PAGE_SIZE],
                        unsigned char trailer,
                        uint32_t left_child,
                        uint32_t tail_child,
                        uint32_t left_max) {
    memset(page, trailer, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 1u;
    write_u32(page + PARENT_POINTER_OFFSET, 0u);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE;
    write_u32(cell, left_child);
    write_u32(cell + INTERNAL_NODE_CHILD_SIZE, left_max);
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, tail_child);
}

static bool seed_tree(unsigned char left[PAGE_SIZE],
                      unsigned char right[PAGE_SIZE],
                      unsigned char parent[PAGE_SIZE]) {
    const unsigned char a[700] = {1u};
    const unsigned char b[900] = {2u};
    const unsigned char c[600] = {3u};
    const unsigned char d[1000] = {4u};
    if (!seed_leaf(left, 0xA1u, 80u, 40u)) return false;
    memset(right, 0xB2, PAGE_SIZE);
    seed_parent(parent, 0xD4u, 40u, 50u, 5u);
    return tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 10u, a, sizeof(a)) &&
           tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 20u, b, sizeof(b)) &&
           tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 30u, c, sizeof(c)) &&
           tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 40u, d, sizeof(d));
}

static bool test_success(void) {
    unsigned char left[PAGE_SIZE], right[PAGE_SIZE], parent[PAGE_SIZE];
    if (!seed_tree(left, right, parent)) return false;
    uint16_t split_index = 0u;
    uint32_t inserted_index = UINT32_MAX;
    if (!tinydb_slotted_leaf_v2_stage_tree_split_nonroot_tail(
            left, PAGE_SIZE, 50u, right, PAGE_SIZE, 51u,
            parent, PAGE_SIZE, &split_index, &inserted_index)) {
        fprintf(stderr, "tail split staging rejected valid rightmost split\n");
        return false;
    }
    uint32_t parent_keys = tinydb_parent_stage_read_u32(parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (split_index == 0u || inserted_index != parent_keys || parent_keys != 2u ||
        !tinydb_slotted_leaf_v2_validate(left, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(parent, PAGE_SIZE) ||
        tinydb_slotted_split_read_u32(left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != 51u ||
        tinydb_slotted_split_read_u32(right + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) != 50u ||
        tinydb_slotted_split_read_u32(right + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != 0u ||
        tinydb_parent_stage_child_at(parent, parent_keys) != 51u ||
        !trailer_is(left, 0xA1u) || !trailer_is(right, 0xB2u) || !trailer_is(parent, 0xD4u)) {
        fprintf(stderr, "tail split staging produced inconsistent topology\n");
        return false;
    }
    return true;
}

static bool test_non_tail_rejection_is_atomic(void) {
    unsigned char left[PAGE_SIZE], right[PAGE_SIZE], parent[PAGE_SIZE];
    unsigned char before_left[PAGE_SIZE], before_right[PAGE_SIZE], before_parent[PAGE_SIZE];
    if (!seed_tree(left, right, parent)) return false;
    tinydb_slotted_split_write_u32(left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, 60u);
    memcpy(before_left, left, PAGE_SIZE);
    memcpy(before_right, right, PAGE_SIZE);
    memcpy(before_parent, parent, PAGE_SIZE);
    if (tinydb_slotted_leaf_v2_stage_tree_split_nonroot_tail(
            left, PAGE_SIZE, 50u, right, PAGE_SIZE, 51u,
            parent, PAGE_SIZE, NULL, NULL) ||
        memcmp(left, before_left, PAGE_SIZE) != 0 ||
        memcmp(right, before_right, PAGE_SIZE) != 0 ||
        memcmp(parent, before_parent, PAGE_SIZE) != 0) {
        fprintf(stderr, "non-tail rejection leaked partial page writes\n");
        return false;
    }
    return true;
}

int main(void) {
    if (!test_success() || !test_non_tail_rejection_is_atomic()) return EXIT_FAILURE;
    printf("SLOTTED_TREE_SPLIT_TAIL_STAGE_OK tail_split=yes parent_stage=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
