#include "internal_root_height_right_outer_left_lower_stage.h"
#include "slotted_leaf_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static bool build_leaf(unsigned char page[PAGE_SIZE],
                       uint32_t parent,
                       uint32_t prev,
                       uint32_t next,
                       uint32_t key,
                       unsigned char trailer) {
    const unsigned char payload[1] = {(unsigned char)(key & 0xffu)};
    memset(page, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent);
    write_u32(page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, prev);
    write_u32(page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, next);
    if (!tinydb_slotted_leaf_v2_insert(page, PAGE_SIZE, key, payload, 1u)) {
        return false;
    }
    memset(page + PAGE_USABLE_SIZE, trailer, PAGE_SIZE - PAGE_USABLE_SIZE);
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE);
}

static bool build_parent(unsigned char page[PAGE_SIZE],
                         uint32_t parent,
                         uint32_t left,
                         uint32_t left_max,
                         uint32_t right,
                         unsigned char trailer) {
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    write_u32(page + INTERNAL_NODE_HEADER_SIZE, left);
    write_u32(page + INTERNAL_NODE_HEADER_SIZE + INTERNAL_NODE_CHILD_SIZE,
              left_max);
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, right);
    memset(page + PAGE_USABLE_SIZE, trailer, PAGE_SIZE - PAGE_USABLE_SIZE);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char value) {
    for (size_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != value) return false;
    }
    return true;
}

int main(void) {
    enum {
        OLD_PARENT = 5u,
        NEW_PARENT = 6u,
        OBSOLETE = 10u,
        KEPT = 11u,
        A = 20u,
        B = 21u,
        REMOVED = 22u,
        D = 23u,
        PREV_EXTERNAL = 19u
    };

    unsigned char obsolete[PAGE_SIZE], kept[PAGE_SIZE], removed[PAGE_SIZE];
    unsigned char a[PAGE_SIZE], b[PAGE_SIZE], d[PAGE_SIZE];
    if (!build_leaf(a, OBSOLETE, PREV_EXTERNAL, B, 50u, 0xa1u) ||
        !build_leaf(b, OBSOLETE, A, REMOVED, 60u, 0xa2u) ||
        !build_leaf(removed, KEPT, B, D, 70u, 0xa3u) ||
        !build_leaf(d, KEPT, REMOVED, 0u, 80u, 0xa4u) ||
        !build_parent(obsolete, OLD_PARENT, A, 50u, B, 0xb1u) ||
        !build_parent(kept, OLD_PARENT, REMOVED, 70u, D, 0xb2u)) {
        return EXIT_FAILURE;
    }

    unsigned char obsolete_before[PAGE_SIZE], removed_before[PAGE_SIZE];
    memcpy(obsolete_before, obsolete, PAGE_SIZE);
    memcpy(removed_before, removed, PAGE_SIZE);
    void* survivors[3] = {a, b, d};
    const uint32_t survivor_nums[3] = {A, B, D};
    if (!tinydb_stage_root_height_right_outer_left_lower_merge(
            obsolete, PAGE_SIZE, OBSOLETE,
            kept, PAGE_SIZE, KEPT,
            OLD_PARENT, NEW_PARENT,
            removed, PAGE_SIZE, REMOVED, 70u,
            survivors, survivor_nums)) {
        return EXIT_FAILURE;
    }

    uint32_t b_next = INVALID_PAGE_NUM;
    uint32_t d_prev = INVALID_PAGE_NUM;
    if (memcmp(obsolete, obsolete_before, PAGE_SIZE) != 0 ||
        memcmp(removed, removed_before, PAGE_SIZE) != 0 ||
        tinydb_parent_stage_read_u32(kept + PARENT_POINTER_OFFSET) != NEW_PARENT ||
        tinydb_parent_stage_read_u32(kept + INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        tinydb_parent_stage_child_at(kept, 0u) != A ||
        tinydb_parent_stage_key_at(kept, 0u) != 50u ||
        tinydb_parent_stage_child_at(kept, 1u) != B ||
        tinydb_parent_stage_key_at(kept, 1u) != 60u ||
        tinydb_parent_stage_child_at(kept, 2u) != D ||
        tinydb_parent_stage_read_u32(a + PARENT_POINTER_OFFSET) != KEPT ||
        tinydb_parent_stage_read_u32(b + PARENT_POINTER_OFFSET) != KEPT ||
        tinydb_parent_stage_read_u32(d + PARENT_POINTER_OFFSET) != KEPT ||
        !tinydb_leaf_page_next(b, PAGE_SIZE, &b_next) || b_next != D ||
        !tinydb_leaf_page_prev(d, PAGE_SIZE, &d_prev) || d_prev != B ||
        !trailer_is(a, 0xa1u) || !trailer_is(b, 0xa2u) ||
        !trailer_is(d, 0xa4u) || !trailer_is(kept, 0xb2u)) {
        return EXIT_FAILURE;
    }

    unsigned char kept_bad[PAGE_SIZE], a_bad[PAGE_SIZE], b_bad[PAGE_SIZE];
    unsigned char d_bad[PAGE_SIZE], kept_bad_before[PAGE_SIZE];
    unsigned char b_bad_before[PAGE_SIZE], d_bad_before[PAGE_SIZE];
    memcpy(kept_bad, kept, PAGE_SIZE);
    memcpy(a_bad, a, PAGE_SIZE);
    memcpy(b_bad, b, PAGE_SIZE);
    memcpy(d_bad, d, PAGE_SIZE);
    write_u32(d_bad + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, 999u);
    memcpy(kept_bad_before, kept_bad, PAGE_SIZE);
    memcpy(b_bad_before, b_bad, PAGE_SIZE);
    memcpy(d_bad_before, d_bad, PAGE_SIZE);
    void* bad_survivors[3] = {a_bad, b_bad, d_bad};
    if (tinydb_stage_root_height_right_outer_left_lower_merge(
            obsolete_before, PAGE_SIZE, OBSOLETE,
            kept_bad, PAGE_SIZE, KEPT,
            OLD_PARENT, NEW_PARENT,
            removed_before, PAGE_SIZE, REMOVED, 70u,
            bad_survivors, survivor_nums) ||
        memcmp(kept_bad, kept_bad_before, PAGE_SIZE) != 0 ||
        memcmp(b_bad, b_bad_before, PAGE_SIZE) != 0 ||
        memcmp(d_bad, d_bad_before, PAGE_SIZE) != 0) {
        return EXIT_FAILURE;
    }

    printf("INTERNAL_ROOT_HEIGHT_RIGHT_OUTER_LEFT_LOWER_STAGE_OK final_orientation=yes "
           "shared_boundary_relink=yes source_immutable=yes reparent=yes "
           "atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
