#include "slotted_v2_publish_batch.h"

#include <stdio.h>
#include <string.h>

#if TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES != 8192u
#error "budget override probe must be compiled with an 8192-byte rollback budget"
#endif

static int page_equals(const unsigned char page[PAGE_SIZE],
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

int main(void) {
    unsigned char pages[3][PAGE_SIZE];
    unsigned char staged[3][PAGE_SIZE];
    TinyDBV2PublishEntry entries[3];

    for (uint32_t i = 0u; i < 3u; i++) {
        unsigned char before_value = (unsigned char)(0x21u + i);
        unsigned char staged_value = (unsigned char)(0x61u + i);
        unsigned char checksum_value = (unsigned char)(0xA1u + i);
        memset(pages[i], before_value, PAGE_USABLE_SIZE);
        memset(pages[i] + PAGE_USABLE_SIZE,
               checksum_value,
               PAGE_CHECKSUM_SIZE);
        memset(staged[i], staged_value, PAGE_SIZE);
        entries[i].page_num = 41u + i;
        entries[i].target = pages[i];
        entries[i].staged = staged[i];
    }

    size_t two_page_bytes = 0u;
    if (!tinydb_v2_publish_batch_snapshot_size(2u, &two_page_bytes) ||
        two_page_bytes != (size_t)2u * PAGE_USABLE_SIZE) {
        fprintf(stderr, "two-page publication should fit the override budget\n");
        return 1;
    }
    if (tinydb_v2_publish_batch_snapshot_size(3u, NULL)) {
        fprintf(stderr, "three-page publication exceeded the override budget\n");
        return 2;
    }

    if (tinydb_v2_publish_batch(entries,
                                3u,
                                TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "over-budget publication unexpectedly succeeded\n");
        return 3;
    }
    for (uint32_t i = 0u; i < 3u; i++) {
        if (!page_equals(pages[i],
                         (unsigned char)(0x21u + i),
                         (unsigned char)(0xA1u + i))) {
            fprintf(stderr,
                    "over-budget preflight mutated page %u before rejection\n",
                    i);
            return 4;
        }
    }

    if (!tinydb_v2_publish_batch(entries,
                                 2u,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "within-budget publication failed\n");
        return 5;
    }
    for (uint32_t i = 0u; i < 2u; i++) {
        if (!page_equals(pages[i],
                         (unsigned char)(0x61u + i),
                         (unsigned char)(0xA1u + i))) {
            fprintf(stderr, "within-budget publication mismatch on page %u\n", i);
            return 6;
        }
    }
    if (!page_equals(pages[2], 0x23u, 0xA3u)) {
        fprintf(stderr, "unpublished page changed\n");
        return 7;
    }

    printf("SLOTTED_V2_PUBLISH_BUDGET_OVERRIDE_OK two_pages=yes "
           "over_budget_rejected=yes no_mutation=yes checksum_isolated=yes\n");
    return 0;
}
