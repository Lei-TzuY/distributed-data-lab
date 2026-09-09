#include "internal_merge_borrow_root_cascade_stage.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT_PAGE 1u
#define LEFT_GRAND_PAGE 10u
#define RIGHT_GRAND_PAGE 11u
#define P0_PAGE 20u
#define P1_PAGE 21u
#define Q0_PAGE 22u
#define Q1_PAGE 23u
#define Q2_PAGE 24u
#define FIRST_LEAF_PAGE 30u
#define LEAF_COUNT 10u

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static uint32_t read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void write_u32_le(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void stamp(unsigned char page[PAGE_SIZE], unsigned char value) {
    for (size_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) page[i] = value;
}

static bool trailer_same(const unsigned char page[PAGE_SIZE],
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
    unsigned char value[3] = {
        (unsigned char)(key & 0xffu),
        (unsigned char)((key >> 8) & 0xffu),
        0x7cu
    };
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
    stamp(page, trailer);
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE);
}

static bool init_internal(unsigned char page[PAGE_SIZE],
                          uint32_t parent,
                          bool is_root,
                          const uint32_t* children,
                          const uint32_t* separators,
                          uint32_t child_count,
                          unsigned char trailer) {
    if (children == NULL || separators == NULL || child_count < 2u) return false;
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = is_root ? 1u : 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, child_count - 1u);
    for (uint32_t i = 0u; i + 1u < child_count; i++) {
        unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE +
                              (size_t)i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, children[i]);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, separators[i]);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              children[child_count - 1u]);
    if (!tinydb_parent_stage_validate(page, PAGE_SIZE)) return false;
    stamp(page, trailer);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

typedef struct {
    unsigned char root[PAGE_SIZE];
    unsigned char left_grand[PAGE_SIZE];
    unsigned char right_grand[PAGE_SIZE];
    unsigned char parents[5][PAGE_SIZE];
    unsigned char leaves[LEAF_COUNT][PAGE_SIZE];
} Fixture;

static bool seed(Fixture* f) {
    if (f == NULL) return false;
    memset(f, 0, sizeof(*f));
    const uint32_t parent_pages[5] = {P0_PAGE,P1_PAGE,Q0_PAGE,Q1_PAGE,Q2_PAGE};
    const uint32_t keys[LEAF_COUNT] = {10u,20u,30u,40u,50u,60u,70u,80u,90u,100u};
    for (uint32_t i = 0u; i < LEAF_COUNT; i++) {
        uint32_t leaf_page = FIRST_LEAF_PAGE + i;
        if (!init_leaf(f->leaves[i],
                       parent_pages[i / 2u],
                       i == 0u ? 0u : leaf_page - 1u,
                       i + 1u == LEAF_COUNT ? 0u : leaf_page + 1u,
                       keys[i],
                       (unsigned char)(0xa0u + i))) {
            return false;
        }
    }

    for (uint32_t i = 0u; i < 5u; i++) {
        uint32_t children[2] = {FIRST_LEAF_PAGE + 2u * i,
                                FIRST_LEAF_PAGE + 2u * i + 1u};
        uint32_t separators[1] = {keys[2u * i]};
        uint32_t grand = i < 2u ? LEFT_GRAND_PAGE : RIGHT_GRAND_PAGE;
        if (!init_internal(f->parents[i],
                           grand,
                           false,
                           children,
                           separators,
                           2u,
                           (unsigned char)(0xb0u + i))) {
            return false;
        }
    }

    const uint32_t left_children[2] = {P0_PAGE, P1_PAGE};
    const uint32_t left_keys[1] = {20u};
    const uint32_t right_children[3] = {Q0_PAGE, Q1_PAGE, Q2_PAGE};
    const uint32_t right_keys[2] = {60u,80u};
    const uint32_t root_children[2] = {LEFT_GRAND_PAGE, RIGHT_GRAND_PAGE};
    const uint32_t root_keys[1] = {40u};
    return init_internal(f->left_grand,
                         ROOT_PAGE,
                         false,
                         left_children,
                         left_keys,
                         2u,
                         0xc0u) &&
           init_internal(f->right_grand,
                         ROOT_PAGE,
                         false,
                         right_children,
                         right_keys,
                         3u,
                         0xc1u) &&
           init_internal(f->root,
                         0u,
                         true,
                         root_children,
                         root_keys,
                         2u,
                         0xd0u);
}

static bool internal_matches(const unsigned char page[PAGE_SIZE],
                             uint32_t parent,
                             bool is_root,
                             const uint32_t* children,
                             const uint32_t* separators,
                             uint32_t child_count) {
    if (!tinydb_parent_stage_validate(page, PAGE_SIZE) ||
        (page[IS_ROOT_OFFSET] != 0u) != is_root ||
        read_u32(page + PARENT_POINTER_OFFSET) != parent ||
        read_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET) != child_count - 1u) {
        return false;
    }
    for (uint32_t i = 0u; i < child_count; i++) {
        if (tinydb_parent_stage_child_at(page, i) != children[i]) return false;
        if (i + 1u < child_count &&
            tinydb_parent_stage_key_at(page, i) != separators[i]) {
            return false;
        }
    }
    return true;
}

static bool leaf_matches(const unsigned char page[PAGE_SIZE],
                         uint32_t parent,
                         uint32_t prev_expected,
                         uint32_t next_expected,
                         uint32_t key_expected) {
    uint32_t count = 0u;
    uint32_t key = 0u;
    uint32_t prev = INVALID_PAGE_NUM;
    uint32_t next = INVALID_PAGE_NUM;
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           read_u32(page + PARENT_POINTER_OFFSET) == parent &&
           tinydb_leaf_page_count(page, PAGE_SIZE, &count) && count == 1u &&
           tinydb_leaf_page_key_at(page, PAGE_SIZE, 0u, &key) &&
           key == key_expected &&
           tinydb_leaf_page_prev(page, PAGE_SIZE, &prev) &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next) &&
           prev == prev_expected && next == next_expected;
}

static bool test_success(void) {
    Fixture f;
    if (!seed(&f)) return false;

    unsigned char root_before[PAGE_SIZE];
    unsigned char left_before[PAGE_SIZE];
    unsigned char right_before[PAGE_SIZE];
    unsigned char obsolete_before[PAGE_SIZE];
    unsigned char kept_before[PAGE_SIZE];
    unsigned char donor_before[PAGE_SIZE];
    unsigned char survivor_before[3][PAGE_SIZE];
    unsigned char removed_before[PAGE_SIZE];
    unsigned char donor_leaf_before[2][PAGE_SIZE];
    unsigned char next_leaf_before[PAGE_SIZE];
    memcpy(root_before, f.root, PAGE_SIZE);
    memcpy(left_before, f.left_grand, PAGE_SIZE);
    memcpy(right_before, f.right_grand, PAGE_SIZE);
    memcpy(obsolete_before, f.parents[0], PAGE_SIZE);
    memcpy(kept_before, f.parents[1], PAGE_SIZE);
    memcpy(donor_before, f.parents[2], PAGE_SIZE);
    memcpy(survivor_before[0], f.leaves[0], PAGE_SIZE);
    memcpy(survivor_before[1], f.leaves[1], PAGE_SIZE);
    memcpy(survivor_before[2], f.leaves[3], PAGE_SIZE);
    memcpy(removed_before, f.leaves[2], PAGE_SIZE);
    memcpy(donor_leaf_before[0], f.leaves[4], PAGE_SIZE);
    memcpy(donor_leaf_before[1], f.leaves[5], PAGE_SIZE);
    memcpy(next_leaf_before, f.leaves[6], PAGE_SIZE);

    void* survivors[3] = {f.leaves[0], f.leaves[1], f.leaves[3]};
    const uint32_t survivor_nums[3] = {30u,31u,33u};
    const void* donor_leaves[2] = {f.leaves[4], f.leaves[5]};
    const uint32_t donor_nums[2] = {34u,35u};
    if (!tinydb_stage_internal_merge_borrow_from_right_grandparent(
            f.root, PAGE_SIZE, ROOT_PAGE,
            f.left_grand, PAGE_SIZE, LEFT_GRAND_PAGE,
            f.right_grand, PAGE_SIZE, RIGHT_GRAND_PAGE,
            f.parents[0], PAGE_SIZE, P0_PAGE,
            f.parents[1], PAGE_SIZE, P1_PAGE,
            f.parents[2], PAGE_SIZE, Q0_PAGE,
            f.leaves[2], PAGE_SIZE, 32u, 30u,
            survivors, survivor_nums,
            donor_leaves, donor_nums,
            f.leaves[6], PAGE_SIZE, 36u)) {
        return false;
    }

    const uint32_t root_children[2] = {LEFT_GRAND_PAGE,RIGHT_GRAND_PAGE};
    const uint32_t root_keys[1] = {60u};
    const uint32_t left_children[2] = {P1_PAGE,Q0_PAGE};
    const uint32_t left_keys[1] = {40u};
    const uint32_t right_children[2] = {Q1_PAGE,Q2_PAGE};
    const uint32_t right_keys[1] = {80u};
    const uint32_t kept_children[3] = {30u,31u,33u};
    const uint32_t kept_keys[2] = {10u,20u};
    const uint32_t donor_children[2] = {34u,35u};
    const uint32_t donor_keys[1] = {50u};

    return internal_matches(f.root,0u,true,root_children,root_keys,2u) &&
           internal_matches(f.left_grand,ROOT_PAGE,false,
                            left_children,left_keys,2u) &&
           internal_matches(f.right_grand,ROOT_PAGE,false,
                            right_children,right_keys,2u) &&
           internal_matches(f.parents[1],LEFT_GRAND_PAGE,false,
                            kept_children,kept_keys,3u) &&
           internal_matches(f.parents[2],LEFT_GRAND_PAGE,false,
                            donor_children,donor_keys,2u) &&
           leaf_matches(f.leaves[0],P1_PAGE,0u,31u,10u) &&
           leaf_matches(f.leaves[1],P1_PAGE,30u,33u,20u) &&
           leaf_matches(f.leaves[3],P1_PAGE,31u,34u,40u) &&
           memcmp(f.parents[0],obsolete_before,PAGE_SIZE) == 0 &&
           memcmp(f.leaves[2],removed_before,PAGE_SIZE) == 0 &&
           memcmp(f.leaves[4],donor_leaf_before[0],PAGE_SIZE) == 0 &&
           memcmp(f.leaves[5],donor_leaf_before[1],PAGE_SIZE) == 0 &&
           memcmp(f.leaves[6],next_leaf_before,PAGE_SIZE) == 0 &&
           trailer_same(f.root,root_before) &&
           trailer_same(f.left_grand,left_before) &&
           trailer_same(f.right_grand,right_before) &&
           trailer_same(f.parents[1],kept_before) &&
           trailer_same(f.parents[2],donor_before) &&
           trailer_same(f.leaves[0],survivor_before[0]) &&
           trailer_same(f.leaves[1],survivor_before[1]) &&
           trailer_same(f.leaves[3],survivor_before[2]);
}

static bool test_atomic_failure(void) {
    Fixture f;
    if (!seed(&f)) return false;
    write_u32_le(f.leaves[4] + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, 31u);
    if (!tinydb_slotted_leaf_v2_validate(f.leaves[4], PAGE_SIZE)) return false;

    unsigned char root_before[PAGE_SIZE];
    unsigned char left_before[PAGE_SIZE];
    unsigned char right_before[PAGE_SIZE];
    unsigned char kept_before[PAGE_SIZE];
    unsigned char donor_before[PAGE_SIZE];
    unsigned char survivor_before[3][PAGE_SIZE];
    memcpy(root_before, f.root, PAGE_SIZE);
    memcpy(left_before, f.left_grand, PAGE_SIZE);
    memcpy(right_before, f.right_grand, PAGE_SIZE);
    memcpy(kept_before, f.parents[1], PAGE_SIZE);
    memcpy(donor_before, f.parents[2], PAGE_SIZE);
    memcpy(survivor_before[0], f.leaves[0], PAGE_SIZE);
    memcpy(survivor_before[1], f.leaves[1], PAGE_SIZE);
    memcpy(survivor_before[2], f.leaves[3], PAGE_SIZE);

    void* survivors[3] = {f.leaves[0], f.leaves[1], f.leaves[3]};
    const uint32_t survivor_nums[3] = {30u,31u,33u};
    const void* donor_leaves[2] = {f.leaves[4], f.leaves[5]};
    const uint32_t donor_nums[2] = {34u,35u};
    bool ok = tinydb_stage_internal_merge_borrow_from_right_grandparent(
        f.root, PAGE_SIZE, ROOT_PAGE,
        f.left_grand, PAGE_SIZE, LEFT_GRAND_PAGE,
        f.right_grand, PAGE_SIZE, RIGHT_GRAND_PAGE,
        f.parents[0], PAGE_SIZE, P0_PAGE,
        f.parents[1], PAGE_SIZE, P1_PAGE,
        f.parents[2], PAGE_SIZE, Q0_PAGE,
        f.leaves[2], PAGE_SIZE, 32u, 30u,
        survivors, survivor_nums,
        donor_leaves, donor_nums,
        f.leaves[6], PAGE_SIZE, 36u);
    return !ok &&
           memcmp(f.root,root_before,PAGE_SIZE) == 0 &&
           memcmp(f.left_grand,left_before,PAGE_SIZE) == 0 &&
           memcmp(f.right_grand,right_before,PAGE_SIZE) == 0 &&
           memcmp(f.parents[1],kept_before,PAGE_SIZE) == 0 &&
           memcmp(f.parents[2],donor_before,PAGE_SIZE) == 0 &&
           memcmp(f.leaves[0],survivor_before[0],PAGE_SIZE) == 0 &&
           memcmp(f.leaves[1],survivor_before[1],PAGE_SIZE) == 0 &&
           memcmp(f.leaves[3],survivor_before[2],PAGE_SIZE) == 0;
}

int main(void) {
    if (!test_success() || !test_atomic_failure()) return EXIT_FAILURE;
    printf("INTERNAL_MERGE_BORROW_ROOT_CASCADE_STAGE_OK "
           "bottom_merge=yes upper_borrow=yes root_separator=yes "
           "height_preserved=yes source_immutable=yes trailer=yes "
           "atomic_failure=yes\n");
    return EXIT_SUCCESS;
}
