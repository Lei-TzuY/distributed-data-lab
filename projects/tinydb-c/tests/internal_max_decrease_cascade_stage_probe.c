#include "internal_max_decrease_cascade_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEVELS 3u

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static void seed_node(unsigned char page[PAGE_SIZE],
                      uint32_t parent_page_num,
                      bool is_root,
                      uint32_t left_child,
                      uint32_t left_max,
                      uint32_t right_child,
                      unsigned char trailer) {
    memset(page, 0, PAGE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = is_root ? 1u : 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE;
    write_u32(cell, left_child);
    write_u32(cell + INTERNAL_NODE_CHILD_SIZE, left_max);
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, right_child);
    memset(page + PAGE_USABLE_SIZE, trailer, PAGE_CHECKSUM_SIZE);
}

static bool trailer_is(const unsigned char page[PAGE_SIZE], unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static void seed_chain(unsigned char pages[LEVELS][PAGE_SIZE],
                       bool root_path_rightmost) {
    /* changed leaf 301 is rightmost in 101; 101 is rightmost in 201. */
    seed_node(pages[0], 201u, false, 302u, 100u, 301u, 0xA1u);
    seed_node(pages[1], 0u, false, 202u, 200u, 101u, 0xB2u);
    if (root_path_rightmost) {
        seed_node(pages[2], 0u, true, 999u, 100u, 201u, 0xC3u);
    } else {
        seed_node(pages[2], 0u, true, 201u, 300u, 999u, 0xC3u);
    }
}

static bool separator_stop_case(void) {
    unsigned char pages[LEVELS][PAGE_SIZE];
    const uint32_t page_nums[LEVELS] = {101u, 201u, 0u};
    seed_chain(pages, false);

    uint32_t stopped = UINT32_MAX;
    bool changed = false;
    if (!tinydb_stage_internal_max_decrease_cascade(pages,
                                                     PAGE_SIZE,
                                                     page_nums,
                                                     LEVELS,
                                                     301u,
                                                     300u,
                                                     250u,
                                                     &stopped,
                                                     &changed)) {
        return false;
    }
    return stopped == 2u && changed &&
           tinydb_parent_stage_key_at(pages[2], 0u) == 250u &&
           tinydb_parent_stage_child_at(pages[2], 0u) == 201u &&
           tinydb_parent_stage_key_at(pages[0], 0u) == 100u &&
           tinydb_parent_stage_key_at(pages[1], 0u) == 200u &&
           trailer_is(pages[0], 0xA1u) &&
           trailer_is(pages[1], 0xB2u) &&
           trailer_is(pages[2], 0xC3u);
}

static bool root_rightmost_case(void) {
    unsigned char pages[LEVELS][PAGE_SIZE];
    unsigned char before[LEVELS][PAGE_SIZE];
    const uint32_t page_nums[LEVELS] = {101u, 201u, 0u};
    seed_chain(pages, true);
    memcpy(before, pages, sizeof(before));

    uint32_t stopped = UINT32_MAX;
    bool changed = true;
    return tinydb_stage_internal_max_decrease_cascade(pages,
                                                       PAGE_SIZE,
                                                       page_nums,
                                                       LEVELS,
                                                       301u,
                                                       300u,
                                                       250u,
                                                       &stopped,
                                                       &changed) &&
           stopped == 2u && !changed &&
           memcmp(pages, before, sizeof(pages)) == 0;
}

static bool atomic_failure_case(void) {
    unsigned char pages[LEVELS][PAGE_SIZE];
    unsigned char before[LEVELS][PAGE_SIZE];
    const uint32_t page_nums[LEVELS] = {101u, 201u, 0u};
    seed_chain(pages, false);
    /* Root separator no longer matches the old propagated maximum. */
    write_u32(pages[2] + INTERNAL_NODE_HEADER_SIZE + INTERNAL_NODE_CHILD_SIZE,
              301u);
    memcpy(before, pages, sizeof(before));

    if (tinydb_stage_internal_max_decrease_cascade(pages,
                                                    PAGE_SIZE,
                                                    page_nums,
                                                    LEVELS,
                                                    301u,
                                                    300u,
                                                    250u,
                                                    NULL,
                                                    NULL)) {
        return false;
    }
    if (memcmp(pages, before, sizeof(pages)) != 0) return false;

    unsigned char short_chain[2][PAGE_SIZE];
    const uint32_t short_nums[2] = {101u, 201u};
    seed_node(short_chain[0], 201u, false, 302u, 100u, 301u, 0xD4u);
    seed_node(short_chain[1], 0u, false, 202u, 200u, 101u, 0xE5u);
    unsigned char short_before[2][PAGE_SIZE];
    memcpy(short_before, short_chain, sizeof(short_before));
    return !tinydb_stage_internal_max_decrease_cascade(short_chain,
                                                        PAGE_SIZE,
                                                        short_nums,
                                                        2u,
                                                        301u,
                                                        300u,
                                                        250u,
                                                        NULL,
                                                        NULL) &&
           memcmp(short_chain, short_before, sizeof(short_chain)) == 0;
}

int main(void) {
    bool separator = separator_stop_case();
    bool root_rightmost = root_rightmost_case();
    bool atomic = atomic_failure_case();
    if (!separator || !root_rightmost || !atomic) {
        fprintf(stderr,
                "separator=%s root_rightmost=%s atomic=%s\n",
                separator ? "yes" : "no",
                root_rightmost ? "yes" : "no",
                atomic ? "yes" : "no");
        return EXIT_FAILURE;
    }

    printf("INTERNAL_MAX_DECREASE_CASCADE_OK separator_stop=yes "
           "root_rightmost=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
