#ifndef SCHEMA_REPACK_STAGING_PAGER_H
#define SCHEMA_REPACK_STAGING_PAGER_H

#include "compact_v2_staging_pager.h"
#include "schema_repack_staging_tree.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Transaction-scoped Pager materialization for schema-repack staging trees.
 *
 * Schema repack rows are initially built with caller-owned private page
 * identities so source scanning never mutates Pager state.  Once the complete
 * row set is known, this seam claims the exact number of physical Pager pages,
 * rewrites the private leaf sibling namespace to those claimed identities,
 * builds the internal hierarchy directly with the remaining claimed page
 * numbers, and copies the validated tree into Pager dirty pages.
 *
 * This intentionally stops before pager_commit(), pager_checkpoint(), migration
 * manifest publication, or catalog/root/schema-generation changes.  Any
 * failure after page claiming requires the caller to roll back the dedicated
 * Pager transaction; result.ready stays false and no staged root is exposed.
 *
 * The first production target is populated/multi-leaf schema widening.  A
 * single-leaf repack remains on the pure-memory path until the Pager staging
 * layer grows a hierarchy-independent single-root materializer.
 */
typedef struct {
    uint32_t root_page_num;
    uint32_t leaf_page_count;
    uint32_t internal_page_count;
    uint32_t claimed_page_count;
    uint64_t row_count;
    bool ready;
} TinyDBSchemaRepackPagerStageResult;

static inline void tinydb_schema_repack_staging_pager_set_message(
    char* message,
    size_t message_size,
    const char* detail) {
    if (message == NULL || message_size == 0u) return;
    if (detail == NULL) detail = "schema repack Pager staging failed";
    (void)snprintf(message, message_size, "%s", detail);
}

/*
 * Rebind an already validated private leaf chain to a new physical namespace.
 * All input validation is completed before any leaf image or chain metadata is
 * changed.  Only sibling page numbers live in compact-V2 leaf-local topology;
 * parent pointers are intentionally left for the subsequent hierarchy build.
 */
static inline bool tinydb_schema_repack_staging_rebind_leaf_pages(
    TinyDBCompactV2StagingLeafChain* chain,
    const uint32_t* new_page_numbers,
    uint32_t new_page_count) {
    if (chain == NULL || new_page_numbers == NULL ||
        new_page_count == 0u || new_page_count != chain->page_count ||
        !tinydb_compact_v2_staging_leaf_chain_validate(chain)) {
        return false;
    }

    for (uint32_t i = 0u; i < new_page_count; i++) {
        uint32_t page_num = new_page_numbers[i];
        if (page_num == 0u || page_num == INVALID_PAGE_NUM) return false;
        for (uint32_t j = 0u; j < i; j++) {
            if (page_num == new_page_numbers[j]) return false;
        }
        const unsigned char* page = tinydb_compact_v2_staging_page_const(chain, i);
        if (page == NULL || !tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE)) {
            return false;
        }
    }

    for (uint32_t i = 0u; i < new_page_count; i++) {
        unsigned char* page = tinydb_compact_v2_staging_page(chain, i);
        uint32_t previous = i == 0u ? 0u : new_page_numbers[i - 1u];
        uint32_t next = i + 1u == new_page_count ? 0u : new_page_numbers[i + 1u];
        tinydb_compact_v2_staging_write_u32_le(
            page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, previous);
        tinydb_compact_v2_staging_write_u32_le(
            page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, next);
    }
    chain->page_numbers = new_page_numbers;
    return tinydb_compact_v2_staging_leaf_chain_validate(chain);
}

static inline bool tinydb_schema_repack_staging_materialize_pager(
    Pager* pager,
    TinyDBSchemaRepackStaging* staging,
    uint64_t expected_rows,
    TinyDBCompactV2StagingHierarchy* hierarchy,
    unsigned char* internal_images,
    uint32_t internal_capacity,
    uint32_t* claimed_pages,
    uint32_t claimed_capacity,
    TinyDBSchemaRepackPagerStageResult* result,
    char* message,
    size_t message_size) {
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (hierarchy != NULL) memset(hierarchy, 0, sizeof(*hierarchy));
    if (message != NULL && message_size > 0u) message[0] = '\0';

    if (pager == NULL || !pager->in_transaction || staging == NULL ||
        staging->chain == NULL || hierarchy == NULL || internal_images == NULL ||
        claimed_pages == NULL || result == NULL) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size, "schema repack Pager staging arguments are invalid");
        return false;
    }

    char finish_message[192];
    if (!tinydb_schema_repack_staging_finish(
            staging, expected_rows, finish_message, sizeof(finish_message))) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size, finish_message);
        return false;
    }

    TinyDBCompactV2StagingLeafChain* leaves = staging->chain;
    if (leaves->page_count < 2u) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size,
            "schema repack Pager staging currently requires a multi-leaf tree");
        return false;
    }

    uint32_t required_internal = 0u;
    uint32_t required_levels = 0u;
    if (!tinydb_compact_v2_staging_required_internal_pages(
            leaves->page_count, &required_internal, &required_levels) ||
        required_internal == 0u || required_internal > internal_capacity) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size, "schema repack hierarchy capacity is insufficient");
        return false;
    }
    (void)required_levels;

    uint64_t total64 = (uint64_t)leaves->page_count + (uint64_t)required_internal;
    if (total64 > UINT32_MAX || total64 > claimed_capacity) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size, "schema repack claimed-page capacity is insufficient");
        return false;
    }
    uint32_t total_pages = (uint32_t)total64;

    /* From this point onward the dedicated transaction must be rolled back on
     * failure, because allocator identities may already have been consumed. */
    if (!tinydb_compact_v2_staging_pager_claim_pages(
            pager, total_pages, claimed_pages)) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size, "Pager could not claim the schema repack page set");
        return false;
    }

    if (!tinydb_schema_repack_staging_rebind_leaf_pages(
            leaves, claimed_pages, leaves->page_count)) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size, "schema repack leaves could not bind Pager page identities");
        return false;
    }

    TinyDBSchemaRepackStagingTreeResult tree_result;
    if (!tinydb_schema_repack_staging_build_tree(
            staging,
            expected_rows,
            hierarchy,
            internal_images,
            claimed_pages + leaves->page_count,
            required_internal,
            &tree_result,
            finish_message,
            sizeof(finish_message))) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size, finish_message);
        return false;
    }

    if (!tree_result.ready || tree_result.internal_page_count != required_internal ||
        tree_result.leaf_page_count != leaves->page_count ||
        tree_result.row_count != expected_rows ||
        !tinydb_compact_v2_staging_pager_claims_match_hierarchy(
            hierarchy, claimed_pages, total_pages)) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size, "schema repack Pager namespace failed hierarchy validation");
        return false;
    }

    uint32_t staged_root = 0u;
    if (!tinydb_compact_v2_staging_pager_materialize_hierarchy(
            pager, hierarchy, claimed_pages, total_pages, &staged_root) ||
        staged_root == 0u || staged_root != tree_result.root_page_num) {
        tinydb_schema_repack_staging_pager_set_message(
            message, message_size, "schema repack hierarchy could not materialize in Pager");
        return false;
    }

    result->root_page_num = staged_root;
    result->leaf_page_count = tree_result.leaf_page_count;
    result->internal_page_count = tree_result.internal_page_count;
    result->claimed_page_count = total_pages;
    result->row_count = tree_result.row_count;
    result->ready = true;
    return true;
}

#endif /* SCHEMA_REPACK_STAGING_PAGER_H */
