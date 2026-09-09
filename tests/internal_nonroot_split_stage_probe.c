#include "internal_nonroot_split_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRANDPARENT_PAGE_NUM 50u
#define FULL_PARENT_PAGE_NUM 80u
#define NEW_RIGHT_INTERNAL_PAGE_NUM 81u
#define CHILD_BASE 1000u
#define SPLIT_RIGHT_PAGE_NUM 9000u

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static void seed_full_parent(unsigned char page[PAGE_SIZE], unsigned char trailer) {
    memset(page, trailer, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, GRANDPARENT_PAGE_NUM);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE +
                              i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, CHILD_BASE + i);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, (i + 1u) * 10u);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              CHILD_BASE + INTERNAL_NODE_MAX_KEYS);
}

static bool contains_child(const unsigned char* node, uint32_t child) {
    uint32_t keys = tinydb_parent_stage_read_u32(
        node + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= keys; i++) {
        if (tinydb_parent_stage_child_at(node, i) == child) return true;
    }
    return false;
}

static bool test_middle_split(void) {
    unsigned char parent[PAGE_SIZE], right[PAGE_SIZE];
    seed_full_parent(parent, 0xA1u);
    memset(right, 0xB2, sizeof(right));

    uint32_t split_index = 100u;
    uint32_t old_max = (split_index + 1u) * 10u;
    uint32_t promoted = 0u;
    uint32_t left_count = 0u;
    if (!tinydb_stage_full_nonroot_after_child_split(
            parent,
            PAGE_SIZE,
            FULL_PARENT_PAGE_NUM,
            right,
            PAGE_SIZE,
            NEW_RIGHT_INTERNAL_PAGE_NUM,
            CHILD_BASE + split_index,
            SPLIT_RIGHT_PAGE_NUM,
            old_max,
            old_max - 5u,
            old_max,
            &promoted,
            &left_count)) {
        fprintf(stderr, "middle non-root split staging failed\n");
        return false;
    }

    uint32_t left_keys = tinydb_parent_stage_read_u32(
        parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    uint32_t right_keys = tinydb_parent_stage_read_u32(
        right + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (!tinydb_parent_stage_validate(parent, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(right, PAGE_SIZE) ||
        parent[IS_ROOT_OFFSET] != 0u || right[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(parent + PARENT_POINTER_OFFSET) !=
            GRANDPARENT_PAGE_NUM ||
        tinydb_parent_stage_read_u32(right + PARENT_POINTER_OFFSET) !=
            GRANDPARENT_PAGE_NUM ||
        left_keys + 1u != left_count ||
        left_keys + right_keys + 2u != INTERNAL_NODE_MAX_KEYS + 2u ||
        promoted == 0u ||
        contains_child(parent, SPLIT_RIGHT_PAGE_NUM) ==
            contains_child(right, SPLIT_RIGHT_PAGE_NUM) ||
        !trailer_is(parent, 0xA1u) || !trailer_is(right, 0xB2u)) {
        fprintf(stderr, "middle non-root split topology invalid\n");
        return false;
    }
    return true;
}

static bool test_rightmost_split(void) {
    unsigned char parent[PAGE_SIZE], right[PAGE_SIZE];
    seed_full_parent(parent, 0xC3u);
    memset(right, 0xD4, sizeof(right));

    uint32_t old_max = (INTERNAL_NODE_MAX_KEYS + 1u) * 10u;
    uint32_t promoted = 0u;
    if (!tinydb_stage_full_nonroot_after_child_split(
            parent,
            PAGE_SIZE,
            FULL_PARENT_PAGE_NUM,
            right,
            PAGE_SIZE,
            NEW_RIGHT_INTERNAL_PAGE_NUM,
            CHILD_BASE + INTERNAL_NODE_MAX_KEYS,
            SPLIT_RIGHT_PAGE_NUM,
            old_max,
            old_max - 5u,
            old_max + 10u,
            &promoted,
            NULL) ||
        promoted == 0u ||
        !contains_child(right, SPLIT_RIGHT_PAGE_NUM) ||
        !trailer_is(parent, 0xC3u) || !trailer_is(right, 0xD4u)) {
        fprintf(stderr, "rightmost non-root split staging failed\n");
        return false;
    }
    return true;
}

static bool test_rejection_atomic(void) {
    unsigned char parent[PAGE_SIZE], right[PAGE_SIZE];
    unsigned char before_parent[PAGE_SIZE], before_right[PAGE_SIZE];
    seed_full_parent(parent, 0xE5u);
    memset(right, 0xF6, sizeof(right));
    memcpy(before_parent, parent, PAGE_SIZE);
    memcpy(before_right, right, PAGE_SIZE);

    uint32_t split_index = 77u;
    uint32_t old_max = (split_index + 1u) * 10u;
    if (tinydb_stage_full_nonroot_after_child_split(
            parent,
            PAGE_SIZE,
            FULL_PARENT_PAGE_NUM,
            right,
            PAGE_SIZE,
            NEW_RIGHT_INTERNAL_PAGE_NUM,
            CHILD_BASE + split_index,
            SPLIT_RIGHT_PAGE_NUM,
            old_max,
            old_max - 5u,
            old_max + 1u,
            NULL,
            NULL) ||
        memcmp(parent, before_parent, PAGE_SIZE) != 0 ||
        memcmp(right, before_right, PAGE_SIZE) != 0) {
        fprintf(stderr, "invalid non-root split leaked partial bytes\n");
        return false;
    }
    return true;
}

int main(void) {
    if (!test_middle_split() || !test_rightmost_split() ||
        !test_rejection_atomic()) {
        return EXIT_FAILURE;
    }
    printf("INTERNAL_NONROOT_SPLIT_STAGE_OK middle=yes rightmost=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
