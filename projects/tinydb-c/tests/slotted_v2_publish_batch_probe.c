#include "slotted_v2_publish_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEEP_BATCH_PAGES 64u
#define PREVIOUS_BATCH_LIMIT 32u

static int page_equals(const unsigned char* page,
                       unsigned char value,
                       unsigned char checksum_value) {
    for (uint32_t i = 0u; i < PAGE_USABLE_SIZE; i++) {
        if (page[i] != value) return 0;
    }
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != checksum_value) return 0;
    }
    return 1;
}

static unsigned char* page_at(unsigned char* pages, uint32_t index) {
    return pages + (size_t)index * PAGE_SIZE;
}

static int exercise_deep_batch(void) {
    unsigned char* pages = (unsigned char*)malloc(
        (size_t)DEEP_BATCH_PAGES * PAGE_SIZE);
    unsigned char* staged = (unsigned char*)malloc(
        (size_t)DEEP_BATCH_PAGES * PAGE_SIZE);
    TinyDBV2PublishEntry* entries = (TinyDBV2PublishEntry*)calloc(
        DEEP_BATCH_PAGES, sizeof(TinyDBV2PublishEntry));
    int result = 0;

    if (pages == NULL || staged == NULL || entries == NULL) {
        fprintf(stderr, "unable to allocate deep publication probe\n");
        result = 10;
        goto done;
    }

    size_t snapshot_bytes = 0u;
    if (!tinydb_v2_publish_batch_snapshot_size(DEEP_BATCH_PAGES,
                                                &snapshot_bytes) ||
        snapshot_bytes != (size_t)DEEP_BATCH_PAGES * PAGE_USABLE_SIZE ||
        TINYDB_V2_PUBLISH_BATCH_MAX_PAGES <= PREVIOUS_BATCH_LIMIT) {
        fprintf(stderr, "publish rollback budget does not cover deep cascade\n");
        result = 11;
        goto done;
    }

    for (uint32_t i = 0u; i < DEEP_BATCH_PAGES; i++) {
        unsigned char before_value = (unsigned char)(0x20u + (i % 64u));
        unsigned char staged_value = (unsigned char)(0x60u + (i % 64u));
        unsigned char checksum_value = (unsigned char)(0xA0u + (i % 64u));
        unsigned char* page = page_at(pages, i);
        unsigned char* staged_page = page_at(staged, i);
        memset(page, before_value, PAGE_USABLE_SIZE);
        memset(page + PAGE_USABLE_SIZE,
               checksum_value,
               PAGE_CHECKSUM_SIZE);
        memset(staged_page, staged_value, PAGE_SIZE);
        entries[i].page_num = 100u + i;
        entries[i].target = page;
        entries[i].staged = staged_page;
    }

    /* Fail only after crossing the former 32-page publication ceiling. */
    if (tinydb_v2_publish_batch(entries,
                                DEEP_BATCH_PAGES,
                                PREVIOUS_BATCH_LIMIT + 1u)) {
        fprintf(stderr, "deep injected publication unexpectedly succeeded\n");
        result = 12;
        goto done;
    }
    for (uint32_t i = 0u; i < DEEP_BATCH_PAGES; i++) {
        if (!page_equals(page_at(pages, i),
                         (unsigned char)(0x20u + (i % 64u)),
                         (unsigned char)(0xA0u + (i % 64u)))) {
            fprintf(stderr, "deep rollback did not restore page %u\n", i);
            result = 13;
            goto done;
        }
    }

    if (!tinydb_v2_publish_batch(entries,
                                 DEEP_BATCH_PAGES,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "deep atomic publication failed\n");
        result = 14;
        goto done;
    }
    for (uint32_t i = 0u; i < DEEP_BATCH_PAGES; i++) {
        if (!page_equals(page_at(pages, i),
                         (unsigned char)(0x60u + (i % 64u)),
                         (unsigned char)(0xA0u + (i % 64u)))) {
            fprintf(stderr, "deep publication mismatch on page %u\n", i);
            result = 15;
            goto done;
        }
    }

    {
        uint32_t over_budget_pages =
            (uint32_t)TINYDB_V2_PUBLISH_BATCH_MAX_PAGES + 1u;
        if (tinydb_v2_publish_batch_snapshot_size(over_budget_pages, NULL)) {
            fprintf(stderr, "over-budget publication shape was accepted\n");
            result = 16;
            goto done;
        }
    }

done:
    free(entries);
    free(staged);
    free(pages);
    return result;
}

int main(void) {
    unsigned char page_a[PAGE_SIZE];
    unsigned char page_b[PAGE_SIZE];
    unsigned char page_c[PAGE_SIZE];
    unsigned char staged_a[PAGE_SIZE];
    unsigned char staged_b[PAGE_SIZE];
    unsigned char staged_c[PAGE_SIZE];

    memset(page_a, 0x11, PAGE_USABLE_SIZE);
    memset(page_b, 0x22, PAGE_USABLE_SIZE);
    memset(page_c, 0x33, PAGE_USABLE_SIZE);
    memset(page_a + PAGE_USABLE_SIZE, 0xA1, PAGE_CHECKSUM_SIZE);
    memset(page_b + PAGE_USABLE_SIZE, 0xB2, PAGE_CHECKSUM_SIZE);
    memset(page_c + PAGE_USABLE_SIZE, 0xC3, PAGE_CHECKSUM_SIZE);

    memset(staged_a, 0x44, sizeof(staged_a));
    memset(staged_b, 0x55, sizeof(staged_b));
    memset(staged_c, 0x66, sizeof(staged_c));

    TinyDBV2PublishEntry entries[3] = {
        {11u, page_a, staged_a},
        {12u, page_b, staged_b},
        {13u, page_c, staged_c},
    };

    if (tinydb_v2_publish_batch(entries, 3u, 2u)) {
        fprintf(stderr, "injected partial publication unexpectedly succeeded\n");
        return 1;
    }
    if (!page_equals(page_a, 0x11, 0xA1) ||
        !page_equals(page_b, 0x22, 0xB2) ||
        !page_equals(page_c, 0x33, 0xC3)) {
        fprintf(stderr, "failure did not restore every page image\n");
        return 2;
    }

    if (!tinydb_v2_publish_batch(entries, 3u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "atomic publication failed\n");
        return 3;
    }
    if (!page_equals(page_a, 0x44, 0xA1) ||
        !page_equals(page_b, 0x55, 0xB2) ||
        !page_equals(page_c, 0x66, 0xC3)) {
        fprintf(stderr, "publication or checksum isolation mismatch\n");
        return 4;
    }

    TinyDBV2PublishEntry duplicate[2] = {
        {21u, page_a, staged_a},
        {21u, page_b, staged_b},
    };
    if (tinydb_v2_publish_batch(duplicate, 2u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "duplicate page identity was accepted\n");
        return 5;
    }

    TinyDBV2PublishEntry alias[2] = {
        {31u, page_a, staged_a},
        {32u, page_a, staged_b},
    };
    if (tinydb_v2_publish_batch(alias, 2u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "aliased target pointer was accepted\n");
        return 6;
    }

    if (tinydb_v2_publish_batch(entries, 0u, TINYDB_V2_PUBLISH_NO_FAIL) ||
        tinydb_v2_publish_batch(NULL, 1u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "invalid batch shape was accepted\n");
        return 7;
    }

    unsigned char root_page[PAGE_SIZE];
    unsigned char staged_root[PAGE_SIZE];
    memset(root_page, 0x71, PAGE_USABLE_SIZE);
    memset(root_page + PAGE_USABLE_SIZE, 0xD4, PAGE_CHECKSUM_SIZE);
    memset(staged_root, 0x82, sizeof(staged_root));
    TinyDBV2PublishEntry root_zero = {0u, root_page, staged_root};
    if (!tinydb_v2_publish_batch(&root_zero,
                                 1u,
                                 TINYDB_V2_PUBLISH_NO_FAIL) ||
        !page_equals(root_page, 0x82, 0xD4)) {
        fprintf(stderr, "valid page-zero root publication was rejected\n");
        return 8;
    }

    TinyDBV2PublishEntry invalid = {
        INVALID_PAGE_NUM,
        root_page,
        staged_root,
    };
    if (tinydb_v2_publish_batch(&invalid,
                                1u,
                                TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "INVALID_PAGE_NUM publication was accepted\n");
        return 9;
    }

    int deep_result = exercise_deep_batch();
    if (deep_result != 0) return deep_result;

    printf("SLOTTED_V2_PUBLISH_BATCH_OK atomic=yes rollback=yes "
           "checksum_isolated=yes duplicate_guard=yes root0=yes "
           "deep_batch=yes beyond_legacy_limit=yes beyond_previous_limit=yes "
           "capacity_guard=yes budget_guard=yes\n");
    return 0;
}
