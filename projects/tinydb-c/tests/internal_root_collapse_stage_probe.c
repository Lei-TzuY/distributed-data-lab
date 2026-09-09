#include "internal_root_collapse_stage.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_leaf_v2_split.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static bool build_root(unsigned char page[PAGE_SIZE],
                       uint32_t left_child,
                       uint32_t separator,
                       uint32_t right_child) {
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 1u;
    write_u32(page + PARENT_POINTER_OFFSET, 0u);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE;
    write_u32(cell, left_child);
    write_u32(cell + INTERNAL_NODE_CHILD_SIZE, separator);
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, right_child);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool build_internal(unsigned char page[PAGE_SIZE],
                           uint32_t parent_page_num,
                           uint32_t left_child,
                           uint32_t separator,
                           uint32_t right_child) {
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE;
    write_u32(cell, left_child);
    write_u32(cell + INTERNAL_NODE_CHILD_SIZE, separator);
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, right_child);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool build_v2_leaf(unsigned char page[PAGE_SIZE],
                          uint32_t parent_page_num,
                          uint32_t key) {
    memset(page, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    tinydb_slotted_split_write_u32(
        page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
        0u);
    tinydb_slotted_split_write_u32(
        page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
        0u);
    uint32_t value = key + 1000u;
    return tinydb_slotted_leaf_v2_insert(page,
                                         PAGE_SIZE,
                                         key,
                                         &value,
                                         (uint16_t)sizeof(value)) &&
           tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE);
}

static bool root_is_single_v2_leaf(const unsigned char page[PAGE_SIZE],
                                   uint32_t key) {
    uint32_t count = 0u;
    uint32_t actual_key = 0u;
    uint32_t previous = INVALID_PAGE_NUM;
    uint32_t next = INVALID_PAGE_NUM;
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           page[IS_ROOT_OFFSET] != 0u &&
           tinydb_parent_stage_read_u32(page + PARENT_POINTER_OFFSET) == 0u &&
           tinydb_leaf_page_count(page, PAGE_SIZE, &count) && count == 1u &&
           tinydb_leaf_page_key_at(page, PAGE_SIZE, 0u, &actual_key) &&
           actual_key == key &&
           tinydb_leaf_page_prev(page, PAGE_SIZE, &previous) &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next) &&
           previous == 0u && next == 0u;
}

static bool root_is_internal(const unsigned char page[PAGE_SIZE],
                             uint32_t left_child,
                             uint32_t separator,
                             uint32_t right_child) {
    return tinydb_parent_stage_validate(page, PAGE_SIZE) &&
           page[IS_ROOT_OFFSET] != 0u &&
           tinydb_parent_stage_read_u32(page + PARENT_POINTER_OFFSET) == 0u &&
           tinydb_parent_stage_read_u32(
               page + INTERNAL_NODE_NUM_KEYS_OFFSET) == 1u &&
           tinydb_parent_stage_child_at(page, 0u) == left_child &&
           tinydb_parent_stage_key_at(page, 0u) == separator &&
           tinydb_parent_stage_child_at(page, 1u) == right_child;
}

static bool page_parent_is(const unsigned char page[PAGE_SIZE],
                           uint32_t parent_page_num) {
    return tinydb_parent_stage_read_u32(page + PARENT_POINTER_OFFSET) ==
           parent_page_num;
}

static bool trailer_matches(const unsigned char page[PAGE_SIZE],
                            unsigned char marker) {
    for (size_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static void fill_trailer(unsigned char page[PAGE_SIZE], unsigned char marker) {
    memset(page + PAGE_USABLE_SIZE,
           marker,
           PAGE_SIZE - PAGE_USABLE_SIZE);
}

static bool left_collapse_case(void) {
    const uint32_t root_page_num = 7u;
    const uint32_t removed_page_num = 11u;
    const uint32_t survivor_page_num = 12u;
    unsigned char root[PAGE_SIZE];
    unsigned char survivor[PAGE_SIZE];
    unsigned char survivor_before[PAGE_SIZE];
    if (!build_root(root, removed_page_num, 10u, survivor_page_num) ||
        !build_v2_leaf(survivor, root_page_num, 20u)) {
        return false;
    }
    fill_trailer(root, 0xa5u);
    memcpy(survivor_before, survivor, PAGE_SIZE);

    uint32_t promoted = 0u;
    if (!tinydb_stage_internal_root_collapse_to_v2_leaf(
            root,
            PAGE_SIZE,
            root_page_num,
            survivor,
            PAGE_SIZE,
            survivor_page_num,
            removed_page_num,
            10u,
            &promoted)) {
        return false;
    }
    return promoted == survivor_page_num &&
           root_is_single_v2_leaf(root, 20u) &&
           trailer_matches(root, 0xa5u) &&
           memcmp(survivor, survivor_before, PAGE_SIZE) == 0;
}

static bool right_collapse_case(void) {
    const uint32_t root_page_num = 9u;
    const uint32_t survivor_page_num = 21u;
    const uint32_t removed_page_num = 22u;
    unsigned char root[PAGE_SIZE];
    unsigned char survivor[PAGE_SIZE];
    unsigned char survivor_before[PAGE_SIZE];
    if (!build_root(root, survivor_page_num, 20u, removed_page_num) ||
        !build_v2_leaf(survivor, root_page_num, 20u)) {
        return false;
    }
    fill_trailer(root, 0x5au);
    memcpy(survivor_before, survivor, PAGE_SIZE);

    uint32_t promoted = 0u;
    if (!tinydb_stage_internal_root_collapse_to_v2_leaf(
            root,
            PAGE_SIZE,
            root_page_num,
            survivor,
            PAGE_SIZE,
            survivor_page_num,
            removed_page_num,
            30u,
            &promoted)) {
        return false;
    }
    return promoted == survivor_page_num &&
           root_is_single_v2_leaf(root, 20u) &&
           trailer_matches(root, 0x5au) &&
           memcmp(survivor, survivor_before, PAGE_SIZE) == 0;
}

static bool internal_survivor_case(void) {
    const uint32_t root_page_num = 70u;
    const uint32_t removed_page_num = 71u;
    const uint32_t survivor_page_num = 72u;
    const uint32_t child_nums[2] = {73u, 74u};
    unsigned char root[PAGE_SIZE];
    unsigned char survivor[PAGE_SIZE];
    unsigned char survivor_before[PAGE_SIZE];
    unsigned char left_child[PAGE_SIZE];
    unsigned char right_child[PAGE_SIZE];
    void* child_pages[2] = {left_child, right_child};

    if (!build_root(root, removed_page_num, 30u, survivor_page_num) ||
        !build_internal(survivor,
                        root_page_num,
                        child_nums[0],
                        40u,
                        child_nums[1]) ||
        !build_v2_leaf(left_child, survivor_page_num, 40u) ||
        !build_v2_leaf(right_child, survivor_page_num, 60u)) {
        return false;
    }

    fill_trailer(root, 0xc1u);
    fill_trailer(survivor, 0xc2u);
    fill_trailer(left_child, 0xc3u);
    fill_trailer(right_child, 0xc4u);
    memcpy(survivor_before, survivor, PAGE_SIZE);

    uint32_t promoted = 0u;
    if (!tinydb_stage_internal_root_collapse_to_internal(
            root,
            PAGE_SIZE,
            root_page_num,
            survivor,
            PAGE_SIZE,
            survivor_page_num,
            60u,
            removed_page_num,
            30u,
            child_pages,
            child_nums,
            2u,
            &promoted)) {
        return false;
    }

    return promoted == survivor_page_num &&
           root_is_internal(root, child_nums[0], 40u, child_nums[1]) &&
           page_parent_is(left_child, root_page_num) &&
           page_parent_is(right_child, root_page_num) &&
           tinydb_slotted_leaf_v2_validate(left_child, PAGE_SIZE) &&
           tinydb_slotted_leaf_v2_validate(right_child, PAGE_SIZE) &&
           trailer_matches(root, 0xc1u) &&
           trailer_matches(left_child, 0xc3u) &&
           trailer_matches(right_child, 0xc4u) &&
           memcmp(survivor, survivor_before, PAGE_SIZE) == 0;
}

static bool internal_failure_is_atomic(void) {
    const uint32_t root_page_num = 80u;
    const uint32_t survivor_page_num = 81u;
    const uint32_t removed_page_num = 82u;
    const uint32_t child_nums[2] = {83u, 84u};
    unsigned char root[PAGE_SIZE];
    unsigned char survivor[PAGE_SIZE];
    unsigned char left_child[PAGE_SIZE];
    unsigned char right_child[PAGE_SIZE];
    unsigned char root_before[PAGE_SIZE];
    unsigned char survivor_before[PAGE_SIZE];
    unsigned char left_before[PAGE_SIZE];
    unsigned char right_before[PAGE_SIZE];
    void* child_pages[2] = {left_child, right_child};

    if (!build_root(root, survivor_page_num, 60u, removed_page_num) ||
        !build_internal(survivor,
                        root_page_num,
                        child_nums[0],
                        40u,
                        child_nums[1]) ||
        !build_v2_leaf(left_child, survivor_page_num, 40u) ||
        !build_v2_leaf(right_child, survivor_page_num + 1u, 60u)) {
        return false;
    }
    fill_trailer(root, 0xd1u);
    fill_trailer(left_child, 0xd2u);
    fill_trailer(right_child, 0xd3u);
    memcpy(root_before, root, PAGE_SIZE);
    memcpy(survivor_before, survivor, PAGE_SIZE);
    memcpy(left_before, left_child, PAGE_SIZE);
    memcpy(right_before, right_child, PAGE_SIZE);

    uint32_t promoted = 999u;
    return !tinydb_stage_internal_root_collapse_to_internal(
               root,
               PAGE_SIZE,
               root_page_num,
               survivor,
               PAGE_SIZE,
               survivor_page_num,
               60u,
               removed_page_num,
               90u,
               child_pages,
               child_nums,
               2u,
               &promoted) &&
           promoted == INVALID_PAGE_NUM &&
           memcmp(root, root_before, PAGE_SIZE) == 0 &&
           memcmp(survivor, survivor_before, PAGE_SIZE) == 0 &&
           memcmp(left_child, left_before, PAGE_SIZE) == 0 &&
           memcmp(right_child, right_before, PAGE_SIZE) == 0;
}

static bool failure_is_atomic(void) {
    const uint32_t root_page_num = 13u;
    const uint32_t removed_page_num = 31u;
    const uint32_t survivor_page_num = 32u;
    unsigned char root[PAGE_SIZE];
    unsigned char survivor[PAGE_SIZE];
    unsigned char root_before[PAGE_SIZE];
    unsigned char survivor_before[PAGE_SIZE];
    if (!build_root(root, removed_page_num, 10u, survivor_page_num) ||
        !build_v2_leaf(survivor, root_page_num, 20u)) {
        return false;
    }
    tinydb_slotted_split_write_u32(
        survivor + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
        99u);
    memcpy(root_before, root, PAGE_SIZE);
    memcpy(survivor_before, survivor, PAGE_SIZE);

    uint32_t promoted = 123u;
    if (tinydb_stage_internal_root_collapse_to_v2_leaf(
            root,
            PAGE_SIZE,
            root_page_num,
            survivor,
            PAGE_SIZE,
            survivor_page_num,
            removed_page_num,
            10u,
            &promoted) ||
        promoted != INVALID_PAGE_NUM ||
        memcmp(root, root_before, PAGE_SIZE) != 0 ||
        memcmp(survivor, survivor_before, PAGE_SIZE) != 0) {
        return false;
    }

    if (!build_v2_leaf(survivor, root_page_num + 1u, 20u)) return false;
    memcpy(root_before, root, PAGE_SIZE);
    memcpy(survivor_before, survivor, PAGE_SIZE);
    promoted = 456u;
    return !tinydb_stage_internal_root_collapse_to_v2_leaf(
               root,
               PAGE_SIZE,
               root_page_num,
               survivor,
               PAGE_SIZE,
               survivor_page_num,
               removed_page_num,
               10u,
               &promoted) &&
           promoted == INVALID_PAGE_NUM &&
           memcmp(root, root_before, PAGE_SIZE) == 0 &&
           memcmp(survivor, survivor_before, PAGE_SIZE) == 0;
}

int main(void) {
    if (!left_collapse_case()) {
        fprintf(stderr, "left root-collapse staging failed\n");
        return EXIT_FAILURE;
    }
    if (!right_collapse_case()) {
        fprintf(stderr, "right root-collapse staging failed\n");
        return EXIT_FAILURE;
    }
    if (!internal_survivor_case()) {
        fprintf(stderr, "internal-survivor root-collapse staging failed\n");
        return EXIT_FAILURE;
    }
    if (!failure_is_atomic() || !internal_failure_is_atomic()) {
        fprintf(stderr, "root-collapse failure was not atomic\n");
        return EXIT_FAILURE;
    }

    printf("INTERNAL_ROOT_COLLAPSE_STAGE_OK left=yes right=yes v2_root=yes "
           "internal=yes descendant_reparent=yes survivor_immutable=yes "
           "atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
