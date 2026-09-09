#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_WIDE_ROOT_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_WIDE_ROOT_H

#include "record_delete_v2_internal_merge_root.h"

/*
 * Merge adjacent minimum two-child internal siblings beneath a root that has
 * at least three children. Unlike the one-key-root contraction path, the root
 * remains internal and keeps its stable page identity; one obsolete internal
 * child is removed from the root while the adjacent survivor is rebuilt with
 * the three remaining leaves.
 *
 * Two boundary-aligned shapes are intentionally supported:
 *
 *   target [A, removed] + right sibling [C, D]
 *      -> keep right sibling as [A, C, D], remove target from root
 *
 *   left sibling [A, B] + target [removed, D]
 *      -> keep target as [A, B, D], remove left sibling from root
 *
 * Choosing the parent on the far side of the removed boundary lets TinyDB use
 * the existing validated internal-child-removal primitive on the root without
 * inventing a second root-rewrite algorithm. The kept subtree maximum is
 * unchanged, so all remaining root separators are preserved by the normal
 * child-removal rebuild. Root + kept parent + three surviving leaf images are
 * published atomically before the removed leaf and obsolete internal parent
 * enter the Pager free list.
 */
static inline TinyDBInternalMergeRootResult
 tinydb_try_delete_v2_internal_merge_wide_root(
    Table* table,
    const TableSchema* schema,
    uint32_t removed_leaf_page_num,
    const unsigned char removed_leaf_before[PAGE_SIZE],
    uint32_t removed_key,
    char* message,
    size_t message_size) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        removed_leaf_before == NULL || removed_leaf_page_num == 0u ||
        removed_leaf_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num >= table->pager->num_pages ||
        removed_leaf_page_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, removed_leaf_page_num) ||
        tinydb_leaf_format_detect_page(removed_leaf_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed_leaf_before, PAGE_SIZE)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t removed_count = 0u;
    uint32_t removed_only_key = 0u;
    uint32_t removed_prev = INVALID_PAGE_NUM;
    uint32_t removed_next = INVALID_PAGE_NUM;
    if (!tinydb_leaf_page_count(removed_leaf_before, PAGE_SIZE, &removed_count) ||
        removed_count != 1u ||
        !tinydb_leaf_page_key_at(removed_leaf_before,
                                 PAGE_SIZE,
                                 0u,
                                 &removed_only_key) ||
        removed_only_key != removed_key ||
        !tinydb_leaf_page_prev(removed_leaf_before, PAGE_SIZE, &removed_prev) ||
        !tinydb_leaf_page_next(removed_leaf_before, PAGE_SIZE, &removed_next)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t target_parent_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (target_parent_num == 0u || target_parent_num == INVALID_PAGE_NUM ||
        target_parent_num >= table->pager->num_pages ||
        target_parent_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, target_parent_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char target_before[PAGE_SIZE];
    memcpy(target_before, get_page(table->pager, target_parent_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(target_before, PAGE_SIZE) ||
        target_before[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(target_before + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t root_num = tinydb_internal_borrow_read_u32(
        target_before + PARENT_POINTER_OFFSET);
    if (root_num != schema->root_page_num || root_num >= table->pager->num_pages ||
        tinydb_internal_borrow_page_free(table->pager, root_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char root_after[PAGE_SIZE];
    memcpy(root_after, get_page(table->pager, root_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        root_after[IS_ROOT_OFFSET] == 0u ||
        tinydb_internal_borrow_read_u32(root_after + PARENT_POINTER_OFFSET) != 0u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    uint32_t root_key_count = tinydb_parent_stage_read_u32(
        root_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (root_key_count < 2u) return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;

    uint32_t target_index = root_key_count + 1u;
    for (uint32_t i = 0u; i <= root_key_count; i++) {
        if (tinydb_parent_stage_child_at(root_after, i) == target_parent_num) {
            if (target_index != root_key_count + 1u) {
                return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
            }
            target_index = i;
        }
    }
    if (target_index == root_key_count + 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t target_left = tinydb_parent_stage_child_at(target_before, 0u);
    uint32_t target_right = tinydb_parent_stage_child_at(target_before, 1u);
    bool removed_rightmost = target_right == removed_leaf_page_num;
    bool removed_leftmost = target_left == removed_leaf_page_num;
    if (removed_rightmost == removed_leftmost) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t kept_parent_num = INVALID_PAGE_NUM;
    uint32_t obsolete_parent_num = INVALID_PAGE_NUM;
    uint32_t obsolete_parent_max = 0u;
    uint32_t survivor_nums[3] = {0u, 0u, 0u};
    uint32_t survivor_maxes[3] = {0u, 0u, 0u};
    unsigned char survivor_pages[3][PAGE_SIZE];

    if (removed_rightmost) {
        if (target_index >= root_key_count ||
            removed_prev != target_left ||
            tinydb_parent_stage_key_at(target_before, 0u) >= removed_key ||
            tinydb_parent_stage_key_at(root_after, target_index) != removed_key) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }

        kept_parent_num = tinydb_parent_stage_child_at(root_after,
                                                        target_index + 1u);
        obsolete_parent_num = target_parent_num;
        obsolete_parent_max = removed_key;
        if (kept_parent_num == 0u || kept_parent_num == INVALID_PAGE_NUM ||
            kept_parent_num >= table->pager->num_pages ||
            kept_parent_num == obsolete_parent_num ||
            tinydb_internal_borrow_page_free(table->pager, kept_parent_num)) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }

        unsigned char kept_before[PAGE_SIZE];
        memcpy(kept_before, get_page(table->pager, kept_parent_num), PAGE_SIZE);
        if (!tinydb_parent_stage_validate(kept_before, PAGE_SIZE) ||
            kept_before[IS_ROOT_OFFSET] != 0u ||
            tinydb_internal_borrow_read_u32(kept_before + PARENT_POINTER_OFFSET) !=
                root_num ||
            tinydb_parent_stage_read_u32(kept_before + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
                1u) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }

        survivor_nums[0] = target_left;
        survivor_nums[1] = tinydb_parent_stage_child_at(kept_before, 0u);
        survivor_nums[2] = tinydb_parent_stage_child_at(kept_before, 1u);
        if (removed_next != survivor_nums[1]) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }

        memcpy(survivor_pages[0],
               get_page(table->pager, survivor_nums[0]),
               PAGE_SIZE);
        memcpy(survivor_pages[1],
               get_page(table->pager, survivor_nums[1]),
               PAGE_SIZE);
        memcpy(survivor_pages[2],
               get_page(table->pager, survivor_nums[2]),
               PAGE_SIZE);

        uint32_t mins[3];
        uint32_t prevs[3];
        uint32_t nexts[3];
        if (!tinydb_internal_merge_root_leaf_valid(survivor_pages[0],
                                                   target_parent_num,
                                                   &mins[0],
                                                   &survivor_maxes[0],
                                                   &prevs[0],
                                                   &nexts[0]) ||
            !tinydb_internal_merge_root_leaf_valid(survivor_pages[1],
                                                   kept_parent_num,
                                                   &mins[1],
                                                   &survivor_maxes[1],
                                                   &prevs[1],
                                                   &nexts[1]) ||
            !tinydb_internal_merge_root_leaf_valid(survivor_pages[2],
                                                   kept_parent_num,
                                                   &mins[2],
                                                   &survivor_maxes[2],
                                                   &prevs[2],
                                                   &nexts[2]) ||
            survivor_maxes[0] != tinydb_parent_stage_key_at(target_before, 0u) ||
            survivor_maxes[1] != tinydb_parent_stage_key_at(kept_before, 0u) ||
            survivor_maxes[0] >= removed_key || removed_key >= mins[1] ||
            survivor_maxes[1] >= mins[2] ||
            nexts[0] != removed_leaf_page_num ||
            prevs[1] != removed_leaf_page_num ||
            nexts[1] != survivor_nums[2] || prevs[2] != survivor_nums[1]) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }

        if (!tinydb_stage_leaf_sibling_relink(survivor_pages[0],
                                              PAGE_SIZE,
                                              true,
                                              removed_leaf_page_num,
                                              survivor_nums[1]) ||
            !tinydb_stage_leaf_sibling_relink(survivor_pages[1],
                                              PAGE_SIZE,
                                              false,
                                              removed_leaf_page_num,
                                              survivor_nums[0])) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
    } else {
        if (target_index == 0u ||
            removed_next != target_right ||
            tinydb_parent_stage_key_at(target_before, 0u) != removed_key) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }

        obsolete_parent_num = tinydb_parent_stage_child_at(root_after,
                                                            target_index - 1u);
        kept_parent_num = target_parent_num;
        if (obsolete_parent_num == 0u ||
            obsolete_parent_num == INVALID_PAGE_NUM ||
            obsolete_parent_num >= table->pager->num_pages ||
            obsolete_parent_num == kept_parent_num ||
            tinydb_internal_borrow_page_free(table->pager, obsolete_parent_num)) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }

        unsigned char obsolete_before[PAGE_SIZE];
        memcpy(obsolete_before,
               get_page(table->pager, obsolete_parent_num),
               PAGE_SIZE);
        if (!tinydb_parent_stage_validate(obsolete_before, PAGE_SIZE) ||
            obsolete_before[IS_ROOT_OFFSET] != 0u ||
            tinydb_internal_borrow_read_u32(
                obsolete_before + PARENT_POINTER_OFFSET) != root_num ||
            tinydb_parent_stage_read_u32(
                obsolete_before + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }

        survivor_nums[0] = tinydb_parent_stage_child_at(obsolete_before, 0u);
        survivor_nums[1] = tinydb_parent_stage_child_at(obsolete_before, 1u);
        survivor_nums[2] = target_right;
        obsolete_parent_max = tinydb_parent_stage_key_at(root_after,
                                                          target_index - 1u);
        if (removed_prev != survivor_nums[1]) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }

        memcpy(survivor_pages[0],
               get_page(table->pager, survivor_nums[0]),
               PAGE_SIZE);
        memcpy(survivor_pages[1],
               get_page(table->pager, survivor_nums[1]),
               PAGE_SIZE);
        memcpy(survivor_pages[2],
               get_page(table->pager, survivor_nums[2]),
               PAGE_SIZE);

        uint32_t mins[3];
        uint32_t prevs[3];
        uint32_t nexts[3];
        if (!tinydb_internal_merge_root_leaf_valid(survivor_pages[0],
                                                   obsolete_parent_num,
                                                   &mins[0],
                                                   &survivor_maxes[0],
                                                   &prevs[0],
                                                   &nexts[0]) ||
            !tinydb_internal_merge_root_leaf_valid(survivor_pages[1],
                                                   obsolete_parent_num,
                                                   &mins[1],
                                                   &survivor_maxes[1],
                                                   &prevs[1],
                                                   &nexts[1]) ||
            !tinydb_internal_merge_root_leaf_valid(survivor_pages[2],
                                                   target_parent_num,
                                                   &mins[2],
                                                   &survivor_maxes[2],
                                                   &prevs[2],
                                                   &nexts[2]) ||
            survivor_maxes[0] !=
                tinydb_parent_stage_key_at(obsolete_before, 0u) ||
            survivor_maxes[1] != obsolete_parent_max ||
            survivor_maxes[1] >= removed_key || removed_key >= mins[2] ||
            nexts[0] != survivor_nums[1] || prevs[1] != survivor_nums[0] ||
            nexts[1] != removed_leaf_page_num ||
            prevs[2] != removed_leaf_page_num) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }

        if (!tinydb_stage_leaf_sibling_relink(survivor_pages[1],
                                              PAGE_SIZE,
                                              true,
                                              removed_leaf_page_num,
                                              survivor_nums[2]) ||
            !tinydb_stage_leaf_sibling_relink(survivor_pages[2],
                                              PAGE_SIZE,
                                              false,
                                              removed_leaf_page_num,
                                              survivor_nums[1])) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_nums[i] == 0u || survivor_nums[i] == INVALID_PAGE_NUM ||
            survivor_nums[i] >= table->pager->num_pages ||
            tinydb_internal_borrow_page_free(table->pager, survivor_nums[i]) ||
            (i > 0u && survivor_nums[i] == survivor_nums[i - 1u]) ||
            (i > 0u && survivor_maxes[i - 1u] >= survivor_maxes[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
        tinydb_parent_stage_write_u32(survivor_pages[i] + PARENT_POINTER_OFFSET,
                                      kept_parent_num);
    }

    unsigned char kept_after[PAGE_SIZE];
    if (!tinydb_internal_merge_root_build_internal(kept_after,
                                                   root_num,
                                                   survivor_nums,
                                                   survivor_maxes)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "slotted V2 wide-root merge could not rebuild the kept parent");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    uint32_t removed_parent_index = UINT32_MAX;
    bool root_max_changed = false;
    uint32_t root_new_max = 0u;
    if (!tinydb_stage_internal_child_remove(root_after,
                                             PAGE_SIZE,
                                             obsolete_parent_num,
                                             obsolete_parent_max,
                                             &removed_parent_index,
                                             &root_max_changed,
                                             &root_new_max)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "slotted V2 wide-root merge could not remove the obsolete parent");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }
    if (root_max_changed) {
        /* A root has no ancestor; max decrease is structurally local. */
        if (root_new_max == 0u) return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    uint32_t new_root_key_count = tinydb_parent_stage_read_u32(
        root_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
    bool found_kept = false;
    for (uint32_t i = 0u; i <= new_root_key_count; i++) {
        if (tinydb_parent_stage_child_at(root_after, i) == kept_parent_num) {
            if (found_kept) return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
            found_kept = true;
            if (i < new_root_key_count &&
                tinydb_parent_stage_key_at(root_after, i) != survivor_maxes[2]) {
                return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
            }
        }
    }
    if (!found_kept || !tinydb_parent_stage_validate(root_after, PAGE_SIZE)) {
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        uint32_t min_key = 0u;
        uint32_t max_key = 0u;
        uint32_t prev_page = INVALID_PAGE_NUM;
        uint32_t next_page = INVALID_PAGE_NUM;
        if (!tinydb_internal_merge_root_leaf_valid(survivor_pages[i],
                                                   kept_parent_num,
                                                   &min_key,
                                                   &max_key,
                                                   &prev_page,
                                                   &next_page) ||
            max_key != survivor_maxes[i]) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
        if (i > 0u && prev_page != survivor_nums[i - 1u]) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
        if (i + 1u < 3u && next_page != survivor_nums[i + 1u]) {
            return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
        }
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    TinyDBV2PublishEntry entries[5];
    entries[0].page_num = root_num;
    entries[0].target = (unsigned char*)get_page(table->pager, root_num);
    entries[0].staged = root_after;
    entries[1].page_num = kept_parent_num;
    entries[1].target = (unsigned char*)get_page(table->pager, kept_parent_num);
    entries[1].staged = kept_after;
    for (uint32_t i = 0u; i < 3u; i++) {
        entries[i + 2u].page_num = survivor_nums[i];
        entries[i + 2u].target =
            (unsigned char*)get_page(table->pager, survivor_nums[i]);
        entries[i + 2u].staged = survivor_pages[i];
    }
    if (!tinydb_v2_publish_batch(entries, 5u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "unable to atomically publish slotted V2 wide-root internal merge");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }
    for (uint32_t i = 0u; i < 5u; i++) {
        mark_page_dirty(table->pager, entries[i].page_num);
    }

    pager_free_page(table->pager, removed_leaf_page_num);
    pager_free_page(table->pager, obsolete_parent_num);
    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return TINYDB_INTERNAL_MERGE_ROOT_SUCCESS;
}

#endif
