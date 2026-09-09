#define main tinydb_root_cascade_stage_probe_main
#include "internal_merge_borrow_root_cascade_stage_probe.c"
#undef main

#include "internal_merge_borrow_nonroot_cascade_stage.h"

#define UPPER_PARENT_PAGE 2u

static bool seed_nonroot(Fixture* f) {
    if (!seed(f)) return false;
    f->root[IS_ROOT_OFFSET] = 0u;
    write_u32(f->root + PARENT_POINTER_OFFSET, UPPER_PARENT_PAGE);
    return tinydb_parent_stage_validate(f->root, PAGE_SIZE);
}

static bool test_nonroot_success(void) {
    Fixture f;
    if (!seed_nonroot(&f)) return false;

    unsigned char ancestor_before[PAGE_SIZE];
    unsigned char left_before[PAGE_SIZE];
    unsigned char right_before[PAGE_SIZE];
    unsigned char obsolete_before[PAGE_SIZE];
    unsigned char kept_before[PAGE_SIZE];
    unsigned char donor_before[PAGE_SIZE];
    unsigned char removed_before[PAGE_SIZE];
    unsigned char donor_leaf_before[2][PAGE_SIZE];
    unsigned char next_leaf_before[PAGE_SIZE];
    unsigned char survivor_before[3][PAGE_SIZE];
    memcpy(ancestor_before, f.root, PAGE_SIZE);
    memcpy(left_before, f.left_grand, PAGE_SIZE);
    memcpy(right_before, f.right_grand, PAGE_SIZE);
    memcpy(obsolete_before, f.parents[0], PAGE_SIZE);
    memcpy(kept_before, f.parents[1], PAGE_SIZE);
    memcpy(donor_before, f.parents[2], PAGE_SIZE);
    memcpy(removed_before, f.leaves[2], PAGE_SIZE);
    memcpy(donor_leaf_before[0], f.leaves[4], PAGE_SIZE);
    memcpy(donor_leaf_before[1], f.leaves[5], PAGE_SIZE);
    memcpy(next_leaf_before, f.leaves[6], PAGE_SIZE);
    memcpy(survivor_before[0], f.leaves[0], PAGE_SIZE);
    memcpy(survivor_before[1], f.leaves[1], PAGE_SIZE);
    memcpy(survivor_before[2], f.leaves[3], PAGE_SIZE);

    void* survivors[3] = {f.leaves[0], f.leaves[1], f.leaves[3]};
    const uint32_t survivor_nums[3] = {30u, 31u, 33u};
    const void* donor_leaves[2] = {f.leaves[4], f.leaves[5]};
    const uint32_t donor_nums[2] = {34u, 35u};
    if (!tinydb_stage_internal_merge_borrow_from_right_nonroot_ancestor(
            f.root, PAGE_SIZE, ROOT_PAGE, UPPER_PARENT_PAGE,
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

    const uint32_t ancestor_children[2] = {LEFT_GRAND_PAGE, RIGHT_GRAND_PAGE};
    const uint32_t ancestor_keys[1] = {60u};
    const uint32_t left_children[2] = {P1_PAGE, Q0_PAGE};
    const uint32_t left_keys[1] = {40u};
    const uint32_t right_children[2] = {Q1_PAGE, Q2_PAGE};
    const uint32_t right_keys[1] = {80u};
    const uint32_t kept_children[3] = {30u, 31u, 33u};
    const uint32_t kept_keys[2] = {10u, 20u};
    const uint32_t donor_children[2] = {34u, 35u};
    const uint32_t donor_keys[1] = {50u};

    return internal_matches(f.root, UPPER_PARENT_PAGE, false,
                            ancestor_children, ancestor_keys, 2u) &&
           internal_matches(f.left_grand, ROOT_PAGE, false,
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
           trailer_same(f.root, ancestor_before) &&
           trailer_same(f.left_grand, left_before) &&
           trailer_same(f.right_grand, right_before) &&
           trailer_same(f.parents[1], kept_before) &&
           trailer_same(f.parents[2], donor_before) &&
           trailer_same(f.leaves[0], survivor_before[0]) &&
           trailer_same(f.leaves[1], survivor_before[1]) &&
           trailer_same(f.leaves[3], survivor_before[2]);
}

static bool test_nonroot_atomic_failure(void) {
    Fixture f;
    if (!seed_nonroot(&f)) return false;
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
    bool ok = tinydb_stage_internal_merge_borrow_from_right_nonroot_ancestor(
        f.root, PAGE_SIZE, ROOT_PAGE, UPPER_PARENT_PAGE,
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
    if (!test_nonroot_success() || !test_nonroot_atomic_failure()) {
        return EXIT_FAILURE;
    }
    printf("INTERNAL_MERGE_BORROW_NONROOT_CASCADE_STAGE_OK height5=yes "
           "nonroot_ancestor=yes separator_update=yes ancestor_max_unchanged=yes "
           "source_immutable=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
