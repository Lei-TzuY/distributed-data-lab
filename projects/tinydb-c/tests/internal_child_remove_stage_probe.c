#include "internal_child_remove_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static void seed_parent(unsigned char page[PAGE_SIZE], uint32_t key_count) {
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 1u;
    write_u32(page + PARENT_POINTER_OFFSET, 0u);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, key_count);
    for (uint32_t i = 0u; i < key_count; i++) {
        unsigned char* cell =
            page + INTERNAL_NODE_HEADER_SIZE + i * INTERNAL_NODE_CELL_SIZE;
        write_u32(cell, 11u + i);
        write_u32(cell + INTERNAL_NODE_CHILD_SIZE, 100u * (i + 1u));
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, 11u + key_count);
}

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static bool interior_case(void) {
    unsigned char parent[PAGE_SIZE];
    seed_parent(parent, 3u);
    memset(parent + PAGE_USABLE_SIZE, 0xA1, PAGE_CHECKSUM_SIZE);

    uint32_t removed_index = UINT32_MAX;
    bool max_changed = true;
    uint32_t new_parent_max = UINT32_MAX;
    if (!tinydb_stage_internal_child_remove(parent,
                                             PAGE_SIZE,
                                             12u,
                                             200u,
                                             &removed_index,
                                             &max_changed,
                                             &new_parent_max)) {
        return false;
    }
    return removed_index == 1u && !max_changed && new_parent_max == 0u &&
           tinydb_parent_stage_validate(parent, PAGE_SIZE) &&
           tinydb_parent_stage_read_u32(parent + INTERNAL_NODE_NUM_KEYS_OFFSET) == 2u &&
           tinydb_parent_stage_child_at(parent, 0u) == 11u &&
           tinydb_parent_stage_child_at(parent, 1u) == 13u &&
           tinydb_parent_stage_child_at(parent, 2u) == 14u &&
           tinydb_parent_stage_key_at(parent, 0u) == 100u &&
           tinydb_parent_stage_key_at(parent, 1u) == 300u &&
           trailer_is(parent, 0xA1u);
}

static bool rightmost_case(void) {
    unsigned char parent[PAGE_SIZE];
    seed_parent(parent, 3u);
    memset(parent + PAGE_USABLE_SIZE, 0xB2, PAGE_CHECKSUM_SIZE);

    uint32_t removed_index = UINT32_MAX;
    bool max_changed = false;
    uint32_t new_parent_max = 0u;
    if (!tinydb_stage_internal_child_remove(parent,
                                             PAGE_SIZE,
                                             14u,
                                             400u,
                                             &removed_index,
                                             &max_changed,
                                             &new_parent_max)) {
        return false;
    }
    return removed_index == 3u && max_changed && new_parent_max == 300u &&
           tinydb_parent_stage_validate(parent, PAGE_SIZE) &&
           tinydb_parent_stage_read_u32(parent + INTERNAL_NODE_NUM_KEYS_OFFSET) == 2u &&
           tinydb_parent_stage_child_at(parent, 0u) == 11u &&
           tinydb_parent_stage_child_at(parent, 1u) == 12u &&
           tinydb_parent_stage_child_at(parent, 2u) == 13u &&
           tinydb_parent_stage_key_at(parent, 0u) == 100u &&
           tinydb_parent_stage_key_at(parent, 1u) == 200u &&
           trailer_is(parent, 0xB2u);
}

static bool atomic_failure_case(void) {
    unsigned char parent[PAGE_SIZE];
    unsigned char before[PAGE_SIZE];
    seed_parent(parent, 3u);
    memset(parent + PAGE_USABLE_SIZE, 0xC3, PAGE_CHECKSUM_SIZE);
    memcpy(before, parent, sizeof(before));

    if (tinydb_stage_internal_child_remove(parent,
                                            PAGE_SIZE,
                                            12u,
                                            201u,
                                            NULL,
                                            NULL,
                                            NULL) ||
        memcmp(parent, before, PAGE_SIZE) != 0) {
        return false;
    }

    seed_parent(parent, 1u);
    memset(parent + PAGE_USABLE_SIZE, 0xD4, PAGE_CHECKSUM_SIZE);
    memcpy(before, parent, sizeof(before));
    return !tinydb_stage_internal_child_remove(parent,
                                                PAGE_SIZE,
                                                11u,
                                                100u,
                                                NULL,
                                                NULL,
                                                NULL) &&
           memcmp(parent, before, PAGE_SIZE) == 0;
}

int main(void) {
    bool interior = interior_case();
    bool rightmost = rightmost_case();
    bool atomic = atomic_failure_case();
    if (!interior || !rightmost || !atomic) {
        fprintf(stderr,
                "interior=%s rightmost=%s atomic=%s\n",
                interior ? "yes" : "no",
                rightmost ? "yes" : "no",
                atomic ? "yes" : "no");
        return EXIT_FAILURE;
    }

    printf("INTERNAL_CHILD_REMOVE_STAGE_OK interior=yes rightmost=yes "
           "underflow_guard=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
