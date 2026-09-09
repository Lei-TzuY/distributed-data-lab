#ifndef TINYDB_COMPACT_V2_MIGRATION_LIVE_PAGE_GUARD_H
#define TINYDB_COMPACT_V2_MIGRATION_LIVE_PAGE_GUARD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compact_v2_migration_pager_reclaim.h"
#include "fixed_v1_tree_reclaim.h"
#include "pager_try_pin.h"
#include "table.h"

/*
 * Build a fail-closed reachability map for every page reachable through the
 * authoritative catalog roots. Migration recovery must protect complete live
 * trees, not merely their roots: a corrupt sidecar that reclaims a live leaf or
 * internal child is just as destructive as reclaiming the root itself.
 *
 * The walker intentionally follows only parent/child topology. Leaf sibling
 * links are not ownership edges. It validates root/non-root identity, parent
 * pointers, node type, internal fanout, free-page exclusion, page bounds, and
 * duplicate/shared child references while using the non-fatal existing-page
 * pin seam.
 */
static inline bool tinydb_compact_v2_migration_live_page_is_free(
    Pager* pager,
    uint32_t page_num) {
    bool is_free;
    if (pager == NULL) return true;
    db_rwlock_rdlock(&pager->pager_lock);
    is_free = page_num >= pager->num_pages ||
        tinydb_compact_v2_migration_pager_page_is_free_locked(pager, page_num);
    db_rwlock_rdunlock(&pager->pager_lock);
    return is_free;
}

static inline bool tinydb_compact_v2_migration_collect_tree_pages(
    Pager* pager,
    uint32_t root_page_num,
    uint8_t* owned,
    uint32_t owned_capacity) {
    uint32_t* queue = NULL;
    uint32_t* expected_parent = NULL;
    uint32_t queue_head = 0u;
    uint32_t queue_count = 0u;
    bool ok = false;

    if (pager == NULL || owned == NULL || owned_capacity == 0u ||
        root_page_num == INVALID_PAGE_NUM || root_page_num >= owned_capacity ||
        owned_capacity != pager->num_pages || owned[root_page_num] != 0u ||
        tinydb_compact_v2_migration_live_page_is_free(pager, root_page_num)) {
        return false;
    }

    queue = (uint32_t*)calloc(owned_capacity, sizeof(uint32_t));
    expected_parent = (uint32_t*)calloc(owned_capacity, sizeof(uint32_t));
    if (queue == NULL || expected_parent == NULL) goto cleanup;

    queue[queue_count++] = root_page_num;
    owned[root_page_num] = 1u;
    expected_parent[root_page_num] = 0u;

    while (queue_head < queue_count) {
        const uint32_t page_num = queue[queue_head++];
        PagerPageHandle handle;
        PagerTryPinStatus status = pager_try_pin_existing_page_handle(
            pager, page_num, &handle);
        if (status != PAGER_TRY_PIN_OK ||
            !pager_page_handle_acquire_read(&handle)) {
            if (status == PAGER_TRY_PIN_OK) {
                (void)pager_release_page_handle(&handle);
            }
            goto cleanup;
        }

        void* node = handle.data;
        const bool is_root = page_num == root_page_num;
        if ((is_root && !is_node_root(node)) ||
            (!is_root && (is_node_root(node) ||
                          *node_parent(node) != expected_parent[page_num]))) {
            (void)pager_page_handle_release_read(&handle);
            (void)pager_release_page_handle(&handle);
            goto cleanup;
        }

        const NodeType type = get_node_type(node);
        if (type == NODE_INTERNAL) {
            const uint32_t key_count = *internal_node_num_keys(node);
            const uint32_t child_count = key_count + 1u;
            if (key_count == 0u || key_count > INTERNAL_NODE_MAX_KEYS ||
                child_count > owned_capacity ||
                queue_count > owned_capacity - child_count) {
                (void)pager_page_handle_release_read(&handle);
                (void)pager_release_page_handle(&handle);
                goto cleanup;
            }
            for (uint32_t i = 0u; i < child_count; i++) {
                const uint32_t child = *internal_node_child(node, i);
                if (child == INVALID_PAGE_NUM || child >= owned_capacity ||
                    owned[child] != 0u ||
                    tinydb_compact_v2_migration_live_page_is_free(pager, child)) {
                    (void)pager_page_handle_release_read(&handle);
                    (void)pager_release_page_handle(&handle);
                    goto cleanup;
                }
                owned[child] = 1u;
                expected_parent[child] = page_num;
                queue[queue_count++] = child;
            }
        } else if (type != NODE_LEAF) {
            (void)pager_page_handle_release_read(&handle);
            (void)pager_release_page_handle(&handle);
            goto cleanup;
        }

        if (!pager_page_handle_release_read(&handle) ||
            !pager_release_page_handle(&handle)) {
            goto cleanup;
        }
    }

    ok = true;

cleanup:
    free(queue);
    free(expected_parent);
    return ok;
}

static inline bool tinydb_compact_v2_migration_build_catalog_live_page_map(
    const Catalog* catalog,
    Pager* pager,
    uint8_t* owned,
    uint32_t owned_capacity) {
    if (catalog == NULL || pager == NULL || owned == NULL ||
        owned_capacity != pager->num_pages || catalog->num_tables > MAX_TABLES) {
        return false;
    }
    memset(owned, 0, owned_capacity);
    for (uint32_t i = 0u; i < catalog->num_tables; i++) {
        if (!tinydb_compact_v2_migration_collect_tree_pages(
                pager, catalog->schemas[i].root_page_num, owned, owned_capacity)) {
            memset(owned, 0, owned_capacity);
            return false;
        }
    }
    return true;
}

/*
 * Rebuild the complete authoritative live-page map after a successful reopen
 * recovery before publishing the database handle. This closes the recovery
 * loop: preflight proves the intended reclaim is disjoint from live trees,
 * while this postcondition proves the committed reclaim/checkpoint did not
 * leave any catalog root, internal child, or leaf marked free/corrupt/shared.
 */
static inline bool tinydb_compact_v2_migration_validate_catalog_live_pages(
    const Catalog* catalog,
    Pager* pager) {
    uint8_t* owned = NULL;
    uint32_t page_count;
    bool ok;

    if (catalog == NULL || pager == NULL) return false;

    db_rwlock_rdlock(&pager->pager_lock);
    page_count = pager->num_pages;
    db_rwlock_rdunlock(&pager->pager_lock);
    if (page_count == 0u) return false;

    owned = (uint8_t*)calloc(page_count, sizeof(uint8_t));
    if (owned == NULL) return false;
    ok = tinydb_compact_v2_migration_build_catalog_live_page_map(
        catalog, pager, owned, page_count);
    free(owned);
    return ok;
}

static inline bool tinydb_compact_v2_migration_detached_tree_disjoint_from_live(
    Pager* pager,
    uint32_t detached_root,
    const uint8_t* live,
    uint32_t live_capacity) {
    uint8_t* detached = NULL;
    bool ok = false;

    if (pager == NULL || live == NULL || live_capacity != pager->num_pages ||
        detached_root == INVALID_PAGE_NUM || detached_root >= live_capacity ||
        live[detached_root] != 0u) {
        return false;
    }

    /* A checkpointed previous reclaim is an idempotent success on retry. */
    if ((detached_root != 0u &&
         tinydb_compact_v2_migration_live_page_is_free(pager, detached_root)) ||
        (detached_root == 0u && tinydb_fixed_v1_page_zero_is_retired(pager))) {
        return true;
    }

    detached = (uint8_t*)calloc(live_capacity, sizeof(uint8_t));
    if (detached == NULL) return false;
    if (!tinydb_compact_v2_migration_collect_tree_pages(
            pager, detached_root, detached, live_capacity)) {
        goto cleanup;
    }
    for (uint32_t page_num = 0u; page_num < live_capacity; page_num++) {
        if (detached[page_num] != 0u && live[page_num] != 0u) goto cleanup;
    }
    ok = true;

cleanup:
    free(detached);
    return ok;
}

#endif /* TINYDB_COMPACT_V2_MIGRATION_LIVE_PAGE_GUARD_H */
