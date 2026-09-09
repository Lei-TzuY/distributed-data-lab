#include "leaf_sibling_relink_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u32_native(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char value) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != value) return false;
    }
    return true;
}

static bool v2_case(void) {
    unsigned char page[PAGE_SIZE];
    memset(page, 0, sizeof(page));
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    tinydb_leaf_relink_write_u32_le(
        page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, 11u);
    tinydb_leaf_relink_write_u32_le(
        page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, 22u);
    memset(page + PAGE_USABLE_SIZE, 0xA1, PAGE_CHECKSUM_SIZE);

    if (!tinydb_stage_leaf_sibling_relink(page,
                                           PAGE_SIZE,
                                           true,
                                           22u,
                                           33u)) {
        return false;
    }
    uint32_t next = 0u;
    uint32_t prev = 0u;
    return tinydb_leaf_page_next(page, PAGE_SIZE, &next) && next == 33u &&
           tinydb_leaf_page_prev(page, PAGE_SIZE, &prev) && prev == 11u &&
           tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           trailer_is(page, 0xA1u);
}

static bool v1_case(void) {
    unsigned char page[PAGE_SIZE];
    memset(page, 0, sizeof(page));
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_LEAF;
    write_u32_native(page + LEAF_NODE_NUM_CELLS_OFFSET, 0u);
    write_u32_native(page + LEAF_NODE_PREV_LEAF_OFFSET, 44u);
    write_u32_native(page + LEAF_NODE_NEXT_LEAF_OFFSET, 66u);
    memset(page + PAGE_USABLE_SIZE, 0xB2, PAGE_CHECKSUM_SIZE);

    if (!tinydb_stage_leaf_sibling_relink(page,
                                           PAGE_SIZE,
                                           false,
                                           44u,
                                           55u)) {
        return false;
    }
    uint32_t next = 0u;
    uint32_t prev = 0u;
    return tinydb_leaf_page_prev(page, PAGE_SIZE, &prev) && prev == 55u &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next) && next == 66u &&
           tinydb_leaf_format_detect_page(page, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 &&
           trailer_is(page, 0xB2u);
}

static bool boundary_case(void) {
    unsigned char page[PAGE_SIZE];
    memset(page, 0, sizeof(page));
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    tinydb_leaf_relink_write_u32_le(
        page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, 77u);
    return tinydb_stage_leaf_sibling_relink(page,
                                            PAGE_SIZE,
                                            true,
                                            77u,
                                            0u) &&
           tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE);
}

static bool failure_atomic_case(void) {
    unsigned char page[PAGE_SIZE];
    unsigned char before[PAGE_SIZE];
    memset(page, 0, sizeof(page));
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    tinydb_leaf_relink_write_u32_le(
        page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, 88u);
    memset(page + PAGE_USABLE_SIZE, 0xC3, PAGE_CHECKSUM_SIZE);
    memcpy(before, page, sizeof(before));

    return !tinydb_stage_leaf_sibling_relink(page,
                                              PAGE_SIZE,
                                              false,
                                              87u,
                                              99u) &&
           memcmp(page, before, PAGE_SIZE) == 0;
}

int main(void) {
    bool v2 = v2_case();
    bool v1 = v1_case();
    bool boundary = boundary_case();
    bool atomic = failure_atomic_case();
    if (!v2 || !v1 || !boundary || !atomic) {
        fprintf(stderr,
                "v2=%s v1=%s boundary=%s atomic=%s\n",
                v2 ? "yes" : "no",
                v1 ? "yes" : "no",
                boundary ? "yes" : "no",
                atomic ? "yes" : "no");
        return EXIT_FAILURE;
    }

    printf("LEAF_SIBLING_RELINK_STAGE_OK v2=yes v1=yes boundary=yes "
           "atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
