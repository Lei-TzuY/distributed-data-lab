#define main tinydb_root_cascade_window_fixture_main
#include "internal_merge_borrow_root_cascade_stage_probe.c"
#undef main

#include "internal_merge_borrow_nonroot_window_stage.h"

#define WINDOW_PARENT_PAGE 2u
#define CONTROL_LEFT_PAGE 70u
#define CONTROL_RIGHT_PAGE 71u
#define PAIR_INDEX 1u

static bool build_wide_ancestor(Fixture* f) {
    if (!seed(f)) return false;
    memset(f->root, 0, PAGE_USABLE_SIZE);
    f->root[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    f->root[IS_ROOT_OFFSET] = 0u;
    write_u32(f->root + PARENT_POINTER_OFFSET, WINDOW_PARENT_PAGE);
    write_u32(f->root + INTERNAL_NODE_NUM_KEYS_OFFSET, 3u);

    unsigned char* c0 = f->root + INTERNAL_NODE_HEADER_SIZE;
    write_u32(c0, CONTROL_LEFT_PAGE);
    write_u32(c0 + INTERNAL_NODE_CHILD_SIZE, 20u);
    unsigned char* c1 = c0 + INTERNAL_NODE_CELL_SIZE;
    write_u32(c1, LEFT_GRAND_PAGE);
    write_u32(c1 + INTERNAL_NODE_CHILD_SIZE, 40u);
    unsigned char* c2 = c1 + INTERNAL_NODE_CELL_SIZE;
    write_u32(c2, RIGHT_GRAND_PAGE);
    write_u32(c2 + INTERNAL_NODE_CHILD_SIZE, 100u);
    write_u32(f->root + INTERNAL_NODE_RIGHT_CHILD_OFFSET, CONTROL_RIGHT_PAGE);
    return tinydb_parent_stage_validate(f->root, PAGE_SIZE);
}

static bool test_inner_window_success(void) {
    Fixture f;
    if (!build_wide_ancestor(&f)) return false;

    unsigned char ancestor_before[PAGE_SIZE];
    unsigned char obsolete_before[PAGE_SIZE];
    unsigned char removed_before[PAGE_SIZE];
    unsigned char donor_leaf_before[2][PAGE_SIZE];
    unsigned char next_leaf_before[PAGE_SIZE];
    memcpy(ancestor_before, f.root, PAGE_SIZE);
    memcpy(obsolete_before, f.parents[0], PAGE_SIZE);
    memcpy(removed_before, f.leaves[2], PAGE_SIZE);
    memcpy(donor_leaf_before[0], f.leaves[4], PAGE_SIZE);
    memcpy(donor_leaf_before[1], f.leaves[5], PAGE_SIZE);
    memcpy(next_leaf_before, f.leaves[6], PAGE_SIZE);

    void* survivors[3] = {f.leaves[0], f.leaves[1], f.leaves[3]};
    const uint32_t survivor_nums[3] = {30u, 31u, 33u};
    const void* donor_leaves[2] = {f.leaves[4], f.leaves[5]};
    const uint32_t donor_nums[2] = {34u, 35u};
    if (!tinydb_stage_internal_merge_borrow_window_from_right(
            f.root, PAGE_SIZE, ROOT_PAGE, WINDOW_PARENT_PAGE, PAIR_INDEX,
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

    if (!tinydb_parent_stage_validate(f.root, PAGE_SIZE) ||
        f.root[IS_ROOT_OFFSET] != 0u ||
        read_u32(f.root + PARENT_POINTER_OFFSET) != WINDOW_PARENT_PAGE ||
        read_u32(f.root + INTERNAL_NODE_NUM_KEYS_OFFSET) != 3u ||
        tinydb_parent_stage_child_at(f.root, 0u) != CONTROL_LEFT_PAGE ||
        tinydb_parent_stage_key_at(f.root, 0u) != 20u ||
        tinydb_parent_stage_child_at(f.root, 1u) != LEFT_GRAND_PAGE ||
        tinydb_parent_stage_key_at(f.root, 1u) != 60u ||
        tinydb_parent_stage_child_at(f.root, 2u) != RIGHT_GRAND_PAGE ||
        tinydb_parent_stage_key_at(f.root, 2u) != 100u ||
        tinydb_parent_stage_child_at(f.root, 3u) != CONTROL_RIGHT_PAGE) {
        return false;
    }

    const uint32_t left_children[2] = {P1_PAGE, Q0_PAGE};
    const uint32_t left_keys[1] = {40u};
    const uint32_t right_children[2] = {Q1_PAGE, Q2_PAGE};
    const uint32_t right_keys[1] = {80u};
    const uint32_t kept_children[3] = {30u, 31u, 33u};
    const uint32_t kept_keys[2] = {10u, 20u};
    const uint32_t donor_children[2] = {34u, 35u};
    const uint32_t donor_keys[1] = {50u};
    return internal_matches(f.left_grand, ROOT_PAGE, false,
                            left_children, left_keys, 2u) &&
           internal_matches(f.right_grand, ROOT_PAGE, false,
                            right_children, right_keys, 2u) &&
           internal_matches(f.parents[1], LEFT_GRAND_PAGE, false,
                            kept_children, kept_keys, 3u) &&
           internal_matches(f.parents[2], LEFT_GRAND_PAGE, false,
                            donor_children, donor_keys, 2u) &&
           leaf_matches(f.leaves[0], P1_PAGE, 0u, 31u, 10u) &&
           leaf_matches(f.leaves[1], P1_PAGE, 30u, 33u, 20u) &&
           leaf_matches(f.leaves[3], P1_PAGE, 31u, 34u, 40u) &&
           memcmp(f.parents[0], obsolete_before, PAGE_SIZE) == 0 &&
           memcmp(f.leaves[2], removed_before, PAGE_SIZE) == 0 &&
           memcmp(f.leaves[4], donor_leaf_before[0], PAGE_SIZE) == 0 &&
           memcmp(f.leaves[5], donor_leaf_before[1], PAGE_SIZE) == 0 &&
           memcmp(f.leaves[6], next_leaf_before, PAGE_SIZE) == 0 &&
           memcmp(f.root + INTERNAL_NODE_HEADER_SIZE,
                  ancestor_before + INTERNAL_NODE_HEADER_SIZE,
                  INTERNAL_NODE_CELL_SIZE) == 0 &&
           memcmp(f.root + INTERNAL_NODE_HEADER_SIZE + 2u * INTERNAL_NODE_CELL_SIZE,
                  ancestor_before + INTERNAL_NODE_HEADER_SIZE + 2u * INTERNAL_NODE_CELL_SIZE,
                  INTERNAL_NODE_CELL_SIZE) == 0 &&
           trailer_same(f.root, ancestor_before);
}

static bool test_inner_window_atomic_failure(void) {
    Fixture f;
    if (!build_wide_ancestor(&f)) return false;
    write_u32_le(f.leaves[4] + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, 31u);
    if (!tinydb_slotted_leaf_v2_validate(f.leaves[4], PAGE_SIZE)) return false;

    unsigned char ancestor_before[PAGE_SIZE];
    unsigned char left_before[PAGE_SIZE];
    unsigned char right_before[PAGE_SIZE];
    unsigned char kept_before[PAGE_SIZE];
    unsigned char donor_before[PAGE_SIZE];
    unsigned char survivor_before[3][PAGE_SIZE];
    memcpy(ancestor_before, f.root, PAGE_SIZE);
    memcpy(left_before, f.left_grand, PAGE_SIZE);
    memcpy(right_before, f.right_grand, PAGE_SIZE);
    memcpy(kept_before, f.parents[1], PAGE_SIZE);
    memcpy(donor_before, f.parents[2], PAGE_SIZE);
    memcpy(survivor_before[0], f.leaves[0], PAGE_SIZE);
    memcpy(survivor_before[1], f.leaves[1], PAGE_SIZE);
    memcpy(survivor_before[2], f.leaves[3], PAGE_SIZE);

    void* survivors[3] = {f.leaves[0], f.leaves[1], f.leaves[3]};
    const uint32_t survivor_nums[3] = {30u, 31u, 33u};
    const void* donor_leaves[2] = {f.leaves[4], f.leaves[5]};
    const uint32_t donor_nums[2] = {34u, 35u};
    bool ok = tinydb_stage_internal_merge_borrow_window_from_right(
        f.root, PAGE_SIZE, ROOT_PAGE, WINDOW_PARENT_PAGE, PAIR_INDEX,
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
           memcmp(f.root, ancestor_before, PAGE_SIZE) == 0 &&
           memcmp(f.left_grand, left_before, PAGE_SIZE) == 0 &&
           memcmp(f.right_grand, right_before, PAGE_SIZE) == 0 &&
           memcmp(f.parents[1], kept_before, PAGE_SIZE) == 0 &&
           memcmp(f.parents[2], donor_before, PAGE_SIZE) == 0 &&
           memcmp(f.leaves[0], survivor_before[0], PAGE_SIZE) == 0 &&
           memcmp(f.leaves[1], survivor_before[1], PAGE_SIZE) == 0 &&
           memcmp(f.leaves[3], survivor_before[2], PAGE_SIZE) == 0;
}

int main(void) {
    if (!test_inner_window_success() || !test_inner_window_atomic_failure()) {
        return EXIT_FAILURE;
    }
    printf("INTERNAL_MERGE_BORROW_NONROOT_WINDOW_STAGE_OK height5=yes "
           "inner_pair=yes pair_index=1 wider_ancestor=yes right_donor=yes "
           "control_siblings_unchanged=yes separator_local=yes "
           "ancestor_max_unchanged=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
