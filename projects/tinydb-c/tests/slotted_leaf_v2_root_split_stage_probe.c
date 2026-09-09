#include "slotted_leaf_v2_root_split_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static bool seed_root(unsigned char page[PAGE_SIZE]) {
    memset(page, 0, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 1u;
    tinydb_root_split_stage_write_u32(page + PARENT_POINTER_OFFSET, 0u);
    for (uint32_t key = 10u; key <= 80u; key += 10u) {
        unsigned char payload[32];
        memset(payload, (int)(key & 0xffu), sizeof(payload));
        if (!tinydb_slotted_leaf_v2_insert(page,
                                           PAGE_SIZE,
                                           key,
                                           payload,
                                           (uint16_t)sizeof(payload))) {
            return false;
        }
    }
    return tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE);
}

static bool success_case(void) {
    unsigned char root[PAGE_SIZE];
    unsigned char left[PAGE_SIZE];
    unsigned char right[PAGE_SIZE];
    memset(left, 0, sizeof(left));
    memset(right, 0, sizeof(right));
    if (!seed_root(root)) return false;
    memset(root + PAGE_USABLE_SIZE, 0xa1, PAGE_SIZE - PAGE_USABLE_SIZE);
    memset(left + PAGE_USABLE_SIZE, 0xb2, PAGE_SIZE - PAGE_USABLE_SIZE);
    memset(right + PAGE_USABLE_SIZE, 0xc3, PAGE_SIZE - PAGE_USABLE_SIZE);

    uint32_t separator = 0u;
    if (!tinydb_slotted_leaf_v2_stage_root_split(root,
                                                  PAGE_SIZE,
                                                  0u,
                                                  left,
                                                  PAGE_SIZE,
                                                  5u,
                                                  right,
                                                  PAGE_SIZE,
                                                  6u,
                                                  &separator)) {
        return false;
    }

    uint16_t left_count = tinydb_slotted_leaf_v2_count(left, PAGE_SIZE);
    uint16_t right_count = tinydb_slotted_leaf_v2_count(right, PAGE_SIZE);
    uint32_t left_max = 0u;
    uint32_t right_min = 0u;
    if (left_count == 0u || right_count == 0u ||
        !tinydb_leaf_page_key_at(left,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &left_max) ||
        !tinydb_leaf_page_key_at(right, PAGE_SIZE, 0u, &right_min)) {
        return false;
    }

    return get_node_type(root) == NODE_INTERNAL && root[IS_ROOT_OFFSET] != 0u &&
           tinydb_parent_stage_validate(root, PAGE_SIZE) &&
           tinydb_parent_stage_child_at(root, 0u) == 5u &&
           tinydb_parent_stage_child_at(root, 1u) == 6u &&
           tinydb_parent_stage_key_at(root, 0u) == separator &&
           separator == left_max && left_max < right_min &&
           tinydb_slotted_leaf_v2_validate(left, PAGE_SIZE) &&
           tinydb_slotted_leaf_v2_validate(right, PAGE_SIZE) &&
           tinydb_slotted_leaf_v2_count(left, PAGE_SIZE) +
                   tinydb_slotted_leaf_v2_count(right, PAGE_SIZE) ==
               8u &&
           tinydb_slotted_split_read_u32(
               left + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) == 0u &&
           tinydb_slotted_split_read_u32(
               left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) == 6u &&
           tinydb_slotted_split_read_u32(
               right + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) == 5u &&
           tinydb_slotted_split_read_u32(
               right + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) == 0u &&
           tinydb_slotted_split_read_u32(left + PARENT_POINTER_OFFSET) == 0u &&
           tinydb_slotted_split_read_u32(right + PARENT_POINTER_OFFSET) == 0u &&
           trailer_is(root, 0xa1u) && trailer_is(left, 0xb2u) &&
           trailer_is(right, 0xc3u);
}

static bool atomic_failure_case(void) {
    unsigned char root[PAGE_SIZE];
    unsigned char left[PAGE_SIZE];
    unsigned char right[PAGE_SIZE];
    unsigned char before_root[PAGE_SIZE];
    unsigned char before_left[PAGE_SIZE];
    unsigned char before_right[PAGE_SIZE];
    memset(left, 0x44, sizeof(left));
    memset(right, 0x55, sizeof(right));
    if (!seed_root(root)) return false;
    tinydb_slotted_split_write_u32(
        root + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
        99u);
    memcpy(before_root, root, sizeof(root));
    memcpy(before_left, left, sizeof(left));
    memcpy(before_right, right, sizeof(right));

    if (tinydb_slotted_leaf_v2_stage_root_split(root,
                                                 PAGE_SIZE,
                                                 0u,
                                                 left,
                                                 PAGE_SIZE,
                                                 5u,
                                                 right,
                                                 PAGE_SIZE,
                                                 6u,
                                                 NULL)) {
        return false;
    }
    return memcmp(root, before_root, PAGE_SIZE) == 0 &&
           memcmp(left, before_left, PAGE_SIZE) == 0 &&
           memcmp(right, before_right, PAGE_SIZE) == 0;
}

int main(void) {
    bool success = success_case();
    bool atomic = atomic_failure_case();
    if (!success || !atomic) {
        fprintf(stderr,
                "success=%s atomic=%s\n",
                success ? "yes" : "no",
                atomic ? "yes" : "no");
        return EXIT_FAILURE;
    }

    printf("SLOTTED_V2_ROOT_SPLIT_STAGE_OK root0=yes topology=yes "
           "atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
