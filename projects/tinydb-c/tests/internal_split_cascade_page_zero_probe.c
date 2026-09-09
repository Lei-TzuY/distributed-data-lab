#include "internal_split_cascade_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARENT_PAGE 100u
#define SPLIT_LEFT_PAGE 1000u
#define SPLIT_RIGHT_PAGE 1001u
#define PARENT_OLD_MAX 600000u

static void write_u32(unsigned char* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static uint32_t read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
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

static bool build_full_parent(unsigned char page[PAGE_SIZE]) {
    init_internal(page, 0u, false, INTERNAL_NODE_MAX_KEYS);
    set_cell(page, 0u, SPLIT_LEFT_PAGE, 1000u);
    for (uint32_t i = 1u; i < INTERNAL_NODE_MAX_KEYS; i++) {
        set_cell(page, i, 1100u + i, (i + 1u) * 1000u);
    }
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, 1900u);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool build_page_zero_root(unsigned char page[PAGE_SIZE]) {
    init_internal(page, 0u, true, 1u);
    set_cell(page, 0u, PARENT_PAGE, PARENT_OLD_MAX);
    write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET, 2900u);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

static bool valid_page_zero_root_case(void) {
    unsigned char ancestors[2][PAGE_SIZE];
    unsigned char new_pages[1][PAGE_SIZE];
    memset(ancestors, 0, sizeof(ancestors));
    memset(new_pages, 0, sizeof(new_pages));
    if (!build_full_parent(ancestors[0]) ||
        !build_page_zero_root(ancestors[1])) {
        return false;
    }

    uint32_t ancestor_nums[2] = {PARENT_PAGE, 0u};
    uint32_t old_maxes[2] = {PARENT_OLD_MAX, 1500000u};
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

    return used == 1u && stop == 1u && !root_grew &&
           tinydb_parent_stage_validate(ancestors[0], PAGE_SIZE) &&
           tinydb_parent_stage_validate(ancestors[1], PAGE_SIZE) &&
           tinydb_parent_stage_validate(new_pages[0], PAGE_SIZE) &&
           read_u32(ancestors[1] + INTERNAL_NODE_NUM_KEYS_OFFSET) == 2u &&
           tinydb_parent_stage_child_at(ancestors[1], 0u) == PARENT_PAGE &&
           tinydb_parent_stage_child_at(ancestors[1], 1u) == new_nums[0] &&
           tinydb_parent_stage_child_at(ancestors[1], 2u) == 2900u;
}

static bool invalid_intermediate_zero_is_atomic(void) {
    unsigned char ancestors[2][PAGE_SIZE];
    unsigned char new_pages[1][PAGE_SIZE];
    unsigned char ancestors_before[2][PAGE_SIZE];
    unsigned char new_before[1][PAGE_SIZE];
    memset(ancestors, 0, sizeof(ancestors));
    memset(new_pages, 0, sizeof(new_pages));
    if (!build_full_parent(ancestors[0]) ||
        !build_page_zero_root(ancestors[1])) {
        return false;
    }
    memcpy(ancestors_before, ancestors, sizeof(ancestors));
    memcpy(new_before, new_pages, sizeof(new_pages));

    uint32_t ancestor_nums[2] = {0u, 300u};
    uint32_t old_maxes[2] = {PARENT_OLD_MAX, 1500000u};
    uint32_t new_nums[1] = {400u};
    uint32_t used = 99u;
    uint32_t stop = 99u;
    bool root_grew = true;

    if (tinydb_stage_internal_split_cascade(
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

    return used == 0u && stop == 0u && !root_grew &&
           memcmp(ancestors, ancestors_before, sizeof(ancestors)) == 0 &&
           memcmp(new_pages, new_before, sizeof(new_pages)) == 0;
}

int main(void) {
    bool valid = valid_page_zero_root_case();
    bool atomic = invalid_intermediate_zero_is_atomic();
    if (!valid || !atomic) {
        fprintf(stderr,
                "page_zero_root=%s intermediate_zero_atomic=%s\n",
                valid ? "yes" : "no",
                atomic ? "yes" : "no");
        return EXIT_FAILURE;
    }
    printf("INTERNAL_SPLIT_CASCADE_PAGE_ZERO_OK root_zero=yes intermediate_zero_rejected=yes atomic=yes\n");
    return EXIT_SUCCESS;
}
