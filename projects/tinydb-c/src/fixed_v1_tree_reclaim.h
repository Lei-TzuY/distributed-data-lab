#ifndef FIXED_V1_TREE_RECLAIM_H
#define FIXED_V1_TREE_RECLAIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compact_v2_migration_pager_reclaim.h"
#include "table.h"

/*
 * Recovery-only ownership walk for an old fixed-cell V1 B+ tree.
 *
 * The walk is deliberately read-only until the complete tree has passed
 * topology, parent/child, separator, leaf-order and sibling validation.  Only
 * then is the collected ownership set handed to the transactional Pager
 * reclaim primitive.  This prevents a corrupt/cyclic old tree from being
 * partially freed before recovery discovers the corruption.
 *
 * Page zero is permanently reserved by the Pager free-list format.  Historical
 * page-zero roots therefore use a retirement protocol: validate the complete
 * old tree first, reclaim every nonzero descendant transactionally, then turn
 * page zero into an empty rooted leaf tombstone.  The tombstone makes retries
 * idempotent without ever inserting page zero into the durable free list.
 */

typedef struct TinyDBFixedV1TreeOwnership {
    uint32_t* pages;
    uint32_t page_count;
} TinyDBFixedV1TreeOwnership;

static inline void tinydb_fixed_v1_tree_ownership_destroy(
    TinyDBFixedV1TreeOwnership* ownership) {
    if (ownership == NULL) return;
    free(ownership->pages);
    ownership->pages = NULL;
    ownership->page_count = 0u;
}

static inline uint32_t tinydb_fixed_v1_read_u32(const void* page,
                                                 uint32_t offset) {
    uint32_t value = 0u;
    memcpy(&value, (const uint8_t*)page + offset, sizeof(value));
    return value;
}

static inline void tinydb_fixed_v1_write_u32(void* page,
                                              uint32_t offset,
                                              uint32_t value) {
    memcpy((uint8_t*)page + offset, &value, sizeof(value));
}

static inline bool tinydb_fixed_v1_tree_page_is_free(Pager* pager,
                                                      uint32_t page_num) {
    bool is_free = false;
    db_rwlock_rdlock(&pager->pager_lock);
    is_free = tinydb_compact_v2_migration_pager_page_is_free_locked(
        pager, page_num);
    db_rwlock_rdunlock(&pager->pager_lock);
    return is_free;
}

static inline bool tinydb_fixed_v1_tree_pin_read(Pager* pager,
                                                  uint32_t page_num,
                                                  PagerPageHandle* handle) {
    if (!pager_pin_page_handle(pager, page_num, handle)) return false;
    if (!pager_page_handle_acquire_read(handle)) {
        (void)pager_release_page_handle(handle);
        return false;
    }
    return true;
}

static inline bool tinydb_fixed_v1_tree_unpin_read(PagerPageHandle* handle) {
    bool unlocked = pager_page_handle_release_read(handle);
    bool released = pager_release_page_handle(handle);
    return unlocked && released;
}

static inline bool tinydb_fixed_v1_tree_collect_ownership_impl(
    Pager* pager,
    uint32_t root_page_num,
    bool allow_page_zero_root,
    TinyDBFixedV1TreeOwnership* ownership_out) {
    uint32_t num_pages;
    uint32_t* queue = NULL;
    uint32_t* expected_parent = NULL;
    uint8_t* seen = NULL;
    uint8_t* node_type = NULL;
    uint8_t* resolved = NULL;
    uint32_t* subtree_min = NULL;
    uint32_t* subtree_max = NULL;
    uint32_t* leaf_prev = NULL;
    uint32_t* leaf_next = NULL;
    uint32_t queue_head = 0u;
    uint32_t queue_count = 0u;
    uint32_t resolved_count = 0u;
    uint32_t leaf_count = 0u;
    bool valid = false;

    if (ownership_out != NULL) {
        ownership_out->pages = NULL;
        ownership_out->page_count = 0u;
    }
    if (pager == NULL || ownership_out == NULL || !pager->in_transaction ||
        root_page_num == INVALID_PAGE_NUM ||
        (root_page_num == 0u && !allow_page_zero_root)) {
        return false;
    }

    db_rwlock_rdlock(&pager->pager_lock);
    num_pages = pager->num_pages;
    bool metadata_valid = pager->free_page_count <= pager->num_pages &&
                          root_page_num < pager->num_pages &&
                          !tinydb_compact_v2_migration_pager_page_is_free_locked(
                              pager, root_page_num);
    db_rwlock_rdunlock(&pager->pager_lock);
    if (!metadata_valid || num_pages == 0u) return false;

    queue = (uint32_t*)calloc(num_pages, sizeof(uint32_t));
    expected_parent = (uint32_t*)calloc(num_pages, sizeof(uint32_t));
    seen = (uint8_t*)calloc(num_pages, sizeof(uint8_t));
    node_type = (uint8_t*)calloc(num_pages, sizeof(uint8_t));
    resolved = (uint8_t*)calloc(num_pages, sizeof(uint8_t));
    subtree_min = (uint32_t*)calloc(num_pages, sizeof(uint32_t));
    subtree_max = (uint32_t*)calloc(num_pages, sizeof(uint32_t));
    leaf_prev = (uint32_t*)calloc(num_pages, sizeof(uint32_t));
    leaf_next = (uint32_t*)calloc(num_pages, sizeof(uint32_t));
    if (queue == NULL || expected_parent == NULL || seen == NULL ||
        node_type == NULL || resolved == NULL || subtree_min == NULL ||
        subtree_max == NULL || leaf_prev == NULL || leaf_next == NULL) {
        goto cleanup;
    }

    queue[queue_count++] = root_page_num;
    seen[root_page_num] = 1u;
    expected_parent[root_page_num] = 0u;

    while (queue_head < queue_count) {
        uint32_t page_num = queue[queue_head++];
        PagerPageHandle handle;
        if (tinydb_fixed_v1_tree_page_is_free(pager, page_num) ||
            !tinydb_fixed_v1_tree_pin_read(pager, page_num, &handle)) {
            goto cleanup;
        }

        const uint8_t* page = (const uint8_t*)handle.data;
        uint8_t type = page[NODE_TYPE_OFFSET];
        uint8_t is_root = page[IS_ROOT_OFFSET];
        uint32_t parent = tinydb_fixed_v1_read_u32(page, PARENT_POINTER_OFFSET);
        bool header_valid = (type == (uint8_t)NODE_LEAF ||
                             type == (uint8_t)NODE_INTERNAL) &&
                            ((page_num == root_page_num && is_root != 0u &&
                              parent == 0u) ||
                             (page_num != root_page_num && is_root == 0u &&
                              parent == expected_parent[page_num]));
        if (!header_valid) {
            (void)tinydb_fixed_v1_tree_unpin_read(&handle);
            goto cleanup;
        }
        node_type[page_num] = type;

        if (type == (uint8_t)NODE_LEAF) {
            uint32_t cell_count = tinydb_fixed_v1_read_u32(
                page, LEAF_NODE_NUM_CELLS_OFFSET);
            if (cell_count > LEAF_NODE_MAX_CELLS ||
                (page_num != root_page_num && cell_count == 0u)) {
                (void)tinydb_fixed_v1_tree_unpin_read(&handle);
                goto cleanup;
            }
            leaf_prev[page_num] = tinydb_fixed_v1_read_u32(
                page, LEAF_NODE_PREV_LEAF_OFFSET);
            leaf_next[page_num] = tinydb_fixed_v1_read_u32(
                page, LEAF_NODE_NEXT_LEAF_OFFSET);
            if ((leaf_prev[page_num] != 0u &&
                 leaf_prev[page_num] >= num_pages) ||
                (leaf_next[page_num] != 0u &&
                 leaf_next[page_num] >= num_pages)) {
                (void)tinydb_fixed_v1_tree_unpin_read(&handle);
                goto cleanup;
            }

            uint32_t previous_key = 0u;
            for (uint32_t i = 0u; i < cell_count; i++) {
                uint32_t key_offset = LEAF_NODE_HEADER_SIZE +
                    i * LEAF_NODE_CELL_SIZE + LEAF_NODE_KEY_OFFSET;
                uint32_t key = tinydb_fixed_v1_read_u32(page, key_offset);
                if (i > 0u && key <= previous_key) {
                    (void)tinydb_fixed_v1_tree_unpin_read(&handle);
                    goto cleanup;
                }
                if (i == 0u) subtree_min[page_num] = key;
                subtree_max[page_num] = key;
                previous_key = key;
            }
            if (cell_count > 0u || page_num == root_page_num) {
                resolved[page_num] = 1u;
                resolved_count++;
            }
            leaf_count++;
        } else {
            uint32_t key_count = tinydb_fixed_v1_read_u32(
                page, INTERNAL_NODE_NUM_KEYS_OFFSET);
            if (key_count == 0u || key_count > INTERNAL_NODE_MAX_KEYS) {
                (void)tinydb_fixed_v1_tree_unpin_read(&handle);
                goto cleanup;
            }

            uint32_t previous_separator = 0u;
            for (uint32_t i = 0u; i <= key_count; i++) {
                uint32_t child = i == key_count
                    ? tinydb_fixed_v1_read_u32(page,
                          INTERNAL_NODE_RIGHT_CHILD_OFFSET)
                    : tinydb_fixed_v1_read_u32(page,
                          INTERNAL_NODE_HEADER_SIZE +
                          i * INTERNAL_NODE_CELL_SIZE);
                if (child == 0u || child == INVALID_PAGE_NUM ||
                    child >= num_pages || seen[child] != 0u) {
                    (void)tinydb_fixed_v1_tree_unpin_read(&handle);
                    goto cleanup;
                }
                if (i < key_count) {
                    uint32_t separator = tinydb_fixed_v1_read_u32(
                        page, INTERNAL_NODE_HEADER_SIZE +
                        i * INTERNAL_NODE_CELL_SIZE + INTERNAL_NODE_CHILD_SIZE);
                    if (i > 0u && separator <= previous_separator) {
                        (void)tinydb_fixed_v1_tree_unpin_read(&handle);
                        goto cleanup;
                    }
                    previous_separator = separator;
                }
                seen[child] = 1u;
                expected_parent[child] = page_num;
                queue[queue_count++] = child;
            }
        }

        if (!tinydb_fixed_v1_tree_unpin_read(&handle)) goto cleanup;
    }

    /* Resolve subtree ranges bottom-up and pin separator routing to child max. */
    while (resolved_count < queue_count) {
        uint32_t progress = 0u;
        for (uint32_t q = 0u; q < queue_count; q++) {
            uint32_t page_num = queue[q];
            if (resolved[page_num] != 0u ||
                node_type[page_num] != (uint8_t)NODE_INTERNAL) {
                continue;
            }
            PagerPageHandle handle;
            if (!tinydb_fixed_v1_tree_pin_read(pager, page_num, &handle)) {
                goto cleanup;
            }
            const uint8_t* page = (const uint8_t*)handle.data;
            uint32_t key_count = tinydb_fixed_v1_read_u32(
                page, INTERNAL_NODE_NUM_KEYS_OFFSET);
            bool children_ready = true;
            uint32_t first_child = 0u;
            uint32_t previous_child = 0u;
            for (uint32_t i = 0u; i <= key_count; i++) {
                uint32_t child = i == key_count
                    ? tinydb_fixed_v1_read_u32(page,
                          INTERNAL_NODE_RIGHT_CHILD_OFFSET)
                    : tinydb_fixed_v1_read_u32(page,
                          INTERNAL_NODE_HEADER_SIZE +
                          i * INTERNAL_NODE_CELL_SIZE);
                if (child >= num_pages || resolved[child] == 0u) {
                    children_ready = false;
                    break;
                }
                if (i == 0u) first_child = child;
                if (i > 0u && subtree_max[previous_child] >= subtree_min[child]) {
                    children_ready = false;
                    break;
                }
                if (i < key_count) {
                    uint32_t separator = tinydb_fixed_v1_read_u32(
                        page, INTERNAL_NODE_HEADER_SIZE +
                        i * INTERNAL_NODE_CELL_SIZE + INTERNAL_NODE_CHILD_SIZE);
                    if (separator != subtree_max[child]) {
                        children_ready = false;
                        break;
                    }
                }
                previous_child = child;
            }
            if (!tinydb_fixed_v1_tree_unpin_read(&handle)) goto cleanup;
            if (!children_ready) continue;
            subtree_min[page_num] = subtree_min[first_child];
            subtree_max[page_num] = subtree_max[previous_child];
            resolved[page_num] = 1u;
            resolved_count++;
            progress++;
        }
        if (progress == 0u) goto cleanup;
    }

    /* Validate that all leaves form one reciprocal, globally ordered chain. */
    if (leaf_count > 0u) {
        uint32_t leftmost = 0u;
        uint32_t leftmost_count = 0u;
        for (uint32_t q = 0u; q < queue_count; q++) {
            uint32_t page_num = queue[q];
            if (node_type[page_num] != (uint8_t)NODE_LEAF) continue;
            if (leaf_prev[page_num] == 0u) {
                leftmost = page_num;
                leftmost_count++;
            } else {
                uint32_t prev = leaf_prev[page_num];
                if (prev >= num_pages || seen[prev] == 0u ||
                    node_type[prev] != (uint8_t)NODE_LEAF ||
                    leaf_next[prev] != page_num ||
                    subtree_max[prev] >= subtree_min[page_num]) {
                    goto cleanup;
                }
            }
            if (leaf_next[page_num] != 0u) {
                uint32_t next = leaf_next[page_num];
                if (next >= num_pages || seen[next] == 0u ||
                    node_type[next] != (uint8_t)NODE_LEAF ||
                    leaf_prev[next] != page_num ||
                    subtree_max[page_num] >= subtree_min[next]) {
                    goto cleanup;
                }
            }
        }
        if (leftmost_count != 1u) goto cleanup;

        uint32_t chain_count = 0u;
        if (leftmost == 0u && root_page_num == 0u && leaf_count == 1u &&
            node_type[0u] == (uint8_t)NODE_LEAF && leaf_next[0u] == 0u) {
            chain_count = 1u;
        } else {
            uint32_t current = leftmost;
            while (current != 0u) {
                if (++chain_count > leaf_count) goto cleanup;
                current = leaf_next[current];
            }
        }
        if (chain_count != leaf_count) goto cleanup;
    }

    ownership_out->pages = queue;
    ownership_out->page_count = queue_count;
    queue = NULL;
    valid = true;

cleanup:
    free(queue);
    free(expected_parent);
    free(seen);
    free(node_type);
    free(resolved);
    free(subtree_min);
    free(subtree_max);
    free(leaf_prev);
    free(leaf_next);
    if (!valid) tinydb_fixed_v1_tree_ownership_destroy(ownership_out);
    return valid;
}

static inline bool tinydb_fixed_v1_tree_collect_ownership(
    Pager* pager,
    uint32_t root_page_num,
    TinyDBFixedV1TreeOwnership* ownership_out) {
    return tinydb_fixed_v1_tree_collect_ownership_impl(
        pager, root_page_num, false, ownership_out);
}

static inline bool tinydb_fixed_v1_page_zero_is_retired(Pager* pager) {
    PagerPageHandle handle;
    if (pager == NULL || pager->num_pages == 0u ||
        !tinydb_fixed_v1_tree_pin_read(pager, 0u, &handle)) {
        return false;
    }
    const uint8_t* page = (const uint8_t*)handle.data;
    bool retired = page[NODE_TYPE_OFFSET] == (uint8_t)NODE_LEAF &&
                   page[IS_ROOT_OFFSET] != 0u &&
                   tinydb_fixed_v1_read_u32(page, PARENT_POINTER_OFFSET) == 0u &&
                   tinydb_fixed_v1_read_u32(page, LEAF_NODE_NUM_CELLS_OFFSET) == 0u &&
                   tinydb_fixed_v1_read_u32(page, LEAF_NODE_PREV_LEAF_OFFSET) == 0u &&
                   tinydb_fixed_v1_read_u32(page, LEAF_NODE_NEXT_LEAF_OFFSET) == 0u;
    if (!tinydb_fixed_v1_tree_unpin_read(&handle)) return false;
    return retired;
}

static inline bool tinydb_fixed_v1_tree_retire_page_zero_root(Pager* pager) {
    TinyDBFixedV1TreeOwnership ownership = {NULL, 0u};
    PagerPageHandle root_handle;
    bool reclaimed = true;
    bool wrote_root = false;

    if (pager == NULL || !pager->in_transaction || pager->num_pages == 0u) {
        return false;
    }

    /* A checkpointed tombstone proves an earlier retirement already finished. */
    if (tinydb_fixed_v1_page_zero_is_retired(pager)) return true;

    if (!tinydb_fixed_v1_tree_collect_ownership_impl(
            pager, 0u, true, &ownership) || ownership.page_count == 0u ||
        ownership.pages[0] != 0u) {
        tinydb_fixed_v1_tree_ownership_destroy(&ownership);
        return false;
    }

    if (ownership.page_count > 1u) {
        reclaimed = tinydb_compact_v2_migration_pager_reclaim_claims(
            pager, ownership.pages + 1u, ownership.page_count - 1u);
    }
    if (!reclaimed || !pager_pin_page_handle(pager, 0u, &root_handle)) {
        tinydb_fixed_v1_tree_ownership_destroy(&ownership);
        return false;
    }
    if (!pager_page_handle_acquire_write(&root_handle)) {
        (void)pager_release_page_handle(&root_handle);
        tinydb_fixed_v1_tree_ownership_destroy(&ownership);
        return false;
    }

    uint8_t* root = (uint8_t*)root_handle.data;
    root[NODE_TYPE_OFFSET] = (uint8_t)NODE_LEAF;
    root[IS_ROOT_OFFSET] = 1u;
    tinydb_fixed_v1_write_u32(root, PARENT_POINTER_OFFSET, 0u);
    tinydb_fixed_v1_write_u32(root, LEAF_NODE_NUM_CELLS_OFFSET, 0u);
    tinydb_fixed_v1_write_u32(root, LEAF_NODE_NEXT_LEAF_OFFSET, 0u);
    tinydb_fixed_v1_write_u32(root, LEAF_NODE_PREV_LEAF_OFFSET, 0u);
    mark_page_dirty(pager, 0u);
    wrote_root = pager_page_handle_release_write(&root_handle);
    if (!pager_release_page_handle(&root_handle)) wrote_root = false;

    tinydb_fixed_v1_tree_ownership_destroy(&ownership);
    return wrote_root;
}

static inline bool tinydb_fixed_v1_tree_reclaim(Pager* pager,
                                                 uint32_t old_root_page_num) {
    TinyDBFixedV1TreeOwnership ownership = {NULL, 0u};
    if (pager == NULL || !pager->in_transaction ||
        old_root_page_num == INVALID_PAGE_NUM) {
        return false;
    }
    if (old_root_page_num == 0u) {
        return tinydb_fixed_v1_tree_retire_page_zero_root(pager);
    }

    /* A previously checkpointed reclaim is an idempotent success on retry. */
    if (tinydb_fixed_v1_tree_page_is_free(pager, old_root_page_num)) return true;

    if (!tinydb_fixed_v1_tree_collect_ownership(
            pager, old_root_page_num, &ownership)) {
        return false;
    }

    bool reclaimed = tinydb_compact_v2_migration_pager_reclaim_claims(
        pager, ownership.pages, ownership.page_count);
    tinydb_fixed_v1_tree_ownership_destroy(&ownership);
    return reclaimed;
}

#endif /* FIXED_V1_TREE_RECLAIM_H */
