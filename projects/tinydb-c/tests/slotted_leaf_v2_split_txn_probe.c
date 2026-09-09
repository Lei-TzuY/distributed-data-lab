#include "slotted_leaf_v2_split_txn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_u32(const unsigned char* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_u32(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static bool seed_leaf(unsigned char page[PAGE_SIZE],
                      unsigned char trailer,
                      uint32_t parent,
                      uint32_t prev,
                      uint32_t next) {
    memset(page, trailer, PAGE_SIZE);
    if (!tinydb_slotted_leaf_v2_init(page, PAGE_SIZE)) return false;
    page[IS_ROOT_OFFSET] = 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent);
    write_u32(page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, prev);
    write_u32(page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, next);
    return true;
}

int main(void) {
    unsigned char left[PAGE_SIZE];
    unsigned char right[PAGE_SIZE];
    unsigned char old_next[PAGE_SIZE];
    const unsigned char a[700] = {1u};
    const unsigned char b[900] = {2u};
    const unsigned char c[600] = {3u};
    const unsigned char d[1000] = {4u};

    if (!seed_leaf(left, 0xA1u, 80u, 40u, 60u) ||
        !seed_leaf(old_next, 0xC3u, 80u, 50u, 70u)) {
        return EXIT_FAILURE;
    }
    memset(right, 0xB2, sizeof(right));

    if (!tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 10u, a, sizeof(a)) ||
        !tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 20u, b, sizeof(b)) ||
        !tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 30u, c, sizeof(c)) ||
        !tinydb_slotted_leaf_v2_insert(left, PAGE_SIZE, 40u, d, sizeof(d)) ||
        !tinydb_slotted_leaf_v2_insert(old_next, PAGE_SIZE, 50u, a, sizeof(a))) {
        return EXIT_FAILURE;
    }

    uint16_t split_index = 0u;
    if (!tinydb_slotted_leaf_v2_split_nonroot_with_next(left,
                                                         PAGE_SIZE,
                                                         50u,
                                                         right,
                                                         PAGE_SIZE,
                                                         51u,
                                                         old_next,
                                                         PAGE_SIZE,
                                                         60u,
                                                         &split_index) ||
        split_index == 0u ||
        !tinydb_slotted_leaf_v2_validate(left, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(old_next, PAGE_SIZE) ||
        read_u32(left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != 51u ||
        read_u32(right + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) != 50u ||
        read_u32(right + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != 60u ||
        read_u32(old_next + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) != 51u ||
        !trailer_is(left, 0xA1u) ||
        !trailer_is(right, 0xB2u) ||
        !trailer_is(old_next, 0xC3u)) {
        fprintf(stderr, "atomic sibling repair split failed\n");
        return EXIT_FAILURE;
    }

    unsigned char bad_left[PAGE_SIZE];
    unsigned char bad_right[PAGE_SIZE];
    unsigned char bad_next[PAGE_SIZE];
    unsigned char before_left[PAGE_SIZE];
    unsigned char before_right[PAGE_SIZE];
    unsigned char before_next[PAGE_SIZE];
    if (!seed_leaf(bad_left, 0x11u, 90u, 0u, 62u) ||
        !seed_leaf(bad_next, 0x33u, 90u, 999u, 0u)) {
        return EXIT_FAILURE;
    }
    memset(bad_right, 0x22, sizeof(bad_right));
    if (!tinydb_slotted_leaf_v2_insert(bad_left, PAGE_SIZE, 1u, a, sizeof(a)) ||
        !tinydb_slotted_leaf_v2_insert(bad_left, PAGE_SIZE, 2u, b, sizeof(b))) {
        return EXIT_FAILURE;
    }
    memcpy(before_left, bad_left, PAGE_SIZE);
    memcpy(before_right, bad_right, PAGE_SIZE);
    memcpy(before_next, bad_next, PAGE_SIZE);

    if (tinydb_slotted_leaf_v2_split_nonroot_with_next(bad_left,
                                                        PAGE_SIZE,
                                                        61u,
                                                        bad_right,
                                                        PAGE_SIZE,
                                                        63u,
                                                        bad_next,
                                                        PAGE_SIZE,
                                                        62u,
                                                        NULL) ||
        memcmp(bad_left, before_left, PAGE_SIZE) != 0 ||
        memcmp(bad_right, before_right, PAGE_SIZE) != 0 ||
        memcmp(bad_next, before_next, PAGE_SIZE) != 0) {
        fprintf(stderr, "corrupt backlink failure was not atomic\n");
        return EXIT_FAILURE;
    }

    printf("SLOTTED_SPLIT_TXN_OK backlink_repair=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
