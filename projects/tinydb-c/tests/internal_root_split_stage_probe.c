#include "internal_root_split_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT_PAGE_NUM 80u
#define LEFT_INTERNAL_PAGE_NUM 700u
#define RIGHT_INTERNAL_PAGE_NUM 701u
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

static void seed_full_root(unsigned char root[PAGE_SIZE], unsigned char trailer) {
    memset(root, trailer, PAGE_SIZE);
    root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    root[IS_ROOT_OFFSET] = 1u;
    write_u32(root + PARENT_POINTER_OFFSET, 0u);
    write_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET, INTERNAL_NODE_MAX_KEYS);
    for (uint32_t i = 0u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        unsigned char* cell = root + INTERNAL_NODE_HEADER_SIZE +
                              i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, CHILD_BASE + i);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, (i + 1u) * 10u);
    }
    write_u32(root + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
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

static bool verify_partition(const unsigned char root[PAGE_SIZE],
                             const unsigned char left[PAGE_SIZE],
                             const unsigned char right[PAGE_SIZE],
                             uint32_t expected_left_count) {
    if (!tinydb_parent_stage_validate(root, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(left, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(right, PAGE_SIZE) ||
        root[IS_ROOT_OFFSET] == 0u || left[IS_ROOT_OFFSET] != 0u ||
        right[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_child_at(root, 0u) != LEFT_INTERNAL_PAGE_NUM ||
        tinydb_parent_stage_child_at(root, 1u) != RIGHT_INTERNAL_PAGE_NUM ||
        tinydb_parent_stage_read_u32(left + PARENT_POINTER_OFFSET) != ROOT_PAGE_NUM ||
        tinydb_parent_stage_read_u32(right + PARENT_POINTER_OFFSET) != ROOT_PAGE_NUM) {
        return false;
    }

    uint32_t left_keys = tinydb_parent_stage_read_u32(
        left + INTERNAL_NODE_NUM_KEYS_OFFSET);
    uint32_t right_keys = tinydb_parent_stage_read_u32(
        right + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (left_keys + 1u != expected_left_count ||
        left_keys + right_keys + 2u != INTERNAL_NODE_MAX_KEYS + 2u) {
        return false;
    }

    for (uint32_t i = 0u; i <= INTERNAL_NODE_MAX_KEYS; i++) {
        uint32_t child = CHILD_BASE + i;
        bool in_left = contains_child(left, child);
        bool in_right = contains_child(right, child);
        if (in_left == in_right) return false;
    }
    return contains_child(left, SPLIT_RIGHT_PAGE_NUM) !=
           contains_child(right, SPLIT_RIGHT_PAGE_NUM);
}

static bool test_middle_split(void) {
    unsigned char root[PAGE_SIZE], left[PAGE_SIZE], right[PAGE_SIZE];
    seed_full_root(root, 0xA1u);
    memset(left, 0xB2, sizeof(left));
    memset(right, 0xC3, sizeof(right));

    uint32_t split_index = 100u;
    uint32_t old_max = (split_index + 1u) * 10u;
    uint32_t left_count = 0u;
    if (!tinydb_stage_full_root_after_child_split(
            root,
            PAGE_SIZE,
            ROOT_PAGE_NUM,
            left,
            PAGE_SIZE,
            LEFT_INTERNAL_PAGE_NUM,
            right,
            PAGE_SIZE,
            RIGHT_INTERNAL_PAGE_NUM,
            CHILD_BASE + split_index,
            SPLIT_RIGHT_PAGE_NUM,
            old_max,
            old_max - 5u,
            old_max,
            &left_count)) {
        fprintf(stderr, "middle full-root split staging failed\n");
        return false;
    }

    if (!verify_partition(root, left, right, left_count) ||
        !trailer_is(root, 0xA1u) || !trailer_is(left, 0xB2u) ||
        !trailer_is(right, 0xC3u)) {
        fprintf(stderr, "middle full-root split staging produced invalid topology\n");
        return false;
    }
    return true;
}

static bool test_tail_split(void) {
    unsigned char root[PAGE_SIZE], left[PAGE_SIZE], right[PAGE_SIZE];
    seed_full_root(root, 0xD4u);
    memset(left, 0xE5, sizeof(left));
    memset(right, 0xF6, sizeof(right));

    uint32_t old_tail_max = (INTERNAL_NODE_MAX_KEYS + 1u) * 10u;
    uint32_t left_count = 0u;
    if (!tinydb_stage_full_root_after_child_split(
            root,
            PAGE_SIZE,
            ROOT_PAGE_NUM,
            left,
            PAGE_SIZE,
            LEFT_INTERNAL_PAGE_NUM,
            right,
            PAGE_SIZE,
            RIGHT_INTERNAL_PAGE_NUM,
            CHILD_BASE + INTERNAL_NODE_MAX_KEYS,
            SPLIT_RIGHT_PAGE_NUM,
            old_tail_max,
            old_tail_max - 5u,
            old_tail_max + 10u,
            &left_count)) {
        fprintf(stderr, "tail full-root split staging failed\n");
        return false;
    }

    if (!verify_partition(root, left, right, left_count) ||
        !contains_child(right, SPLIT_RIGHT_PAGE_NUM) ||
        !trailer_is(root, 0xD4u) || !trailer_is(left, 0xE5u) ||
        !trailer_is(right, 0xF6u)) {
        fprintf(stderr, "tail full-root split staging produced invalid topology\n");
        return false;
    }
    return true;
}

static bool test_rejection_is_atomic(void) {
    unsigned char root[PAGE_SIZE], left[PAGE_SIZE], right[PAGE_SIZE];
    unsigned char before_root[PAGE_SIZE], before_left[PAGE_SIZE], before_right[PAGE_SIZE];
    seed_full_root(root, 0x11u);
    memset(left, 0x22, sizeof(left));
    memset(right, 0x33, sizeof(right));
    memcpy(before_root, root, PAGE_SIZE);
    memcpy(before_left, left, PAGE_SIZE);
    memcpy(before_right, right, PAGE_SIZE);

    uint32_t split_index = 50u;
    uint32_t old_max = (split_index + 1u) * 10u;
    if (tinydb_stage_full_root_after_child_split(
            root,
            PAGE_SIZE,
            ROOT_PAGE_NUM,
            left,
            PAGE_SIZE,
            LEFT_INTERNAL_PAGE_NUM,
            right,
            PAGE_SIZE,
            RIGHT_INTERNAL_PAGE_NUM,
            CHILD_BASE + split_index,
            SPLIT_RIGHT_PAGE_NUM,
            old_max,
            old_max - 5u,
            old_max + 1u,
            NULL) ||
        memcmp(root, before_root, PAGE_SIZE) != 0 ||
        memcmp(left, before_left, PAGE_SIZE) != 0 ||
        memcmp(right, before_right, PAGE_SIZE) != 0) {
        fprintf(stderr, "invalid boundary rejection leaked staged bytes\n");
        return false;
    }
    return true;
}

int main(void) {
    if (!test_middle_split() || !test_tail_split() ||
        !test_rejection_is_atomic()) {
        return EXIT_FAILURE;
    }
    printf("INTERNAL_ROOT_SPLIT_STAGE_OK middle=yes tail=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
