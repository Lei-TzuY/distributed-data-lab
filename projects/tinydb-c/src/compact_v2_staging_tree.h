#ifndef COMPACT_V2_STAGING_TREE_H
#define COMPACT_V2_STAGING_TREE_H

#include "leaf_cursor_read.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Pure-memory topology seam for fixed-V1 -> compact-V2 rebuilds.
 *
 * The leaf chain has already been populated by
 * tinydb_fixed_v1_tree_stage_compact_v2_leaf_chain().  This layer adds one
 * unpublished internal root above two or more private V2 leaves.  It does not
 * allocate Pager pages and does not publish a catalog/root/WAL change.
 *
 * This deliberately handles one internal level first.  Chains larger than an
 * internal page can route are rejected so a future multi-level builder can add
 * hierarchy construction without weakening the publication boundary here.
 */
typedef struct {
    TinyDBCompactV2StagingLeafChain* leaves;
    unsigned char* root_image;
    uint32_t root_page_num;
    bool built;
} TinyDBCompactV2StagingSingleRoot;

static inline bool tinydb_compact_v2_staging_root_page_number_is_private(
    const TinyDBCompactV2StagingLeafChain* chain,
    uint32_t root_page_num) {
    if (chain == NULL || chain->page_numbers == NULL || root_page_num == 0u) {
        return false;
    }
    for (uint32_t i = 0u; i < chain->page_count; i++) {
        if (chain->page_numbers[i] == root_page_num) return false;
    }
    return true;
}

static inline bool tinydb_compact_v2_staging_leaf_max_key(
    const TinyDBCompactV2StagingLeafChain* chain,
    uint32_t leaf_index,
    uint32_t* max_key) {
    if (chain == NULL || max_key == NULL || leaf_index >= chain->page_count) {
        return false;
    }
    const unsigned char* leaf =
        tinydb_compact_v2_staging_page_const(chain, leaf_index);
    uint32_t count = 0u;
    if (leaf == NULL ||
        !tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE) ||
        !tinydb_leaf_page_count(leaf, PAGE_SIZE, &count) ||
        count == 0u) {
        return false;
    }
    return tinydb_leaf_page_key_at(leaf, PAGE_SIZE, count - 1u, max_key);
}

static inline bool tinydb_compact_v2_staging_single_root_validate(
    const TinyDBCompactV2StagingSingleRoot* staging) {
    if (staging == NULL || staging->leaves == NULL ||
        staging->root_image == NULL || !staging->built ||
        !tinydb_compact_v2_staging_leaf_chain_validate(staging->leaves) ||
        staging->leaves->page_count < 2u ||
        staging->leaves->page_count > INTERNAL_NODE_MAX_KEYS + 1u ||
        !tinydb_compact_v2_staging_root_page_number_is_private(
            staging->leaves, staging->root_page_num)) {
        return false;
    }

    void* root = staging->root_image;
    uint32_t expected_keys = staging->leaves->page_count - 1u;
    if (get_node_type(root) != NODE_INTERNAL ||
        !is_node_root(root) ||
        *node_parent(root) != 0u ||
        *internal_node_num_keys(root) != expected_keys ||
        *internal_node_right_child(root) !=
            staging->leaves->page_numbers[staging->leaves->page_count - 1u]) {
        return false;
    }

    for (uint32_t i = 0u; i < staging->leaves->page_count; i++) {
        const unsigned char* leaf =
            tinydb_compact_v2_staging_page_const(staging->leaves, i);
        if (leaf == NULL || get_node_type((void*)leaf) != NODE_LEAF ||
            is_node_root((void*)leaf) ||
            *node_parent((void*)leaf) != staging->root_page_num) {
            return false;
        }

        if (i < expected_keys) {
            uint32_t max_key = 0u;
            if (*internal_node_child(root, i) != staging->leaves->page_numbers[i] ||
                !tinydb_compact_v2_staging_leaf_max_key(
                    staging->leaves, i, &max_key) ||
                *internal_node_key(root, i) != max_key) {
                return false;
            }
        }
    }
    return true;
}

static inline bool tinydb_compact_v2_staging_single_root_build(
    TinyDBCompactV2StagingSingleRoot* staging,
    TinyDBCompactV2StagingLeafChain* leaves,
    unsigned char* root_image,
    uint32_t root_page_num) {
    if (staging == NULL || leaves == NULL || root_image == NULL ||
        !tinydb_compact_v2_staging_leaf_chain_validate(leaves) ||
        leaves->page_count < 2u ||
        leaves->page_count > INTERNAL_NODE_MAX_KEYS + 1u ||
        !tinydb_compact_v2_staging_root_page_number_is_private(
            leaves, root_page_num)) {
        return false;
    }

    /* Preflight every separator before mutating any caller-owned image. */
    uint32_t separator_keys[INTERNAL_NODE_MAX_KEYS];
    uint32_t num_keys = leaves->page_count - 1u;
    for (uint32_t i = 0u; i < num_keys; i++) {
        if (!tinydb_compact_v2_staging_leaf_max_key(
                leaves, i, &separator_keys[i])) {
            return false;
        }
        if (i > 0u && separator_keys[i] <= separator_keys[i - 1u]) {
            return false;
        }
    }
    uint32_t right_max = 0u;
    if (!tinydb_compact_v2_staging_leaf_max_key(
            leaves, leaves->page_count - 1u, &right_max) ||
        (num_keys > 0u && right_max <= separator_keys[num_keys - 1u])) {
        return false;
    }

    unsigned char staged_root[PAGE_SIZE];
    memset(staged_root, 0, sizeof(staged_root));
    set_node_type(staged_root, NODE_INTERNAL);
    set_node_root(staged_root, true);
    *node_parent(staged_root) = 0u;
    *internal_node_num_keys(staged_root) = num_keys;
    *internal_node_right_child(staged_root) =
        leaves->page_numbers[leaves->page_count - 1u];
    for (uint32_t i = 0u; i < num_keys; i++) {
        *internal_node_child(staged_root, i) = leaves->page_numbers[i];
        *internal_node_key(staged_root, i) = separator_keys[i];
    }

    /* All fallible work is complete. Publish only into private images. */
    memcpy(root_image, staged_root, PAGE_USABLE_SIZE);
    for (uint32_t i = 0u; i < leaves->page_count; i++) {
        unsigned char* leaf = tinydb_compact_v2_staging_page(leaves, i);
        set_node_root(leaf, false);
        *node_parent(leaf) = root_page_num;
    }

    staging->leaves = leaves;
    staging->root_image = root_image;
    staging->root_page_num = root_page_num;
    staging->built = true;
    return tinydb_compact_v2_staging_single_root_validate(staging);
}

#endif /* COMPACT_V2_STAGING_TREE_H */
