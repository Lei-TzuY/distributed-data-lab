#include "slotted_v2_parent_max_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static void seed_parent(unsigned char page[PAGE_SIZE]) {
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 1u;
    write_u32(page + PARENT_POINTER_OFFSET, 0u);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 3u);

    unsigned char* cell0 = page + INTERNAL_NODE_HEADER_SIZE;
    unsigned char* cell1 = cell0 + INTERNAL_NODE_CELL_SIZE;
    unsigned char* cell2 = cell1 + INTERNAL_NODE_CELL_SIZE;
    write_u32(cell0, 11u);
    write_u32(cell0 + INTERNAL_NODE_CHILD_SIZE, 100u);
    write_u32(cell1, 12u);
    write_u32(cell1 + INTERNAL_NODE_CHILD_SIZE, 200u);
    write_u32(cell2, 13u);
    write_u32(cell2 + INTERNAL_NODE_CHILD_SIZE, 300u);
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, 14u);
}

static bool interior_success(void) {
    unsigned char parent[PAGE_SIZE];
    seed_parent(parent);
    memset(parent + PAGE_USABLE_SIZE, 0xA5, PAGE_CHECKSUM_SIZE);

    uint32_t child_index = UINT32_MAX;
    bool changed = false;
    if (!tinydb_stage_parent_child_max_decrease(parent,
                                                 PAGE_SIZE,
                                                 12u,
                                                 200u,
                                                 150u,
                                                 &child_index,
                                                 &changed)) {
        return false;
    }
    return child_index == 1u && changed &&
           tinydb_parent_stage_validate(parent, PAGE_SIZE) &&
           tinydb_parent_stage_key_at(parent, 0u) == 100u &&
           tinydb_parent_stage_key_at(parent, 1u) == 150u &&
           tinydb_parent_stage_key_at(parent, 2u) == 300u &&
           tinydb_parent_stage_child_at(parent, 1u) == 12u &&
           trailer_is(parent, 0xA5u);
}

static bool rightmost_noop(void) {
    unsigned char parent[PAGE_SIZE];
    unsigned char before[PAGE_SIZE];
    seed_parent(parent);
    memset(parent + PAGE_USABLE_SIZE, 0xB6, PAGE_CHECKSUM_SIZE);
    memcpy(before, parent, sizeof(before));

    uint32_t child_index = UINT32_MAX;
    bool changed = true;
    return tinydb_stage_parent_child_max_decrease(parent,
                                                   PAGE_SIZE,
                                                   14u,
                                                   400u,
                                                   350u,
                                                   &child_index,
                                                   &changed) &&
           child_index == 3u && !changed &&
           memcmp(parent, before, PAGE_SIZE) == 0;
}

static bool atomic_failure(void) {
    unsigned char parent[PAGE_SIZE];
    unsigned char before[PAGE_SIZE];
    seed_parent(parent);
    memset(parent + PAGE_USABLE_SIZE, 0xC7, PAGE_CHECKSUM_SIZE);
    memcpy(before, parent, sizeof(before));

    if (tinydb_stage_parent_child_max_decrease(parent,
                                                PAGE_SIZE,
                                                12u,
                                                200u,
                                                90u,
                                                NULL,
                                                NULL)) {
        return false;
    }
    if (memcmp(parent, before, PAGE_SIZE) != 0) return false;

    if (tinydb_stage_parent_child_max_decrease(parent,
                                                PAGE_SIZE,
                                                12u,
                                                201u,
                                                150u,
                                                NULL,
                                                NULL)) {
        return false;
    }
    return memcmp(parent, before, PAGE_SIZE) == 0;
}

int main(void) {
    bool interior = interior_success();
    bool rightmost = rightmost_noop();
    bool atomic = atomic_failure();
    if (!interior || !rightmost || !atomic) {
        fprintf(stderr,
                "interior=%s rightmost=%s atomic=%s\n",
                interior ? "yes" : "no",
                rightmost ? "yes" : "no",
                atomic ? "yes" : "no");
        return EXIT_FAILURE;
    }

    printf("SLOTTED_V2_PARENT_MAX_STAGE_OK interior=yes rightmost_noop=yes "
           "atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
