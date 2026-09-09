#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_LOCAL_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_LOCAL_H

#include "generic_index_epoch.h"
#include "internal_nonroot_merge_stage.h"
#include "record_delete_v2_internal_merge_root.h"
#include "slotted_v2_publish_batch.h"

/*
 * Publish the bounded non-root merge staged by internal_nonroot_merge_stage.h.
 *
 * This route intentionally stops before recursive ancestor underflow. The
 * target leaf-parent and its adjacent sibling must both be minimum two-child
 * internal nodes, while their common non-root ancestor must own at least three
 * children. The staging primitive chooses the obsolete parent so that removing
 * it does not change the ancestor subtree maximum; consequently no separator
 * above that ancestor needs to move.
 *
 * Local borrow runs before this helper, so a sibling that can donate never gets
 * merged. On success the ancestor, kept parent, and three surviving leaves are
 * published atomically. Only after publication are the singleton removed leaf
 * and obsolete internal parent placed on the Pager free list, preserving the
 * normal transaction/WAL rollback contract.
 */
static inline TinyDBInternalMergeRootResult
 tinydb_try_delete_v2_internal_merge_local(
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
    if (!tinydb_leaf_page_count(removed_leaf_before, PAGE_SIZE, &removed_count) ||
        removed_count != 1u ||
        !tinydb_leaf_page_key_at(removed_leaf_before,
                                 PAGE_SIZE,
                                 0u,
                                 &removed_only_key) ||
        removed_only_key != removed_key) {
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

    unsigned char target_after[PAGE_SIZE];
    memcpy(target_after,
           get_page(table->pager, target_parent_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(target_after, PAGE_SIZE) ||
        target_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            target_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t ancestor_num = tinydb_internal_borrow_read_u32(
        target_after + PARENT_POINTER_OFFSET);
    if (ancestor_num == 0u || ancestor_num == INVALID_PAGE_NUM ||
        ancestor_num >= table->pager->num_pages ||
        ancestor_num == schema->root_page_num ||
        ancestor_num == target_parent_num ||
        tinydb_internal_borrow_page_free(table->pager, ancestor_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char ancestor_after[PAGE_SIZE];
    memcpy(ancestor_after, get_page(table->pager, ancestor_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(ancestor_after, PAGE_SIZE) ||
        ancestor_after[IS_ROOT_OFFSET] != 0u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    uint32_t ancestor_key_count = tinydb_parent_stage_read_u32(
        ancestor_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (ancestor_key_count < 2u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t target_index = ancestor_key_count + 1u;
    for (uint32_t i = 0u; i <= ancestor_key_count; i++) {
        if (tinydb_parent_stage_child_at(ancestor_after, i) ==
            target_parent_num) {
            if (target_index != ancestor_key_count + 1u) {
                tinydb_internal_merge_root_message(
                    message, message_size,
                    "slotted V2 local merge found a duplicate target parent");
                return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
            }
            target_index = i;
        }
    }
    if (target_index > ancestor_key_count) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t target_left = tinydb_parent_stage_child_at(target_after, 0u);
    uint32_t target_right = tinydb_parent_stage_child_at(target_after, 1u);
    bool removed_rightmost = target_right == removed_leaf_page_num;
    bool removed_leftmost = target_left == removed_leaf_page_num;
    if (removed_rightmost == removed_leftmost) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t sibling_num = INVALID_PAGE_NUM;
    if (removed_rightmost) {
        if (target_index >= ancestor_key_count ||
            tinydb_parent_stage_key_at(ancestor_after, target_index) !=
                removed_key) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        sibling_num = tinydb_parent_stage_child_at(ancestor_after,
                                                    target_index + 1u);
    } else {
        if (target_index == 0u ||
            tinydb_parent_stage_key_at(target_after, 0u) != removed_key) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        sibling_num = tinydb_parent_stage_child_at(ancestor_after,
                                                    target_index - 1u);
    }

    if (sibling_num == 0u || sibling_num == INVALID_PAGE_NUM ||
        sibling_num >= table->pager->num_pages ||
        sibling_num == target_parent_num ||
        tinydb_internal_borrow_page_free(table->pager, sibling_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char sibling_after[PAGE_SIZE];
    memcpy(sibling_after, get_page(table->pager, sibling_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(sibling_after, PAGE_SIZE) ||
        sibling_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(sibling_after + PARENT_POINTER_OFFSET) !=
            ancestor_num) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "slotted V2 local merge found an invalid adjacent parent");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }
    if (tinydb_parent_stage_read_u32(
            sibling_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        /* A larger sibling should have been handled by local borrow. */
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t survivor_nums[3];
    if (removed_rightmost) {
        survivor_nums[0] = target_left;
        survivor_nums[1] = tinydb_parent_stage_child_at(sibling_after, 0u);
        survivor_nums[2] = tinydb_parent_stage_child_at(sibling_after, 1u);
    } else {
        survivor_nums[0] = tinydb_parent_stage_child_at(sibling_after, 0u);
        survivor_nums[1] = tinydb_parent_stage_child_at(sibling_after, 1u);
        survivor_nums[2] = target_right;
    }

    unsigned char survivor_after[3][PAGE_SIZE];
    void* survivor_pages[3] = {
        survivor_after[0], survivor_after[1], survivor_after[2]
    };
    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_nums[i] == 0u || survivor_nums[i] == INVALID_PAGE_NUM ||
            survivor_nums[i] >= table->pager->num_pages ||
            survivor_nums[i] == removed_leaf_page_num ||
            survivor_nums[i] == target_parent_num ||
            survivor_nums[i] == sibling_num ||
            survivor_nums[i] == ancestor_num ||
            tinydb_internal_borrow_page_free(table->pager, survivor_nums[i]) ||
            (i > 0u && survivor_nums[i] == survivor_nums[i - 1u])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        memcpy(survivor_after[i],
               get_page(table->pager, survivor_nums[i]),
               PAGE_SIZE);
    }

    uint32_t kept_parent_num = INVALID_PAGE_NUM;
    uint32_t obsolete_parent_num = INVALID_PAGE_NUM;
    if (!tinydb_stage_internal_nonroot_merge_after_v2_leaf_removal(
            ancestor_after,
            PAGE_SIZE,
            ancestor_num,
            target_after,
            PAGE_SIZE,
            target_parent_num,
            sibling_after,
            PAGE_SIZE,
            sibling_num,
            removed_leaf_before,
            PAGE_SIZE,
            removed_leaf_page_num,
            removed_key,
            survivor_pages,
            survivor_nums,
            &kept_parent_num,
            &obsolete_parent_num)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "slotted V2 local merge staging validation failed");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    if ((kept_parent_num != target_parent_num &&
         kept_parent_num != sibling_num) ||
        (obsolete_parent_num != target_parent_num &&
         obsolete_parent_num != sibling_num) ||
        kept_parent_num == obsolete_parent_num) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "slotted V2 local merge produced invalid parent ownership");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    unsigned char* kept_after =
        kept_parent_num == target_parent_num ? target_after : sibling_after;

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    TinyDBV2PublishEntry entries[5];
    entries[0].page_num = ancestor_num;
    entries[0].target = (unsigned char*)get_page(table->pager, ancestor_num);
    entries[0].staged = ancestor_after;
    entries[1].page_num = kept_parent_num;
    entries[1].target =
        (unsigned char*)get_page(table->pager, kept_parent_num);
    entries[1].staged = kept_after;
    for (uint32_t i = 0u; i < 3u; i++) {
        entries[2u + i].page_num = survivor_nums[i];
        entries[2u + i].target =
            (unsigned char*)get_page(table->pager, survivor_nums[i]);
        entries[2u + i].staged = survivor_after[i];
    }

    if (!tinydb_v2_publish_batch(entries, 5u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "slotted V2 local merge publication failed");
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

#endif /* TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_LOCAL_H */
