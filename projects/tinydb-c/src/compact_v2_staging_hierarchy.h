#ifndef COMPACT_V2_STAGING_HIERARCHY_H
#define COMPACT_V2_STAGING_HIERARCHY_H

#include "compact_v2_staging_tree.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Multi-level pure-memory B+tree topology builder for fixed-V1 -> compact-V2
 * rebuilds.  Leaves are produced by TinyDBCompactV2StagingLeafChain. Internal
 * page images and their future physical page numbers are owned by the caller.
 *
 * This layer deliberately performs no Pager allocation, dirtying, WAL,
 * checkpoint, catalog, root, or index publication.  It first preflights the
 * complete hierarchy geometry and page-number namespace, then builds all
 * internal levels bottom-up.  The resulting root is still unpublished and may
 * be discarded as a unit if a later durable-publication phase fails.
 */
typedef struct {
    TinyDBCompactV2StagingLeafChain* leaves;
    unsigned char* internal_images;
    const uint32_t* internal_page_numbers;
    uint32_t internal_capacity;
    uint32_t internal_count;
    uint32_t level_count;
    uint32_t root_page_num;
    unsigned char* root_image;
    bool built;
} TinyDBCompactV2StagingHierarchy;

typedef struct {
    uint32_t page_num;
    uint32_t max_key;
    unsigned char* image;
    bool is_leaf;
} TinyDBCompactV2StagingNodeRef;

static inline unsigned char* tinydb_compact_v2_staging_internal_page(
    TinyDBCompactV2StagingHierarchy* hierarchy,
    uint32_t index) {
    if (hierarchy == NULL || hierarchy->internal_images == NULL ||
        index >= hierarchy->internal_capacity) {
        return NULL;
    }
    return hierarchy->internal_images + (size_t)index * PAGE_SIZE;
}

static inline const unsigned char* tinydb_compact_v2_staging_internal_page_const(
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    uint32_t index) {
    if (hierarchy == NULL || hierarchy->internal_images == NULL ||
        index >= hierarchy->internal_capacity) {
        return NULL;
    }
    return hierarchy->internal_images + (size_t)index * PAGE_SIZE;
}

static inline bool tinydb_compact_v2_staging_required_internal_pages(
    uint32_t leaf_count,
    uint32_t* required_pages,
    uint32_t* required_levels) {
    const uint32_t fanout = INTERNAL_NODE_MAX_KEYS + 1u;
    if (required_pages == NULL || required_levels == NULL ||
        leaf_count < 2u || fanout < 2u) {
        return false;
    }

    uint64_t total = 0u;
    uint32_t levels = 0u;
    uint32_t nodes = leaf_count;
    while (nodes > 1u) {
        uint32_t parents = nodes / fanout + (nodes % fanout != 0u ? 1u : 0u);
        if (parents == 0u || parents >= nodes) return false;
        total += parents;
        if (total > UINT32_MAX) return false;
        nodes = parents;
        levels++;
    }

    *required_pages = (uint32_t)total;
    *required_levels = levels;
    return true;
}

static inline bool tinydb_compact_v2_staging_hierarchy_page_numbers_valid(
    const TinyDBCompactV2StagingLeafChain* leaves,
    const uint32_t* internal_page_numbers,
    uint32_t required_internal_pages) {
    if (leaves == NULL || leaves->page_numbers == NULL ||
        internal_page_numbers == NULL || required_internal_pages == 0u) {
        return false;
    }

    for (uint32_t i = 0u; i < required_internal_pages; i++) {
        uint32_t page_num = internal_page_numbers[i];
        if (page_num == 0u) return false;
        for (uint32_t leaf = 0u; leaf < leaves->page_count; leaf++) {
            if (page_num == leaves->page_numbers[leaf]) return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (page_num == internal_page_numbers[j]) return false;
        }
    }
    return true;
}

static inline void* tinydb_compact_v2_staging_hierarchy_find_page(
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    uint32_t page_num,
    bool* is_leaf) {
    if (hierarchy == NULL || hierarchy->leaves == NULL || page_num == 0u) {
        return NULL;
    }

    for (uint32_t i = 0u; i < hierarchy->leaves->page_count; i++) {
        if (hierarchy->leaves->page_numbers[i] == page_num) {
            if (is_leaf != NULL) *is_leaf = true;
            return (void*)tinydb_compact_v2_staging_page_const(hierarchy->leaves, i);
        }
    }
    for (uint32_t i = 0u; i < hierarchy->internal_count; i++) {
        if (hierarchy->internal_page_numbers[i] == page_num) {
            if (is_leaf != NULL) *is_leaf = false;
            return (void*)tinydb_compact_v2_staging_internal_page_const(hierarchy, i);
        }
    }
    return NULL;
}

static inline bool tinydb_compact_v2_staging_hierarchy_subtree_max_key(
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    uint32_t page_num,
    uint32_t depth,
    uint32_t* max_key) {
    if (hierarchy == NULL || max_key == NULL ||
        depth > hierarchy->internal_count + 1u) {
        return false;
    }

    bool is_leaf = false;
    void* image = tinydb_compact_v2_staging_hierarchy_find_page(
        hierarchy, page_num, &is_leaf);
    if (image == NULL) return false;
    if (is_leaf) {
        uint32_t leaf_index = 0u;
        while (leaf_index < hierarchy->leaves->page_count &&
               hierarchy->leaves->page_numbers[leaf_index] != page_num) {
            leaf_index++;
        }
        return leaf_index < hierarchy->leaves->page_count &&
               tinydb_compact_v2_staging_leaf_max_key(
                   hierarchy->leaves, leaf_index, max_key);
    }

    if (get_node_type(image) != NODE_INTERNAL) return false;
    uint32_t num_keys = *internal_node_num_keys(image);
    if (num_keys == 0u || num_keys > INTERNAL_NODE_MAX_KEYS) return false;
    return tinydb_compact_v2_staging_hierarchy_subtree_max_key(
        hierarchy,
        *internal_node_right_child(image),
        depth + 1u,
        max_key);
}

static inline uint32_t tinydb_compact_v2_staging_hierarchy_inbound_count(
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    uint32_t page_num) {
    if (hierarchy == NULL || page_num == 0u) return 0u;
    uint32_t inbound = 0u;
    for (uint32_t i = 0u; i < hierarchy->internal_count; i++) {
        const void* node = tinydb_compact_v2_staging_internal_page_const(hierarchy, i);
        if (node == NULL || get_node_type((void*)node) != NODE_INTERNAL) continue;
        uint32_t num_keys = *internal_node_num_keys((void*)node);
        if (num_keys > INTERNAL_NODE_MAX_KEYS) continue;
        for (uint32_t child = 0u; child < num_keys; child++) {
            if (*internal_node_child((void*)node, child) == page_num) inbound++;
        }
        if (*internal_node_right_child((void*)node) == page_num) inbound++;
    }
    return inbound;
}

static inline bool tinydb_compact_v2_staging_hierarchy_reaches_root(
    const TinyDBCompactV2StagingHierarchy* hierarchy,
    uint32_t page_num) {
    if (hierarchy == NULL || page_num == 0u || hierarchy->root_page_num == 0u) {
        return false;
    }
    uint32_t current = page_num;
    for (uint32_t depth = 0u; depth <= hierarchy->internal_count; depth++) {
        if (current == hierarchy->root_page_num) return true;
        bool is_leaf = false;
        void* image = tinydb_compact_v2_staging_hierarchy_find_page(
            hierarchy, current, &is_leaf);
        if (image == NULL) return false;
        (void)is_leaf;
        uint32_t parent = *node_parent(image);
        if (parent == 0u || parent == current) return false;
        current = parent;
    }
    return false;
}

static inline bool tinydb_compact_v2_staging_hierarchy_validate(
    const TinyDBCompactV2StagingHierarchy* hierarchy) {
    if (hierarchy == NULL || hierarchy->leaves == NULL ||
        hierarchy->internal_images == NULL ||
        hierarchy->internal_page_numbers == NULL || !hierarchy->built ||
        !tinydb_compact_v2_staging_leaf_chain_validate(hierarchy->leaves) ||
        hierarchy->leaves->page_count < 2u || hierarchy->internal_count == 0u ||
        hierarchy->internal_count > hierarchy->internal_capacity ||
        hierarchy->root_image == NULL || hierarchy->root_page_num == 0u) {
        return false;
    }

    uint32_t expected_internal = 0u;
    uint32_t expected_levels = 0u;
    if (!tinydb_compact_v2_staging_required_internal_pages(
            hierarchy->leaves->page_count,
            &expected_internal,
            &expected_levels) ||
        hierarchy->internal_count != expected_internal ||
        hierarchy->level_count != expected_levels ||
        !tinydb_compact_v2_staging_hierarchy_page_numbers_valid(
            hierarchy->leaves,
            hierarchy->internal_page_numbers,
            hierarchy->internal_count)) {
        return false;
    }

    bool found_root = false;
    for (uint32_t i = 0u; i < hierarchy->internal_count; i++) {
        void* node = (void*)tinydb_compact_v2_staging_internal_page_const(hierarchy, i);
        uint32_t page_num = hierarchy->internal_page_numbers[i];
        if (node == NULL || get_node_type(node) != NODE_INTERNAL) return false;
        uint32_t num_keys = *internal_node_num_keys(node);
        if (num_keys == 0u || num_keys > INTERNAL_NODE_MAX_KEYS) return false;

        bool is_root = page_num == hierarchy->root_page_num;
        if (is_node_root(node) != is_root) return false;
        if (is_root) {
            if (found_root || *node_parent(node) != 0u ||
                node != hierarchy->root_image ||
                tinydb_compact_v2_staging_hierarchy_inbound_count(
                    hierarchy, page_num) != 0u) {
                return false;
            }
            found_root = true;
        } else if (*node_parent(node) == 0u ||
                   tinydb_compact_v2_staging_hierarchy_inbound_count(
                       hierarchy, page_num) != 1u ||
                   !tinydb_compact_v2_staging_hierarchy_reaches_root(
                       hierarchy, page_num)) {
            return false;
        }

        uint32_t previous_separator = 0u;
        for (uint32_t child = 0u; child < num_keys; child++) {
            uint32_t child_page = *internal_node_child(node, child);
            bool child_is_leaf = false;
            void* child_image = tinydb_compact_v2_staging_hierarchy_find_page(
                hierarchy, child_page, &child_is_leaf);
            uint32_t child_max = 0u;
            if (child_image == NULL || child_page == page_num ||
                *node_parent(child_image) != page_num ||
                !tinydb_compact_v2_staging_hierarchy_subtree_max_key(
                    hierarchy, child_page, 0u, &child_max) ||
                *internal_node_key(node, child) != child_max ||
                (child > 0u && child_max <= previous_separator)) {
                return false;
            }
            (void)child_is_leaf;
            previous_separator = child_max;
        }

        uint32_t right_page = *internal_node_right_child(node);
        bool right_is_leaf = false;
        void* right_image = tinydb_compact_v2_staging_hierarchy_find_page(
            hierarchy, right_page, &right_is_leaf);
        uint32_t right_max = 0u;
        if (right_image == NULL || right_page == page_num ||
            *node_parent(right_image) != page_num ||
            !tinydb_compact_v2_staging_hierarchy_subtree_max_key(
                hierarchy, right_page, 0u, &right_max) ||
            right_max <= previous_separator) {
            return false;
        }
        (void)right_is_leaf;
    }
    if (!found_root) return false;

    for (uint32_t i = 0u; i < hierarchy->leaves->page_count; i++) {
        void* leaf = (void*)tinydb_compact_v2_staging_page_const(hierarchy->leaves, i);
        uint32_t page_num = hierarchy->leaves->page_numbers[i];
        if (leaf == NULL || get_node_type(leaf) != NODE_LEAF || is_node_root(leaf) ||
            *node_parent(leaf) == 0u ||
            tinydb_compact_v2_staging_hierarchy_inbound_count(
                hierarchy, page_num) != 1u ||
            !tinydb_compact_v2_staging_hierarchy_reaches_root(
                hierarchy, page_num)) {
            return false;
        }
    }
    return true;
}

static inline bool tinydb_compact_v2_staging_hierarchy_build(
    TinyDBCompactV2StagingHierarchy* hierarchy,
    TinyDBCompactV2StagingLeafChain* leaves,
    unsigned char* internal_images,
    const uint32_t* internal_page_numbers,
    uint32_t internal_capacity) {
    if (hierarchy == NULL || leaves == NULL || internal_images == NULL ||
        internal_page_numbers == NULL ||
        !tinydb_compact_v2_staging_leaf_chain_validate(leaves) ||
        leaves->page_count < 2u) {
        return false;
    }

    uint32_t required_internal = 0u;
    uint32_t required_levels = 0u;
    if (!tinydb_compact_v2_staging_required_internal_pages(
            leaves->page_count, &required_internal, &required_levels) ||
        required_internal > internal_capacity ||
        !tinydb_compact_v2_staging_hierarchy_page_numbers_valid(
            leaves, internal_page_numbers, required_internal)) {
        return false;
    }

    /* Allocate all transient descriptors before mutating caller-owned images. */
    TinyDBCompactV2StagingNodeRef* current =
        (TinyDBCompactV2StagingNodeRef*)malloc(
            (size_t)leaves->page_count * sizeof(*current));
    TinyDBCompactV2StagingNodeRef* next =
        (TinyDBCompactV2StagingNodeRef*)malloc(
            (size_t)leaves->page_count * sizeof(*next));
    if (current == NULL || next == NULL) {
        free(current);
        free(next);
        return false;
    }

    bool ok = false;
    uint32_t previous_max = 0u;
    for (uint32_t i = 0u; i < leaves->page_count; i++) {
        uint32_t max_key = 0u;
        unsigned char* image = tinydb_compact_v2_staging_page(leaves, i);
        if (image == NULL ||
            !tinydb_compact_v2_staging_leaf_max_key(leaves, i, &max_key) ||
            (i > 0u && max_key <= previous_max)) {
            goto done;
        }
        current[i].page_num = leaves->page_numbers[i];
        current[i].max_key = max_key;
        current[i].image = image;
        current[i].is_leaf = true;
        previous_max = max_key;
    }

    /* No fallible allocation or geometry checks remain beyond this point. */
    memset(internal_images, 0, (size_t)required_internal * PAGE_SIZE);
    uint32_t current_count = leaves->page_count;
    uint32_t internal_index = 0u;
    uint32_t built_levels = 0u;

    while (current_count > 1u) {
        const uint32_t fanout = INTERNAL_NODE_MAX_KEYS + 1u;
        uint32_t parent_count =
            current_count / fanout + (current_count % fanout != 0u ? 1u : 0u);
        uint32_t base_children = current_count / parent_count;
        uint32_t extra_children = current_count % parent_count;
        uint32_t child_offset = 0u;

        for (uint32_t parent = 0u; parent < parent_count; parent++) {
            uint32_t child_count = base_children + (parent < extra_children ? 1u : 0u);
            if (child_count < 2u || child_count > fanout ||
                internal_index >= required_internal) {
                goto done;
            }

            unsigned char* node = internal_images + (size_t)internal_index * PAGE_SIZE;
            uint32_t node_page_num = internal_page_numbers[internal_index];
            set_node_type(node, NODE_INTERNAL);
            set_node_root(node, false);
            *node_parent(node) = 0u;
            *internal_node_num_keys(node) = child_count - 1u;
            *internal_node_right_child(node) =
                current[child_offset + child_count - 1u].page_num;

            for (uint32_t child = 0u; child < child_count; child++) {
                TinyDBCompactV2StagingNodeRef* ref = &current[child_offset + child];
                set_node_root(ref->image, false);
                *node_parent(ref->image) = node_page_num;
                if (child + 1u < child_count) {
                    *internal_node_child(node, child) = ref->page_num;
                    *internal_node_key(node, child) = ref->max_key;
                }
            }

            next[parent].page_num = node_page_num;
            next[parent].max_key = current[child_offset + child_count - 1u].max_key;
            next[parent].image = node;
            next[parent].is_leaf = false;
            child_offset += child_count;
            internal_index++;
        }
        if (child_offset != current_count) goto done;

        TinyDBCompactV2StagingNodeRef* swap = current;
        current = next;
        next = swap;
        current_count = parent_count;
        built_levels++;
    }

    if (internal_index != required_internal || built_levels != required_levels ||
        current_count != 1u || current[0].is_leaf) {
        goto done;
    }

    set_node_root(current[0].image, true);
    *node_parent(current[0].image) = 0u;
    hierarchy->leaves = leaves;
    hierarchy->internal_images = internal_images;
    hierarchy->internal_page_numbers = internal_page_numbers;
    hierarchy->internal_capacity = internal_capacity;
    hierarchy->internal_count = required_internal;
    hierarchy->level_count = required_levels;
    hierarchy->root_page_num = current[0].page_num;
    hierarchy->root_image = current[0].image;
    hierarchy->built = true;
    ok = tinydb_compact_v2_staging_hierarchy_validate(hierarchy);
    if (!ok) hierarchy->built = false;

done:
    free(current);
    free(next);
    return ok;
}

#endif /* COMPACT_V2_STAGING_HIERARCHY_H */
