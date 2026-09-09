#include "leaf_format.h"
#include "slotted_leaf_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void make_v1_leaf(unsigned char page[PAGE_SIZE], uint32_t count) {
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_LEAF;
    page[IS_ROOT_OFFSET] = 1u;
    memcpy(page + LEAF_NODE_NUM_CELLS_OFFSET, &count, sizeof(count));
}

int main(void) {
    if (!tinydb_leaf_format_v2_marker_disjoint_from_v1()) {
        fprintf(stderr, "V2 marker overlaps a legal V1 num_cells encoding\n");
        return EXIT_FAILURE;
    }

    unsigned char v1[PAGE_SIZE];
    make_v1_leaf(v1, 0u);
    if (tinydb_leaf_format_detect_page(v1, sizeof(v1)) !=
        TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        fprintf(stderr, "empty V1 leaf was not detected as V1\n");
        return EXIT_FAILURE;
    }

    make_v1_leaf(v1, LEAF_NODE_MAX_CELLS);
    if (tinydb_leaf_format_detect_page(v1, sizeof(v1)) !=
        TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        fprintf(stderr, "full V1 leaf was not detected as V1\n");
        return EXIT_FAILURE;
    }

    make_v1_leaf(v1, LEAF_NODE_MAX_CELLS + 1u);
    if (tinydb_leaf_format_detect_page(v1, sizeof(v1)) !=
        TINYDB_LEAF_PAGE_FORMAT_UNKNOWN) {
        fprintf(stderr, "invalid V1 cell count was accepted\n");
        return EXIT_FAILURE;
    }

    unsigned char v2[PAGE_SIZE];
    memset(v2, 0, sizeof(v2));
    if (!tinydb_slotted_leaf_v2_init(v2, sizeof(v2)) ||
        tinydb_leaf_format_detect_page(v2, sizeof(v2)) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        fprintf(stderr, "valid V2 leaf was not detected as V2\n");
        return EXIT_FAILURE;
    }

    unsigned char corrupt_v2[PAGE_SIZE];
    memcpy(corrupt_v2, v2, sizeof(corrupt_v2));
    corrupt_v2[TINYDB_SLOTTED_V2_VERSION_OFFSET] ^= 0x01u;
    if (tinydb_leaf_format_detect_page(corrupt_v2, sizeof(corrupt_v2)) !=
        TINYDB_LEAF_PAGE_FORMAT_UNKNOWN) {
        fprintf(stderr, "corrupt V2 marker/version fell back to V1\n");
        return EXIT_FAILURE;
    }

    unsigned char fake_marker[PAGE_SIZE];
    make_v1_leaf(fake_marker, 0u);
    fake_marker[TINYDB_SLOTTED_V2_MAGIC_OFFSET + 0u] = 0x54u;
    fake_marker[TINYDB_SLOTTED_V2_MAGIC_OFFSET + 1u] = 0x4cu;
    fake_marker[TINYDB_SLOTTED_V2_MAGIC_OFFSET + 2u] = 0x46u;
    fake_marker[TINYDB_SLOTTED_V2_MAGIC_OFFSET + 3u] = 0x32u;
    if (tinydb_leaf_format_detect_page(fake_marker, sizeof(fake_marker)) !=
        TINYDB_LEAF_PAGE_FORMAT_UNKNOWN) {
        fprintf(stderr, "partial V2 page with marker was accepted as V1\n");
        return EXIT_FAILURE;
    }

    unsigned char internal[PAGE_SIZE];
    memset(internal, 0, sizeof(internal));
    internal[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    if (tinydb_leaf_format_detect_page(internal, sizeof(internal)) !=
            TINYDB_LEAF_PAGE_FORMAT_UNKNOWN ||
        tinydb_leaf_format_detect_page(v2, PAGE_SIZE - 1u) !=
            TINYDB_LEAF_PAGE_FORMAT_UNKNOWN) {
        fprintf(stderr, "non-leaf or truncated page was accepted\n");
        return EXIT_FAILURE;
    }

    printf("LEAF_FORMAT_DISCRIMINATOR_OK v1=yes v2=yes disjoint=yes corrupt_fail_closed=yes\n");
    return EXIT_SUCCESS;
}
