#ifndef COMPACT_V2_MIGRATION_PAGER_RECLAIM_H
#define COMPACT_V2_MIGRATION_PAGER_RECLAIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pager.h"

/*
 * Pager-backed reclaim primitive for durable compact-V2 migration claims.
 *
 * Recovery can be replayed after a crash between allocator durability and
 * manifest removal.  Reclaim therefore has to be idempotent: an already-free
 * claimed page is a successful no-op, while duplicate identities in the input
 * manifest are rejected before any allocator mutation.
 *
 * This seam intentionally does not COMMIT/CHECKPOINT.  The reopen recovery
 * executor owns that durability boundary after a complete reclaim succeeds.
 */

static inline bool tinydb_compact_v2_migration_pager_page_is_free_locked(
    const Pager* pager,
    uint32_t page_num) {
    if (pager == NULL) return false;
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static inline bool tinydb_compact_v2_migration_pager_claims_are_valid_locked(
    const Pager* pager,
    const uint32_t* claimed_pages,
    uint32_t claimed_page_count) {
    if (pager == NULL || !pager->in_transaction ||
        claimed_pages == NULL || claimed_page_count == 0u ||
        pager->free_page_count > pager->num_pages) {
        return false;
    }

    for (uint32_t i = 0u; i < claimed_page_count; i++) {
        uint32_t page_num = claimed_pages[i];
        if (page_num == 0u || page_num == INVALID_PAGE_NUM ||
            page_num >= pager->num_pages) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (claimed_pages[j] == page_num) return false;
        }
    }

    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        uint32_t page_num = pager->free_pages[i];
        if (page_num == 0u || page_num == INVALID_PAGE_NUM ||
            page_num >= pager->num_pages) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (pager->free_pages[j] == page_num) return false;
        }
    }

    return true;
}

static inline bool tinydb_compact_v2_migration_pager_claims_are_reclaimed(
    Pager* pager,
    const uint32_t* claimed_pages,
    uint32_t claimed_page_count) {
    if (pager == NULL || claimed_pages == NULL || claimed_page_count == 0u) {
        return false;
    }

    bool reclaimed = true;
    db_rwlock_rdlock(&pager->pager_lock);
    if (pager->free_page_count > pager->num_pages) {
        reclaimed = false;
    } else {
        for (uint32_t i = 0u; i < claimed_page_count && reclaimed; i++) {
            if (!tinydb_compact_v2_migration_pager_page_is_free_locked(
                    pager, claimed_pages[i])) {
                reclaimed = false;
            }
        }
    }
    db_rwlock_rdunlock(&pager->pager_lock);
    return reclaimed;
}

static inline bool tinydb_compact_v2_migration_pager_reclaim_claims(
    Pager* pager,
    const uint32_t* claimed_pages,
    uint32_t claimed_page_count) {
    if (pager == NULL || claimed_pages == NULL || claimed_page_count == 0u) {
        return false;
    }

    /* Preflight the complete manifest-owned set before the first free. */
    db_rwlock_wrlock(&pager->pager_lock);
    bool valid = tinydb_compact_v2_migration_pager_claims_are_valid_locked(
        pager, claimed_pages, claimed_page_count);
    if (valid && pager_pin_transition_busy_locked(pager)) valid = false;
    if (valid) {
        for (uint32_t i = 0u; i < claimed_page_count; i++) {
            uint32_t page_num = claimed_pages[i];
            if (!tinydb_compact_v2_migration_pager_page_is_free_locked(
                    pager, page_num) &&
                pager_frame_for_page_is_pinned_locked(pager, page_num)) {
                valid = false;
                break;
            }
        }
    }
    db_rwlock_wrunlock(&pager->pager_lock);
    if (!valid) return false;

    /*
     * pager_free_page is guarded by the Pager pin barrier.  A concurrent pin
     * admission that races after preflight may therefore make an individual
     * free a no-op; post-validation below converts that race into false rather
     * than reporting a partial reclaim as complete.  A retry in the same
     * transaction is safe because already-free claims are skipped.
     */
    for (uint32_t i = 0u; i < claimed_page_count; i++) {
        uint32_t page_num = claimed_pages[i];
        bool already_free = false;
        db_rwlock_rdlock(&pager->pager_lock);
        already_free = tinydb_compact_v2_migration_pager_page_is_free_locked(
            pager, page_num);
        db_rwlock_rdunlock(&pager->pager_lock);
        if (!already_free) pager_free_page(pager, page_num);
    }

    return tinydb_compact_v2_migration_pager_claims_are_reclaimed(
        pager, claimed_pages, claimed_page_count);
}

#endif
