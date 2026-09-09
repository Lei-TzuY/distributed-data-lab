#include "internal_root_merge_stage.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT_PAGE 1u
#define LEFT_PARENT_PAGE 2u
#define RIGHT_PARENT_PAGE 3u
#define LEAF0_PAGE 10u
#define LEAF1_PAGE 11u
#define LEAF2_PAGE 12u
#define LEAF3_PAGE 13u

typedef struct {
    unsigned char root[PAGE_SIZE];
    unsigned char left_parent[PAGE_SIZE];
    unsigned char right_parent[PAGE_SIZE];
    unsigned char leaves[4][PAGE_SIZE];
} Fixture;

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static void write_u32_le(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static uint32_t read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void stamp_trailer(unsigned char page[PAGE_SIZE], unsigned char value) {
    for (size_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) page[i] = value;
}

static bool trailer_matches(const unsigned char page[PAGE_SIZE],
                            const unsigned char before[PAGE_SIZE]) {
    return memcmp(page + PAGE_USABLE_SIZE,
                  before + PAGE_USABLE_SIZE,
                  PAGE_SIZE - PAGE_USABLE_SIZE) == 0;
}

static bool init_leaf(unsigned char page[PAGE_SIZE],
                      uint32_t parent,
                      uint32_t prev,
                      uint32_t next,
                      uint32_t key,
                      unsigned char trailer) {
    unsigned char value[3];
    value[0] = (unsigned char)(key & 0xffu);
    value[1] = (unsigned char)((key >> 8) & 0xffu);
    value[2] = (unsigned char)0x5au;
    memset(page, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent);
    write_u32_le(page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, prev);
    write_u32_le(page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, next);
    if (!tinydb_slotted_leaf_v2_insert(page,
                                       PAGE_SIZE,
                                       key,
                                       value,
                                       (uint16_t)sizeof(value)) ||
        !tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE)) {
        return false;
    }
    stamp_trailer(page, trailer);
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE);
}

static bool init_internal(unsigned char page[PAGE_SIZE],
                          uint32_t parent,
                          bool is_root,
                          uint32_t left_child,
                          uint32_t left_max,
                          uint32_t right_child,
                          unsigned char trailer) {
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = is_root ? 1u : 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE;
    write_u32(cell, left_child);
    write_u32(cell + INTERNAL_NODE_CHILD_SIZE, left_max);
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, right_child);
    if (!tinydb_parent_stage_validate(page, PAGE_SIZE)) return false;
    stamp_trailer(page, trailer);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool seed(Fixture* fixture) {
    if (fixture == NULL) return false;
    memset(fixture, 0, sizeof(*fixture));
    if (!init_leaf(fixture->leaves[0], LEFT_PARENT_PAGE, 0u, LEAF1_PAGE,
                   10u, 0xa0u) ||
        !init_leaf(fixture->leaves[1], LEFT_PARENT_PAGE, LEAF0_PAGE, LEAF2_PAGE,
                   20u, 0xa1u) ||
        !init_leaf(fixture->leaves[2], RIGHT_PARENT_PAGE, LEAF1_PAGE, LEAF3_PAGE,
                   30u, 0xa2u) ||
        !init_leaf(fixture->leaves[3], RIGHT_PARENT_PAGE, LEAF2_PAGE, 0u,
                   40u, 0xa3u) ||
        !init_internal(fixture->left_parent, ROOT_PAGE, false,
                       LEAF0_PAGE, 10u, LEAF1_PAGE, 0xb1u) ||
        !init_internal(fixture->right_parent, ROOT_PAGE, false,
                       LEAF2_PAGE, 30u, LEAF3_PAGE, 0xb2u) ||
        !init_internal(fixture->root, 0u, true,
                       LEFT_PARENT_PAGE, 20u, RIGHT_PARENT_PAGE, 0xc0u)) {
        return false;
    }
    return true;
}

static bool check_leaf(const unsigned char page[PAGE_SIZE],
                       uint32_t expected_prev,
                       uint32_t expected_next,
                       uint32_t expected_key) {
    uint32_t count = 0u;
    uint32_t prev = INVALID_PAGE_NUM;
    uint32_t next = INVALID_PAGE_NUM;
    uint32_t key = 0u;
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           page[IS_ROOT_OFFSET] == 0u &&
           read_u32(page + PARENT_POINTER_OFFSET) == ROOT_PAGE &&
           tinydb_leaf_page_count(page, PAGE_SIZE, &count) && count == 1u &&
           tinydb_leaf_page_key_at(page, PAGE_SIZE, 0u, &key) &&
           key == expected_key &&
           tinydb_leaf_page_prev(page, PAGE_SIZE, &prev) &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next) &&
           prev == expected_prev && next == expected_next;
}

static bool check_root(const unsigned char root[PAGE_SIZE],
                       uint32_t child0,
                       uint32_t key0,
                       uint32_t child1,
                       uint32_t key1,
                       uint32_t child2) {
    return tinydb_parent_stage_validate(root, PAGE_SIZE) &&
           root[IS_ROOT_OFFSET] != 0u &&
           read_u32(root + PARENT_POINTER_OFFSET) == 0u &&
           read_u32(root + INTERNAL_NODE_NUM_KEYS_OFFSET) == 2u &&
           tinydb_parent_stage_child_at(root, 0u) == child0 &&
           tinydb_parent_stage_key_at(root, 0u) == key0 &&
           tinydb_parent_stage_child_at(root, 1u) == child1 &&
           tinydb_parent_stage_key_at(root, 1u) == key1 &&
           tinydb_parent_stage_child_at(root, 2u) == child2;
}

static bool test_remove_left_boundary(void) {
    Fixture fixture;
    unsigned char root_before[PAGE_SIZE];
    unsigned char left_before[PAGE_SIZE];
    unsigned char right_before[PAGE_SIZE];
    unsigned char removed_before[PAGE_SIZE];
    unsigned char leaf_before[3][PAGE_SIZE];
    if (!seed(&fixture)) return false;

    memcpy(root_before, fixture.root, PAGE_SIZE);
    memcpy(left_before, fixture.left_parent, PAGE_SIZE);
    memcpy(right_before, fixture.right_parent, PAGE_SIZE);
    memcpy(removed_before, fixture.leaves[1], PAGE_SIZE);
    memcpy(leaf_before[0], fixture.leaves[0], PAGE_SIZE);
    memcpy(leaf_before[1], fixture.leaves[2], PAGE_SIZE);
    memcpy(leaf_before[2], fixture.leaves[3], PAGE_SIZE);

    void* survivors[3] = {
        fixture.leaves[0], fixture.leaves[2], fixture.leaves[3]
    };
    const uint32_t survivor_nums[3] = {LEAF0_PAGE, LEAF2_PAGE, LEAF3_PAGE};
    if (!tinydb_stage_internal_root_merge_after_v2_leaf_removal(
            fixture.root, PAGE_SIZE, ROOT_PAGE,
            fixture.left_parent, PAGE_SIZE, LEFT_PARENT_PAGE,
            fixture.right_parent, PAGE_SIZE, RIGHT_PARENT_PAGE,
            fixture.leaves[1], PAGE_SIZE, LEAF1_PAGE, 20u,
            survivors, survivor_nums)) {
        return false;
    }

    return check_root(fixture.root, LEAF0_PAGE, 10u,
                      LEAF2_PAGE, 30u, LEAF3_PAGE) &&
           check_leaf(fixture.leaves[0], 0u, LEAF2_PAGE, 10u) &&
           check_leaf(fixture.leaves[2], LEAF0_PAGE, LEAF3_PAGE, 30u) &&
           check_leaf(fixture.leaves[3], LEAF2_PAGE, 0u, 40u) &&
           memcmp(fixture.left_parent, left_before, PAGE_SIZE) == 0 &&
           memcmp(fixture.right_parent, right_before, PAGE_SIZE) == 0 &&
           memcmp(fixture.leaves[1], removed_before, PAGE_SIZE) == 0 &&
           trailer_matches(fixture.root, root_before) &&
           trailer_matches(fixture.leaves[0], leaf_before[0]) &&
           trailer_matches(fixture.leaves[2], leaf_before[1]) &&
           trailer_matches(fixture.leaves[3], leaf_before[2]);
}

static bool test_remove_right_boundary(void) {
    Fixture fixture;
    unsigned char root_before[PAGE_SIZE];
    unsigned char left_before[PAGE_SIZE];
    unsigned char right_before[PAGE_SIZE];
    unsigned char removed_before[PAGE_SIZE];
    unsigned char leaf_before[3][PAGE_SIZE];
    if (!seed(&fixture)) return false;

    memcpy(root_before, fixture.root, PAGE_SIZE);
    memcpy(left_before, fixture.left_parent, PAGE_SIZE);
    memcpy(right_before, fixture.right_parent, PAGE_SIZE);
    memcpy(removed_before, fixture.leaves[2], PAGE_SIZE);
    memcpy(leaf_before[0], fixture.leaves[0], PAGE_SIZE);
    memcpy(leaf_before[1], fixture.leaves[1], PAGE_SIZE);
    memcpy(leaf_before[2], fixture.leaves[3], PAGE_SIZE);

    void* survivors[3] = {
        fixture.leaves[0], fixture.leaves[1], fixture.leaves[3]
    };
    const uint32_t survivor_nums[3] = {LEAF0_PAGE, LEAF1_PAGE, LEAF3_PAGE};
    if (!tinydb_stage_internal_root_merge_after_v2_leaf_removal(
            fixture.root, PAGE_SIZE, ROOT_PAGE,
            fixture.left_parent, PAGE_SIZE, LEFT_PARENT_PAGE,
            fixture.right_parent, PAGE_SIZE, RIGHT_PARENT_PAGE,
            fixture.leaves[2], PAGE_SIZE, LEAF2_PAGE, 30u,
            survivors, survivor_nums)) {
        return false;
    }

    return check_root(fixture.root, LEAF0_PAGE, 10u,
                      LEAF1_PAGE, 20u, LEAF3_PAGE) &&
           check_leaf(fixture.leaves[0], 0u, LEAF1_PAGE, 10u) &&
           check_leaf(fixture.leaves[1], LEAF0_PAGE, LEAF3_PAGE, 20u) &&
           check_leaf(fixture.leaves[3], LEAF1_PAGE, 0u, 40u) &&
           memcmp(fixture.left_parent, left_before, PAGE_SIZE) == 0 &&
           memcmp(fixture.right_parent, right_before, PAGE_SIZE) == 0 &&
           memcmp(fixture.leaves[2], removed_before, PAGE_SIZE) == 0 &&
           trailer_matches(fixture.root, root_before) &&
           trailer_matches(fixture.leaves[0], leaf_before[0]) &&
           trailer_matches(fixture.leaves[1], leaf_before[1]) &&
           trailer_matches(fixture.leaves[3], leaf_before[2]);
}

static bool test_atomic_failure(void) {
    Fixture fixture;
    unsigned char root_before[PAGE_SIZE];
    unsigned char leaf0_before[PAGE_SIZE];
    unsigned char leaf2_before[PAGE_SIZE];
    unsigned char leaf3_before[PAGE_SIZE];
    if (!seed(&fixture)) return false;

    /* Make the cross-parent backlink stale while keeping every page otherwise
     * valid. The staging helper must reject the topology before publishing any
     * of the caller-visible images. */
    write_u32_le(fixture.leaves[2] + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
                 LEAF0_PAGE);
    if (!tinydb_slotted_leaf_v2_validate(fixture.leaves[2], PAGE_SIZE)) {
        return false;
    }

    memcpy(root_before, fixture.root, PAGE_SIZE);
    memcpy(leaf0_before, fixture.leaves[0], PAGE_SIZE);
    memcpy(leaf2_before, fixture.leaves[2], PAGE_SIZE);
    memcpy(leaf3_before, fixture.leaves[3], PAGE_SIZE);
    void* survivors[3] = {
        fixture.leaves[0], fixture.leaves[2], fixture.leaves[3]
    };
    const uint32_t survivor_nums[3] = {LEAF0_PAGE, LEAF2_PAGE, LEAF3_PAGE};
    if (tinydb_stage_internal_root_merge_after_v2_leaf_removal(
            fixture.root, PAGE_SIZE, ROOT_PAGE,
            fixture.left_parent, PAGE_SIZE, LEFT_PARENT_PAGE,
            fixture.right_parent, PAGE_SIZE, RIGHT_PARENT_PAGE,
            fixture.leaves[1], PAGE_SIZE, LEAF1_PAGE, 20u,
            survivors, survivor_nums)) {
        return false;
    }
    return memcmp(fixture.root, root_before, PAGE_SIZE) == 0 &&
           memcmp(fixture.leaves[0], leaf0_before, PAGE_SIZE) == 0 &&
           memcmp(fixture.leaves[2], leaf2_before, PAGE_SIZE) == 0 &&
           memcmp(fixture.leaves[3], leaf3_before, PAGE_SIZE) == 0;
}

int main(void) {
    bool left = test_remove_left_boundary();
    bool right = test_remove_right_boundary();
    bool atomic = test_atomic_failure();
    if (!left || !right || !atomic) {
        fprintf(stderr,
                "internal root merge staging failed: left=%s right=%s atomic=%s\n",
                left ? "yes" : "no",
                right ? "yes" : "no",
                atomic ? "yes" : "no");
        return EXIT_FAILURE;
    }

    printf("INTERNAL_ROOT_MERGE_STAGE_OK ");
    printf("left=yes right=yes root_contract=yes descendant_reparent=yes ");
    printf("cross_parent_relink=yes source_immutable=yes atomic_failure=yes ");
    printf("checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
