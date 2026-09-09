#include "internal_nonroot_merge_stage.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRANDPARENT_PAGE 70u
#define ANCESTOR_PAGE 7u
#define PARENT0_PAGE 20u
#define PARENT1_PAGE 21u
#define PARENT2_PAGE 22u
#define LEAF0_PAGE 30u
#define LEAF1_PAGE 31u
#define LEAF2_PAGE 32u
#define LEAF3_PAGE 33u
#define LEAF4_PAGE 34u
#define LEAF5_PAGE 35u

typedef struct {
    unsigned char ancestor[PAGE_SIZE];
    unsigned char parents[3][PAGE_SIZE];
    unsigned char leaves[6][PAGE_SIZE];
} Fixture;

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
    value[2] = 0x6du;
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
                          const uint32_t* children,
                          const uint32_t* separators,
                          uint32_t child_count,
                          unsigned char trailer) {
    if (page == NULL || children == NULL || separators == NULL ||
        child_count < 2u) {
        return false;
    }
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, child_count - 1u);
    for (uint32_t i = 0u; i + 1u < child_count; i++) {
        unsigned char* cell =
            page + INTERNAL_NODE_HEADER_SIZE +
            (size_t)i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, children[i]);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, separators[i]);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
              children[child_count - 1u]);
    if (!tinydb_parent_stage_validate(page, PAGE_SIZE)) return false;
    stamp_trailer(page, trailer);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool seed(Fixture* fixture) {
    if (fixture == NULL) return false;
    memset(fixture, 0, sizeof(*fixture));
    const uint32_t parent_pages[3] = {
        PARENT0_PAGE, PARENT1_PAGE, PARENT2_PAGE
    };
    const uint32_t leaf_pages[6] = {
        LEAF0_PAGE, LEAF1_PAGE, LEAF2_PAGE,
        LEAF3_PAGE, LEAF4_PAGE, LEAF5_PAGE
    };
    const uint32_t keys[6] = {10u, 20u, 30u, 40u, 50u, 60u};
    for (uint32_t i = 0u; i < 6u; i++) {
        if (!init_leaf(fixture->leaves[i],
                       parent_pages[i / 2u],
                       i == 0u ? 0u : leaf_pages[i - 1u],
                       i == 5u ? 0u : leaf_pages[i + 1u],
                       keys[i],
                       (unsigned char)(0xa0u + i))) {
            return false;
        }
    }

    const uint32_t p0_children[2] = {LEAF0_PAGE, LEAF1_PAGE};
    const uint32_t p0_keys[1] = {10u};
    const uint32_t p1_children[2] = {LEAF2_PAGE, LEAF3_PAGE};
    const uint32_t p1_keys[1] = {30u};
    const uint32_t p2_children[2] = {LEAF4_PAGE, LEAF5_PAGE};
    const uint32_t p2_keys[1] = {50u};
    const uint32_t ancestor_children[3] = {
        PARENT0_PAGE, PARENT1_PAGE, PARENT2_PAGE
    };
    const uint32_t ancestor_keys[2] = {20u, 40u};
    return init_internal(fixture->parents[0], ANCESTOR_PAGE,
                         p0_children, p0_keys, 2u, 0xb0u) &&
           init_internal(fixture->parents[1], ANCESTOR_PAGE,
                         p1_children, p1_keys, 2u, 0xb1u) &&
           init_internal(fixture->parents[2], ANCESTOR_PAGE,
                         p2_children, p2_keys, 2u, 0xb2u) &&
           init_internal(fixture->ancestor, GRANDPARENT_PAGE,
                         ancestor_children, ancestor_keys, 3u, 0xc0u);
}

static bool internal_matches(const unsigned char page[PAGE_SIZE],
                             uint32_t expected_parent,
                             const uint32_t* children,
                             const uint32_t* separators,
                             uint32_t child_count) {
    if (!tinydb_parent_stage_validate(page, PAGE_SIZE) ||
        page[IS_ROOT_OFFSET] != 0u ||
        read_u32(page + PARENT_POINTER_OFFSET) != expected_parent ||
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
                         uint32_t expected_parent,
                         uint32_t expected_prev,
                         uint32_t expected_next,
                         uint32_t expected_key) {
    uint32_t count = 0u;
    uint32_t key = 0u;
    uint32_t prev = INVALID_PAGE_NUM;
    uint32_t next = INVALID_PAGE_NUM;
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           read_u32(page + PARENT_POINTER_OFFSET) == expected_parent &&
           tinydb_leaf_page_count(page, PAGE_SIZE, &count) && count == 1u &&
           tinydb_leaf_page_key_at(page, PAGE_SIZE, 0u, &key) &&
           key == expected_key &&
           tinydb_leaf_page_prev(page, PAGE_SIZE, &prev) &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next) &&
           prev == expected_prev && next == expected_next;
}

static bool test_merge_from_right(void) {
    Fixture f;
    if (!seed(&f)) return false;
    unsigned char ancestor_before[PAGE_SIZE];
    unsigned char obsolete_before[PAGE_SIZE];
    unsigned char removed_before[PAGE_SIZE];
    unsigned char kept_before[PAGE_SIZE];
    unsigned char survivor_before[3][PAGE_SIZE];
    unsigned char unrelated_before[PAGE_SIZE];
    memcpy(ancestor_before, f.ancestor, PAGE_SIZE);
    memcpy(obsolete_before, f.parents[1], PAGE_SIZE);
    memcpy(removed_before, f.leaves[3], PAGE_SIZE);
    memcpy(kept_before, f.parents[2], PAGE_SIZE);
    memcpy(unrelated_before, f.parents[0], PAGE_SIZE);
    memcpy(survivor_before[0], f.leaves[2], PAGE_SIZE);
    memcpy(survivor_before[1], f.leaves[4], PAGE_SIZE);
    memcpy(survivor_before[2], f.leaves[5], PAGE_SIZE);

    void* survivors[3] = {f.leaves[2], f.leaves[4], f.leaves[5]};
    const uint32_t survivor_nums[3] = {LEAF2_PAGE, LEAF4_PAGE, LEAF5_PAGE};
    uint32_t kept = INVALID_PAGE_NUM;
    uint32_t obsolete = INVALID_PAGE_NUM;
    if (!tinydb_stage_internal_nonroot_merge_after_v2_leaf_removal(
            f.ancestor, PAGE_SIZE, ANCESTOR_PAGE,
            f.parents[1], PAGE_SIZE, PARENT1_PAGE,
            f.parents[2], PAGE_SIZE, PARENT2_PAGE,
            f.leaves[3], PAGE_SIZE, LEAF3_PAGE, 40u,
            survivors, survivor_nums, &kept, &obsolete)) {
        return false;
    }

    const uint32_t ancestor_children[2] = {PARENT0_PAGE, PARENT2_PAGE};
    const uint32_t ancestor_keys[1] = {20u};
    const uint32_t kept_children[3] = {LEAF2_PAGE, LEAF4_PAGE, LEAF5_PAGE};
    const uint32_t kept_keys[2] = {30u, 50u};
    return kept == PARENT2_PAGE && obsolete == PARENT1_PAGE &&
           internal_matches(f.ancestor, GRANDPARENT_PAGE,
                            ancestor_children, ancestor_keys, 2u) &&
           internal_matches(f.parents[2], ANCESTOR_PAGE,
                            kept_children, kept_keys, 3u) &&
           leaf_matches(f.leaves[2], PARENT2_PAGE,
                        LEAF1_PAGE, LEAF4_PAGE, 30u) &&
           leaf_matches(f.leaves[4], PARENT2_PAGE,
                        LEAF2_PAGE, LEAF5_PAGE, 50u) &&
           leaf_matches(f.leaves[5], PARENT2_PAGE,
                        LEAF4_PAGE, 0u, 60u) &&
           memcmp(f.parents[1], obsolete_before, PAGE_SIZE) == 0 &&
           memcmp(f.leaves[3], removed_before, PAGE_SIZE) == 0 &&
           memcmp(f.parents[0], unrelated_before, PAGE_SIZE) == 0 &&
           trailer_matches(f.ancestor, ancestor_before) &&
           trailer_matches(f.parents[2], kept_before) &&
           trailer_matches(f.leaves[2], survivor_before[0]) &&
           trailer_matches(f.leaves[4], survivor_before[1]) &&
           trailer_matches(f.leaves[5], survivor_before[2]);
}

static bool test_merge_from_left(void) {
    Fixture f;
    if (!seed(&f)) return false;
    unsigned char ancestor_before[PAGE_SIZE];
    unsigned char obsolete_before[PAGE_SIZE];
    unsigned char removed_before[PAGE_SIZE];
    unsigned char kept_before[PAGE_SIZE];
    unsigned char survivor_before[3][PAGE_SIZE];
    unsigned char unrelated_before[PAGE_SIZE];
    memcpy(ancestor_before, f.ancestor, PAGE_SIZE);
    memcpy(obsolete_before, f.parents[0], PAGE_SIZE);
    memcpy(removed_before, f.leaves[2], PAGE_SIZE);
    memcpy(kept_before, f.parents[1], PAGE_SIZE);
    memcpy(unrelated_before, f.parents[2], PAGE_SIZE);
    memcpy(survivor_before[0], f.leaves[0], PAGE_SIZE);
    memcpy(survivor_before[1], f.leaves[1], PAGE_SIZE);
    memcpy(survivor_before[2], f.leaves[3], PAGE_SIZE);

    void* survivors[3] = {f.leaves[0], f.leaves[1], f.leaves[3]};
    const uint32_t survivor_nums[3] = {LEAF0_PAGE, LEAF1_PAGE, LEAF3_PAGE};
    uint32_t kept = INVALID_PAGE_NUM;
    uint32_t obsolete = INVALID_PAGE_NUM;
    if (!tinydb_stage_internal_nonroot_merge_after_v2_leaf_removal(
            f.ancestor, PAGE_SIZE, ANCESTOR_PAGE,
            f.parents[1], PAGE_SIZE, PARENT1_PAGE,
            f.parents[0], PAGE_SIZE, PARENT0_PAGE,
            f.leaves[2], PAGE_SIZE, LEAF2_PAGE, 30u,
            survivors, survivor_nums, &kept, &obsolete)) {
        return false;
    }

    const uint32_t ancestor_children[2] = {PARENT1_PAGE, PARENT2_PAGE};
    const uint32_t ancestor_keys[1] = {40u};
    const uint32_t kept_children[3] = {LEAF0_PAGE, LEAF1_PAGE, LEAF3_PAGE};
    const uint32_t kept_keys[2] = {10u, 20u};
    return kept == PARENT1_PAGE && obsolete == PARENT0_PAGE &&
           internal_matches(f.ancestor, GRANDPARENT_PAGE,
                            ancestor_children, ancestor_keys, 2u) &&
           internal_matches(f.parents[1], ANCESTOR_PAGE,
                            kept_children, kept_keys, 3u) &&
           leaf_matches(f.leaves[0], PARENT1_PAGE, 0u, LEAF1_PAGE, 10u) &&
           leaf_matches(f.leaves[1], PARENT1_PAGE,
                        LEAF0_PAGE, LEAF3_PAGE, 20u) &&
           leaf_matches(f.leaves[3], PARENT1_PAGE,
                        LEAF1_PAGE, LEAF4_PAGE, 40u) &&
           memcmp(f.parents[0], obsolete_before, PAGE_SIZE) == 0 &&
           memcmp(f.leaves[2], removed_before, PAGE_SIZE) == 0 &&
           memcmp(f.parents[2], unrelated_before, PAGE_SIZE) == 0 &&
           trailer_matches(f.ancestor, ancestor_before) &&
           trailer_matches(f.parents[1], kept_before) &&
           trailer_matches(f.leaves[0], survivor_before[0]) &&
           trailer_matches(f.leaves[1], survivor_before[1]) &&
           trailer_matches(f.leaves[3], survivor_before[2]);
}

static bool test_atomic_failure(void) {
    Fixture f;
    if (!seed(&f)) return false;
    write_u32_le(f.leaves[4] + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
                 LEAF1_PAGE);
    if (!tinydb_slotted_leaf_v2_validate(f.leaves[4], PAGE_SIZE)) return false;

    unsigned char ancestor_before[PAGE_SIZE];
    unsigned char kept_before[PAGE_SIZE];
    unsigned char leaf2_before[PAGE_SIZE];
    unsigned char leaf4_before[PAGE_SIZE];
    unsigned char leaf5_before[PAGE_SIZE];
    memcpy(ancestor_before, f.ancestor, PAGE_SIZE);
    memcpy(kept_before, f.parents[2], PAGE_SIZE);
    memcpy(leaf2_before, f.leaves[2], PAGE_SIZE);
    memcpy(leaf4_before, f.leaves[4], PAGE_SIZE);
    memcpy(leaf5_before, f.leaves[5], PAGE_SIZE);

    void* survivors[3] = {f.leaves[2], f.leaves[4], f.leaves[5]};
    const uint32_t survivor_nums[3] = {LEAF2_PAGE, LEAF4_PAGE, LEAF5_PAGE};
    if (tinydb_stage_internal_nonroot_merge_after_v2_leaf_removal(
            f.ancestor, PAGE_SIZE, ANCESTOR_PAGE,
            f.parents[1], PAGE_SIZE, PARENT1_PAGE,
            f.parents[2], PAGE_SIZE, PARENT2_PAGE,
            f.leaves[3], PAGE_SIZE, LEAF3_PAGE, 40u,
            survivors, survivor_nums, NULL, NULL)) {
        return false;
    }
    return memcmp(f.ancestor, ancestor_before, PAGE_SIZE) == 0 &&
           memcmp(f.parents[2], kept_before, PAGE_SIZE) == 0 &&
           memcmp(f.leaves[2], leaf2_before, PAGE_SIZE) == 0 &&
           memcmp(f.leaves[4], leaf4_before, PAGE_SIZE) == 0 &&
           memcmp(f.leaves[5], leaf5_before, PAGE_SIZE) == 0;
}

int main(void) {
    bool right = test_merge_from_right();
    bool left = test_merge_from_left();
    bool atomic = test_atomic_failure();
    if (!right || !left || !atomic) {
        fprintf(stderr,
                "non-root internal merge staging failed: right=%s left=%s atomic=%s\n",
                right ? "yes" : "no",
                left ? "yes" : "no",
                atomic ? "yes" : "no");
        return EXIT_FAILURE;
    }

    printf("INTERNAL_NONROOT_MERGE_STAGE_OK right=yes left=yes nonroot=yes ");
    printf("ancestor_child_remove=yes ancestor_max_stable=yes grandparent_untouched=yes ");
    printf("survivor_reparent=yes cross_parent_relink=yes third_subtree=yes ");
    printf("source_immutable=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
