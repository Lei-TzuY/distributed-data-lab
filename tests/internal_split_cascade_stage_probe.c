#include "internal_split_cascade_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARENT0_PAGE 100u
#define PARENT1_PAGE 200u
#define ROOT_PAGE 300u
#define SPLIT_LEFT_PAGE 1000u
#define SPLIT_RIGHT_PAGE 1001u
#define PARENT0_OLD_MAX 600000u
#define PARENT1_OLD_MAX 1200000u

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static uint32_t read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void fill_trailer(unsigned char page[PAGE_SIZE], unsigned char marker) {
    memset(page + PAGE_USABLE_SIZE, marker, PAGE_SIZE - PAGE_USABLE_SIZE);
}

static bool trailer_is(const unsigned char page[PAGE_SIZE],
                       unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static void init_internal(unsigned char page[PAGE_SIZE],
                          uint32_t parent_page_num,
                          bool is_root,
                          uint32_t key_count) {
    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = is_root ? 1u : 0u;
    write_u32(page + PARENT_POINTER_OFFSET, parent_page_num);
    write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, key_count);
}

static void set_cell(unsigned char page[PAGE_SIZE],
                     uint32_t index,
                     uint32_t child,
                     uint32_t key) {
    unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE +
                          index * INTERNAL_NODE_CELL_SIZE;
    write_u32(cell, child);
    write_u32(cell + INTERNAL_NODE_CHILD_SIZE, key);
}

static bool build_parent0(unsigned char page[PAGE_SIZE],
                          uint32_t parent_page_num,
                          bool is_root) {
    init_internal(page,
                  parent_page_num,
                  is_root,
                  INTERNAL_NODE_MAX_KEYS);
    set_cell(page, 0u, SPLIT_LEFT_PAGE, 1000u);
    for (uint32_t i = 1u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        set_cell(page,
                 i,
                 1100u + i,
                 (i + 1u) * 1000u);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, 1900u);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool build_parent1(unsigned char page[PAGE_SIZE],
                          uint32_t parent_page_num,
                          bool is_root,
                          uint32_t key_count) {
    init_internal(page, parent_page_num, is_root, key_count);
    set_cell(page, 0u, PARENT0_PAGE, PARENT0_OLD_MAX);
    for (uint32_t i = 1u; i < key_count; i++) {
        set_cell(page,
                 i,
                 2100u + i,
                 PARENT0_OLD_MAX + i * 1000u);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, 2900u);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool build_root(unsigned char page[PAGE_SIZE], uint32_t key_count) {
    init_internal(page, 0u, true, key_count);
    set_cell(page, 0u, PARENT1_PAGE, PARENT1_OLD_MAX);
    for (uint32_t i = 1u; i < key_count; i++) {
        set_cell(page,
                 i,
                 3100u + i,
                 PARENT1_OLD_MAX + i * 1000u);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, 3900u);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool full_root_cascade_case(void) {
    unsigned char ancestors[3][PAGE_SIZE];
    unsigned char new_pages[4][PAGE_SIZE];
    memset(ancestors, 0, sizeof(ancestors));
    memset(new_pages, 0, sizeof(new_pages));

    if (!build_parent0(ancestors[0], PARENT1_PAGE, false) ||
        !build_parent1(ancestors[1], ROOT_PAGE, false, INTERNAL_NODE_MAX_KEYS) ||
        !build_root(ancestors[2], INTERNAL_NODE_MAX_KEYS)) {
        return false;
    }

    fill_trailer(ancestors[0], 0xa1u);
    fill_trailer(ancestors[1], 0xa2u);
    fill_trailer(ancestors[2], 0xa3u);
    for (uint32_t i = 0u; i < 4u; i++) {
        fill_trailer(new_pages[i], (unsigned char)(0xb1u + i));
    }

    uint32_t ancestor_nums[3] = {PARENT0_PAGE, PARENT1_PAGE, ROOT_PAGE};
    uint32_t old_maxes[3] = {PARENT0_OLD_MAX, PARENT1_OLD_MAX, 1800000u};
    uint32_t new_nums[4] = {400u, 500u, 600u, 700u};
    uint32_t used = 0u;
    uint32_t stop = UINT32_MAX;
    bool root_grew = false;

    if (!tinydb_stage_internal_split_cascade(
            ancestors,
            PAGE_SIZE,
            ancestor_nums,
            old_maxes,
            3u,
            new_pages,
            PAGE_SIZE,
            new_nums,
            4u,
            SPLIT_LEFT_PAGE,
            SPLIT_RIGHT_PAGE,
            1000u,
            400u,
            1000u,
            &used,
            &stop,
            &root_grew)) {
        return false;
    }

    if (used != 4u || stop != 2u || !root_grew ||
        !tinydb_parent_stage_validate(ancestors[0], PAGE_SIZE) ||
        !tinydb_parent_stage_validate(ancestors[1], PAGE_SIZE) ||
        !tinydb_parent_stage_validate(ancestors[2], PAGE_SIZE) ||
        !tinydb_parent_stage_validate(new_pages[0], PAGE_SIZE) ||
        !tinydb_parent_stage_validate(new_pages[1], PAGE_SIZE) ||
        !tinydb_parent_stage_validate(new_pages[2], PAGE_SIZE) ||
        !tinydb_parent_stage_validate(new_pages[3], PAGE_SIZE) ||
        read_u32(ancestors[0] + INTERNAL_NODE_NUM_KEYS_OFFSET) >=
            INTERNAL_NODE_MAX_KEYS ||
        read_u32(ancestors[1] + INTERNAL_NODE_NUM_KEYS_OFFSET) >=
            INTERNAL_NODE_MAX_KEYS ||
        read_u32(ancestors[2] + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(ancestors[2], 0u) != new_nums[2] ||
        tinydb_parent_stage_child_at(ancestors[2], 1u) != new_nums[3]) {
        return false;
    }

    return trailer_is(ancestors[0], 0xa1u) &&
           trailer_is(ancestors[1], 0xa2u) &&
           trailer_is(ancestors[2], 0xa3u) &&
           trailer_is(new_pages[0], 0xb1u) &&
           trailer_is(new_pages[1], 0xb2u) &&
           trailer_is(new_pages[2], 0xb3u) &&
           trailer_is(new_pages[3], 0xb4u);
}

static bool nonfull_stop_case(void) {
    unsigned char ancestors[2][PAGE_SIZE];
    unsigned char new_pages[1][PAGE_SIZE];
    memset(ancestors, 0, sizeof(ancestors));
    memset(new_pages, 0, sizeof(new_pages));

    if (!build_parent0(ancestors[0], PARENT1_PAGE, false) ||
        !build_parent1(ancestors[1], 0u, true, 1u)) {
        return false;
    }
    fill_trailer(ancestors[0], 0xc1u);
    fill_trailer(ancestors[1], 0xc2u);
    fill_trailer(new_pages[0], 0xc3u);

    uint32_t ancestor_nums[2] = {PARENT0_PAGE, PARENT1_PAGE};
    uint32_t old_maxes[2] = {PARENT0_OLD_MAX, 1500000u};
    uint32_t new_nums[1] = {400u};
    uint32_t used = 0u;
    uint32_t stop = UINT32_MAX;
    bool root_grew = true;

    if (!tinydb_stage_internal_split_cascade(
            ancestors,
            PAGE_SIZE,
            ancestor_nums,
            old_maxes,
            2u,
            new_pages,
            PAGE_SIZE,
            new_nums,
            1u,
            SPLIT_LEFT_PAGE,
            SPLIT_RIGHT_PAGE,
            1000u,
            400u,
            1000u,
            &used,
            &stop,
            &root_grew)) {
        return false;
    }

    if (used != 1u || stop != 1u || root_grew ||
        read_u32(ancestors[1] + INTERNAL_NODE_NUM_KEYS_OFFSET) != 2u ||
        tinydb_parent_stage_child_at(ancestors[1], 0u) != PARENT0_PAGE ||
        tinydb_parent_stage_child_at(ancestors[1], 1u) != new_nums[0] ||
        tinydb_parent_stage_child_at(ancestors[1], 2u) != 2900u ||
        !tinydb_parent_stage_validate(ancestors[1], PAGE_SIZE)) {
        return false;
    }

    return trailer_is(ancestors[0], 0xc1u) &&
           trailer_is(ancestors[1], 0xc2u) &&
           trailer_is(new_pages[0], 0xc3u);
}

static bool atomic_failure_case(void) {
    unsigned char ancestors[3][PAGE_SIZE];
    unsigned char new_pages[3][PAGE_SIZE];
    unsigned char ancestors_before[3][PAGE_SIZE];
    unsigned char new_before[3][PAGE_SIZE];
    memset(ancestors, 0, sizeof(ancestors));
    memset(new_pages, 0, sizeof(new_pages));

    if (!build_parent0(ancestors[0], PARENT1_PAGE, false) ||
        !build_parent1(ancestors[1], ROOT_PAGE, false, INTERNAL_NODE_MAX_KEYS) ||
        !build_root(ancestors[2], INTERNAL_NODE_MAX_KEYS)) {
        return false;
    }
    for (uint32_t i = 0u; i < 3u; i++) {
        fill_trailer(ancestors[i], (unsigned char)(0xd1u + i));
        fill_trailer(new_pages[i], (unsigned char)(0xe1u + i));
    }
    memcpy(ancestors_before, ancestors, sizeof(ancestors));
    memcpy(new_before, new_pages, sizeof(new_pages));

    uint32_t ancestor_nums[3] = {PARENT0_PAGE, PARENT1_PAGE, ROOT_PAGE};
    uint32_t old_maxes[3] = {PARENT0_OLD_MAX, PARENT1_OLD_MAX, 1800000u};
    uint32_t new_nums[3] = {400u, 500u, 600u};
    uint32_t used = 99u;
    uint32_t stop = 99u;
    bool root_grew = true;

    if (tinydb_stage_internal_split_cascade(
            ancestors,
            PAGE_SIZE,
            ancestor_nums,
            old_maxes,
            3u,
            new_pages,
            PAGE_SIZE,
            new_nums,
            3u,
            SPLIT_LEFT_PAGE,
            SPLIT_RIGHT_PAGE,
            1000u,
            400u,
            1000u,
            &used,
            &stop,
            &root_grew)) {
        return false;
    }

    return used == 0u && stop == 0u && !root_grew &&
           memcmp(ancestors, ancestors_before, sizeof(ancestors)) == 0 &&
           memcmp(new_pages, new_before, sizeof(new_pages)) == 0;
}

int main(void) {
    bool cascade = full_root_cascade_case();
    bool stop = nonfull_stop_case();
    bool atomic = atomic_failure_case();

    if (!cascade || !stop || !atomic) {
        fprintf(stderr,
                "cascade=%s stop=%s atomic=%s\n",
                cascade ? "yes" : "no",
                stop ? "yes" : "no",
                atomic ? "yes" : "no");
        return EXIT_FAILURE;
    }

    printf("INTERNAL_SPLIT_CASCADE_STAGE_OK cascade=yes nonfull_stop=yes "
           "atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}
