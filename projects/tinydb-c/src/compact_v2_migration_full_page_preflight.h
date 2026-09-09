#ifndef TINYDB_COMPACT_V2_MIGRATION_FULL_PAGE_PREFLIGHT_H
#define TINYDB_COMPACT_V2_MIGRATION_FULL_PAGE_PREFLIGHT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compact_v2_migration_live_page_guard.h"
#include "compact_v2_migration_open_adapter.h"

/*
 * Strong production preflight for interrupted compact-V2 migrations.
 *
 * Root-only checks are insufficient in a multi-table file: staging claims or
 * a detached old tree can overlap another table's live internal/leaf pages
 * without naming that table's root. Build a complete authoritative catalog
 * reachability map first, then prove that every page recovery may reclaim is
 * disjoint from every live tree.
 */
static inline bool tinydb_compact_v2_migration_claims_are_disjoint_from_live(
    const TinyDBCompactV2MigrationManifest* manifest,
    const uint8_t* live,
    uint32_t live_capacity) {
    if (manifest == NULL || live == NULL || live_capacity == 0u ||
        !tinydb_compact_v2_migration_manifest_is_valid(manifest)) {
        return false;
    }
    for (uint32_t i = 0u; i < manifest->claimed_page_count; i++) {
        const uint32_t page_num = manifest->claimed_pages[i];
        if (page_num >= live_capacity || live[page_num] != 0u) return false;
    }
    return true;
}

static inline bool tinydb_compact_v2_migration_open_adapter_manifest_pages_are_safe(
    void* opaque,
    const TinyDBCompactV2MigrationManifest* manifest) {
    TinyDBCompactV2MigrationOpenAdapterContext* context =
        (TinyDBCompactV2MigrationOpenAdapterContext*)opaque;
    uint32_t authoritative_root = 0u;
    uint64_t authoritative_generation = UINT64_C(0);
    TinyDBCompactV2MigrationStrictRecoveryAction action;
    Pager* pager;
    const Catalog* catalog;
    uint8_t* live = NULL;
    uint32_t num_pages;
    bool safe = false;

    if (context == NULL || manifest == NULL ||
        !tinydb_compact_v2_migration_catalog_state_is_valid(&context->catalog_state) ||
        !tinydb_compact_v2_migration_manifest_is_valid(manifest)) {
        return false;
    }
    if (!tinydb_compact_v2_migration_catalog_state_read(
            &context->catalog_state,
            manifest->table_id,
            &authoritative_root,
            &authoritative_generation)) {
        return false;
    }

    action = tinydb_compact_v2_migration_manifest_classify_recovery_strict(
        manifest, authoritative_root, authoritative_generation);
    if (action != TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING &&
        action != TINYDB_COMPACT_V2_MIGRATION_STRICT_KEEP_NEW_RECLAIM_OLD) {
        return false;
    }

    pager = (Pager*)context->catalog_state.pager;
    catalog = context->catalog_state.catalog;
    if (pager == NULL || catalog == NULL) return false;
    num_pages = pager->num_pages;
    if (num_pages == 0u) return false;

    live = (uint8_t*)calloc(num_pages, sizeof(uint8_t));
    if (live == NULL) return false;
    if (!tinydb_compact_v2_migration_build_catalog_live_page_map(
            catalog, pager, live, num_pages)) {
        goto cleanup;
    }

    if (action == TINYDB_COMPACT_V2_MIGRATION_STRICT_RECLAIM_STAGING) {
        safe = tinydb_compact_v2_migration_claims_are_disjoint_from_live(
            manifest, live, num_pages);
        goto cleanup;
    }

    safe = tinydb_compact_v2_migration_detached_tree_disjoint_from_live(
        pager, manifest->old_root_page_num, live, num_pages);

cleanup:
    free(live);
    return safe;
}

#endif /* TINYDB_COMPACT_V2_MIGRATION_FULL_PAGE_PREFLIGHT_H */
