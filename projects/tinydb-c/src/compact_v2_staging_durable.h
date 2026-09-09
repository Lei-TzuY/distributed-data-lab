#ifndef COMPACT_V2_STAGING_DURABLE_H
#define COMPACT_V2_STAGING_DURABLE_H

#include "compact_v2_staging_pager.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Durable-but-unpublished boundary for compact-V2 migration staging trees.
 *
 * The caller must dedicate the active Pager transaction to the staging tree.
 * Before committing, this seam proves that the Pager dirty set is exactly the
 * claimed hierarchy page set: every claimed staging page is dirty and no
 * unrelated page is dirty.  That isolation rule is important because
 * pager_commit() commits the entire transaction, not a selected page subset.
 *
 * On success the staging page images and allocator state have passed the WAL
 * commit and checkpoint durability boundary, while catalog/schema/root
 * publication remains deliberately outside this layer.  A later recovery
 * protocol can therefore treat these pages as durable orphan staging state
 * until an explicit catalog publication makes the new root authoritative.
 */

static inline bool tinydb_compact_v2_staging_page_is_claimed(
    uint32_t page_num,
    const uint32_t* claimed_pages,
    uint32_t claimed_count) {
    if (claimed_pages == NULL) return false;
    for (uint32_t i = 0u; i < claimed_count; i++) {
        if (claimed_pages[i] == page_num) return true;
    }
    return false;
}

static inline bool tinydb_compact_v2_staging_pager_dirty_set_is_exact(
    const Pager* pager,
    const uint32_t* claimed_pages,
    uint32_t claimed_count) {
    if (pager == NULL || claimed_pages == NULL || claimed_count == 0u ||
        !pager->in_transaction) {
        return false;
    }

    for (uint32_t i = 0u; i < claimed_count; i++) {
        uint32_t page_num = claimed_pages[i];
        if (page_num == 0u || page_num == INVALID_PAGE_NUM ||
            page_num >= pager->num_pages || page_num >= pager->page_capacity ||
            !pager->is_dirty[page_num]) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (page_num == claimed_pages[j]) return false;
        }
    }

    for (uint32_t page_num = 0u; page_num < pager->num_pages; page_num++) {
        if (!pager->is_dirty[page_num]) continue;
        if (!tinydb_compact_v2_staging_page_is_claimed(
                page_num, claimed_pages, claimed_count)) {
            return false;
        }
    }
    return true;
}

static inline bool tinydb_compact_v2_staging_pager_make_durable_unpublished(
    Pager* pager,
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    const uint32_t* claimed_pages,
    uint32_t claimed_count,
    uint32_t* durable_staged_root_page_num) {
    if (durable_staged_root_page_num != NULL) {
        *durable_staged_root_page_num = 0u;
    }
    if (pager == NULL || hierarchy == NULL || claimed_pages == NULL ||
        !pager->in_transaction ||
        !tinydb_compact_v2_staging_pager_claims_match_hierarchy(
            hierarchy, claimed_pages, claimed_count) ||
        !tinydb_compact_v2_staging_pager_verify_materialized_hierarchy(
            pager, hierarchy, claimed_pages, claimed_count) ||
        !tinydb_compact_v2_staging_pager_dirty_set_is_exact(
            pager, claimed_pages, claimed_count)) {
        return false;
    }

    /*
     * pager_commit() serializes all dirty page images plus allocator state to
     * WAL and ends the transaction.  The exact-dirty-set gate above guarantees
     * that this cannot accidentally commit unrelated page mutations.
     */
    pager_commit(pager);
    if (pager->in_transaction) return false;

    /*
     * Checkpoint transfers the committed staging images to the database file,
     * syncs them, publishes free-space state, and removes the replayed WAL.
     * Catalog/root publication is intentionally not performed here.
     */
    pager_checkpoint(pager);

    for (uint32_t i = 0u; i < claimed_count; i++) {
        uint32_t page_num = claimed_pages[i];
        if (page_num >= pager->num_pages || page_num >= pager->page_capacity ||
            pager->is_dirty[page_num]) {
            return false;
        }
    }

    /*
     * Commit writes page checksums into the final four bytes.  Compare the
     * semantic page image excluding that checksum trailer after checkpoint.
     */
    for (uint32_t i = 0u; i < hierarchy->leaves->page_count; i++) {
        uint32_t page_num = hierarchy->leaves->page_numbers[i];
        const unsigned char* expected =
            tinydb_compact_v2_staging_page_const(hierarchy->leaves, i);
        const void* actual = get_page(pager, page_num);
        if (expected == NULL || actual == NULL ||
            memcmp(actual, expected, PAGE_USABLE_SIZE) != 0) {
            return false;
        }
    }
    for (uint32_t i = 0u; i < hierarchy->internal_count; i++) {
        uint32_t page_num = hierarchy->internal_page_numbers[i];
        const unsigned char* expected =
            tinydb_compact_v2_staging_internal_page_const(hierarchy, i);
        const void* actual = get_page(pager, page_num);
        if (expected == NULL || actual == NULL ||
            memcmp(actual, expected, PAGE_USABLE_SIZE) != 0) {
            return false;
        }
    }

    if (durable_staged_root_page_num != NULL) {
        *durable_staged_root_page_num = hierarchy->root_page_num;
    }
    return true;
}

#endif
