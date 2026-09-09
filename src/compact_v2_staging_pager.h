#ifndef COMPACT_V2_STAGING_PAGER_H
#define COMPACT_V2_STAGING_PAGER_H

#include "compact_v2_staging_hierarchy.h"
#include "pager.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Pager-backed publication seam for compact-V2 migration staging trees.
 *
 * This layer deliberately stops before CHECKPOINT/catalog/root publication.
 * Page identities are claimed only inside an active Pager transaction, so an
 * abandoned staging build can be discarded with the ordinary transaction
 * rollback path.  Materialization accepts only a fully validated private
 * hierarchy whose page-number namespace exactly matches the claimed batch.
 */

static inline bool tinydb_compact_v2_staging_pager_claim_pages(
    Pager* pager,
    uint32_t page_count,
    uint32_t* claimed_pages) {
    if (pager == NULL || claimed_pages == NULL || page_count == 0u ||
        !pager->in_transaction) {
        return false;
    }

    /* Preflight allocator metadata before consuming any identity. */
    if (pager->free_page_count > pager->num_pages) return false;
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        uint32_t page_num = pager->free_pages[i];
        if (page_num == 0u || page_num == INVALID_PAGE_NUM ||
            page_num >= pager->num_pages) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (page_num == pager->free_pages[j]) return false;
        }
    }

    uint64_t append_needed = page_count > pager->free_page_count
        ? (uint64_t)(page_count - pager->free_page_count)
        : 0u;
    if ((uint64_t)pager->num_pages + append_needed >=
        (uint64_t)INVALID_PAGE_NUM) {
        return false;
    }

    /*
     * get_unused_page_num() alone does not advance a file-tail allocation.
     * Fetch each identity immediately so pager->num_pages advances before the
     * next claim.  The pages remain clean until materialization below.
     */
    for (uint32_t i = 0u; i < page_count; i++) {
        uint32_t page_num = get_unused_page_num(pager);
        if (page_num == 0u || page_num == INVALID_PAGE_NUM) return false;
        (void)get_page(pager, page_num);
        claimed_pages[i] = page_num;
    }

    for (uint32_t i = 0u; i < page_count; i++) {
        for (uint32_t j = 0u; j < i; j++) {
            if (claimed_pages[i] == claimed_pages[j]) return false;
        }
    }
    return true;
}

static inline bool tinydb_compact_v2_staging_pager_claims_match_hierarchy(
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    const uint32_t* claimed_pages,
    uint32_t claimed_count) {
    if (hierarchy == NULL || claimed_pages == NULL ||
        !tinydb_compact_v2_staging_hierarchy_validate(hierarchy)) {
        return false;
    }

    uint64_t expected64 = (uint64_t)hierarchy->leaves->page_count +
                          (uint64_t)hierarchy->internal_count;
    if (expected64 > UINT32_MAX || claimed_count != (uint32_t)expected64) {
        return false;
    }

    for (uint32_t i = 0u; i < claimed_count; i++) {
        uint32_t page_num = claimed_pages[i];
        if (page_num == 0u || page_num == INVALID_PAGE_NUM) return false;
        for (uint32_t j = 0u; j < i; j++) {
            if (page_num == claimed_pages[j]) return false;
        }

        bool found = false;
        for (uint32_t leaf = 0u; leaf < hierarchy->leaves->page_count; leaf++) {
            if (page_num == hierarchy->leaves->page_numbers[leaf]) {
                found = true;
                break;
            }
        }
        if (!found) {
            for (uint32_t internal = 0u;
                 internal < hierarchy->internal_count;
                 internal++) {
                if (page_num == hierarchy->internal_page_numbers[internal]) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) return false;
    }
    return true;
}

/*
 * Re-read every Pager-backed staging page before exposing its root identity.
 * The private hierarchy has already passed full topology validation; exact
 * byte equality therefore proves that the transaction-scoped Pager images
 * preserve the same sibling, parent/child, separator, and compact-row bytes.
 * This remains an in-transaction verification seam and performs no durable or
 * catalog publication.
 */
static inline bool tinydb_compact_v2_staging_pager_verify_materialized_hierarchy(
    Pager* pager,
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    const uint32_t* claimed_pages,
    uint32_t claimed_count) {
    if (pager == NULL || !pager->in_transaction ||
        !tinydb_compact_v2_staging_pager_claims_match_hierarchy(
            hierarchy, claimed_pages, claimed_count)) {
        return false;
    }

    for (uint32_t i = 0u; i < claimed_count; i++) {
        uint32_t page_num = claimed_pages[i];
        if (page_num >= pager->num_pages) return false;
        for (uint32_t free_index = 0u;
             free_index < pager->free_page_count;
             free_index++) {
            if (pager->free_pages[free_index] == page_num) return false;
        }
    }

    for (uint32_t i = 0u; i < hierarchy->leaves->page_count; i++) {
        uint32_t page_num = hierarchy->leaves->page_numbers[i];
        const unsigned char* expected =
            tinydb_compact_v2_staging_page_const(hierarchy->leaves, i);
        const void* actual = get_page(pager, page_num);
        if (expected == NULL || actual == NULL ||
            memcmp(actual, expected, PAGE_SIZE) != 0) {
            return false;
        }
    }

    for (uint32_t i = 0u; i < hierarchy->internal_count; i++) {
        uint32_t page_num = hierarchy->internal_page_numbers[i];
        const unsigned char* expected =
            tinydb_compact_v2_staging_internal_page_const(hierarchy, i);
        const void* actual = get_page(pager, page_num);
        if (expected == NULL || actual == NULL ||
            memcmp(actual, expected, PAGE_SIZE) != 0) {
            return false;
        }
    }
    return true;
}

static inline bool tinydb_compact_v2_staging_pager_materialize_hierarchy(
    Pager* pager,
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    const uint32_t* claimed_pages,
    uint32_t claimed_count,
    uint32_t* staged_root_page_num) {
    if (staged_root_page_num != NULL) *staged_root_page_num = 0u;
    if (pager == NULL || !pager->in_transaction ||
        !tinydb_compact_v2_staging_pager_claims_match_hierarchy(
            hierarchy, claimed_pages, claimed_count)) {
        return false;
    }

    /* Every claimed identity must still be allocated and absent from free list. */
    for (uint32_t i = 0u; i < claimed_count; i++) {
        uint32_t page_num = claimed_pages[i];
        if (page_num >= pager->num_pages) return false;
        for (uint32_t free_index = 0u;
             free_index < pager->free_page_count;
             free_index++) {
            if (pager->free_pages[free_index] == page_num) return false;
        }
    }

    /* All fallible hierarchy/identity validation is complete before writes. */
    for (uint32_t i = 0u; i < hierarchy->leaves->page_count; i++) {
        uint32_t page_num = hierarchy->leaves->page_numbers[i];
        const unsigned char* source =
            tinydb_compact_v2_staging_page_const(hierarchy->leaves, i);
        void* destination = get_page(pager, page_num);
        if (source == NULL || destination == NULL) return false;
        memcpy(destination, source, PAGE_SIZE);
        mark_page_dirty(pager, page_num);
    }

    for (uint32_t i = 0u; i < hierarchy->internal_count; i++) {
        uint32_t page_num = hierarchy->internal_page_numbers[i];
        const unsigned char* source =
            tinydb_compact_v2_staging_internal_page_const(hierarchy, i);
        void* destination = get_page(pager, page_num);
        if (source == NULL || destination == NULL) return false;
        memcpy(destination, source, PAGE_SIZE);
        mark_page_dirty(pager, page_num);
    }

    /* Never expose a root identity until Pager readback matches the private tree. */
    if (!tinydb_compact_v2_staging_pager_verify_materialized_hierarchy(
            pager, hierarchy, claimed_pages, claimed_count)) {
        return false;
    }

    if (staged_root_page_num != NULL) {
        *staged_root_page_num = hierarchy->root_page_num;
    }
    return true;
}

#endif
