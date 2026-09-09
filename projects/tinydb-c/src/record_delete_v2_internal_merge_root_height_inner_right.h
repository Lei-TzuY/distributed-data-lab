#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_HEIGHT_INNER_RIGHT_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_HEIGHT_INNER_RIGHT_H

#include "generic_index_epoch.h"
#include "internal_root_height_inner_right_lower_stage.h"
#include "internal_root_collapse_stage.h"
#include "record_delete_v2_internal_merge_root.h"
#include "slotted_v2_parent_max_stage.h"
#include "slotted_v2_publish_batch.h"

/*
 * Publish the inner-right height-4 -> height-3 contraction.
 *
 * The singleton removed leaf is the right child of the right bottom parent
 * beneath the left grandparent. Both bottom siblings and both grandparents are
 * minimum, so no redistribution is possible. The lower merge keeps the left
 * bottom parent as [A,B,C], repairs the cross-grandparent leaf boundary
 * C<->next, lowers the root separator to the new left-subtree maximum, rebuilds
 * the right grandparent as [kept,q0,q1], and promotes that image into the stable
 * root. All topology is staged before publication and reclamation.
 */
static inline TinyDBInternalMergeRootResult
 tinydb_try_delete_v2_internal_merge_root_height_inner_right(
    Table* table,
    const TableSchema* schema,
    uint32_t removed_leaf_page_num,
    const unsigned char removed_leaf_before[PAGE_SIZE],
    uint32_t removed_key,
    char* message,
    size_t message_size) {
    Pager* pager = table == NULL ? NULL : table->pager;
    if (pager == NULL || schema == NULL || removed_leaf_before == NULL ||
        removed_leaf_page_num == 0u ||
        removed_leaf_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num >= pager->num_pages ||
        removed_leaf_page_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(pager, removed_leaf_page_num) ||
        tinydb_leaf_format_detect_page(removed_leaf_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed_leaf_before, PAGE_SIZE)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t removed_count = 0u;
    uint32_t removed_only_key = 0u;
    uint32_t removed_next = INVALID_PAGE_NUM;
    if (!tinydb_leaf_page_count(removed_leaf_before, PAGE_SIZE, &removed_count) ||
        removed_count != 1u ||
        !tinydb_leaf_page_key_at(removed_leaf_before, PAGE_SIZE, 0u,
                                 &removed_only_key) ||
        removed_only_key != removed_key ||
        !tinydb_leaf_page_next(removed_leaf_before, PAGE_SIZE, &removed_next) ||
        removed_next == 0u || removed_next == INVALID_PAGE_NUM) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t obsolete_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (obsolete_num == 0u || obsolete_num == INVALID_PAGE_NUM ||
        obsolete_num >= pager->num_pages ||
        tinydb_internal_borrow_page_free(pager, obsolete_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char obsolete[PAGE_SIZE];
    memcpy(obsolete, get_page(pager, obsolete_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(obsolete, PAGE_SIZE) ||
        obsolete[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            obsolete + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(obsolete, 1u) != removed_leaf_page_num ||
        tinydb_parent_stage_key_at(obsolete, 0u) >= removed_key) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t left_grand_num = tinydb_internal_borrow_read_u32(
        obsolete + PARENT_POINTER_OFFSET);
    if (left_grand_num == 0u || left_grand_num == INVALID_PAGE_NUM ||
        left_grand_num >= pager->num_pages ||
        left_grand_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(pager, left_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char left_grand[PAGE_SIZE];
    memcpy(left_grand, get_page(pager, left_grand_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(left_grand, PAGE_SIZE) ||
        left_grand[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            left_grand + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(left_grand, 1u) != obsolete_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t kept_num = tinydb_parent_stage_child_at(left_grand, 0u);
    if (kept_num == 0u || kept_num == INVALID_PAGE_NUM ||
        kept_num >= pager->num_pages || kept_num == obsolete_num ||
        tinydb_internal_borrow_page_free(pager, kept_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char kept[PAGE_SIZE];
    memcpy(kept, get_page(pager, kept_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(kept, PAGE_SIZE) ||
        kept[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(kept + PARENT_POINTER_OFFSET) !=
            left_grand_num ||
        tinydb_parent_stage_read_u32(kept + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t root_num = tinydb_internal_borrow_read_u32(
        left_grand + PARENT_POINTER_OFFSET);
    if (root_num != schema->root_page_num || root_num >= pager->num_pages ||
        tinydb_internal_borrow_page_free(pager, root_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char root_after[PAGE_SIZE];
    memcpy(root_after, get_page(pager, root_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        root_after[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(root_after + PARENT_POINTER_OFFSET) != 0u ||
        tinydb_parent_stage_read_u32(
            root_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(root_after, 0u) != left_grand_num ||
        tinydb_parent_stage_key_at(root_after, 0u) != removed_key) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t right_grand_num = tinydb_parent_stage_child_at(root_after, 1u);
    if (right_grand_num == 0u || right_grand_num == INVALID_PAGE_NUM ||
        right_grand_num >= pager->num_pages ||
        right_grand_num == left_grand_num ||
        tinydb_internal_borrow_page_free(pager, right_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char right_grand[PAGE_SIZE];
    memcpy(right_grand, get_page(pager, right_grand_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(right_grand, PAGE_SIZE) ||
        right_grand[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(right_grand + PARENT_POINTER_OFFSET) !=
            root_num ||
        tinydb_parent_stage_read_u32(
            right_grand + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t q_num[2] = {
        tinydb_parent_stage_child_at(right_grand, 0u),
        tinydb_parent_stage_child_at(right_grand, 1u)
    };
    unsigned char q_after[2][PAGE_SIZE];
    for (uint32_t i = 0u; i < 2u; i++) {
        if (q_num[i] == 0u || q_num[i] == INVALID_PAGE_NUM ||
            q_num[i] >= pager->num_pages ||
            tinydb_internal_borrow_page_free(pager, q_num[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        memcpy(q_after[i], get_page(pager, q_num[i]), PAGE_SIZE);
        if (!tinydb_parent_stage_validate(q_after[i], PAGE_SIZE) ||
            q_after[i][IS_ROOT_OFFSET] != 0u ||
            tinydb_internal_borrow_read_u32(
                q_after[i] + PARENT_POINTER_OFFSET) != right_grand_num ||
            tinydb_parent_stage_read_u32(
                q_after[i] + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
    }

    uint32_t next_leaf_num = tinydb_parent_stage_child_at(q_after[0], 0u);
    if (next_leaf_num != removed_next || next_leaf_num == 0u ||
        next_leaf_num == INVALID_PAGE_NUM || next_leaf_num >= pager->num_pages ||
        tinydb_internal_borrow_page_free(pager, next_leaf_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char next_leaf_after[PAGE_SIZE];
    memcpy(next_leaf_after, get_page(pager, next_leaf_num), PAGE_SIZE);

    uint32_t survivor_nums[3] = {
        tinydb_parent_stage_child_at(kept, 0u),
        tinydb_parent_stage_child_at(kept, 1u),
        tinydb_parent_stage_child_at(obsolete, 0u)
    };
    unsigned char survivor_after[3][PAGE_SIZE];
    void* survivor_pages[3] = {
        survivor_after[0], survivor_after[1], survivor_after[2]
    };
    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_nums[i] == 0u || survivor_nums[i] == INVALID_PAGE_NUM ||
            survivor_nums[i] >= pager->num_pages ||
            tinydb_internal_borrow_page_free(pager, survivor_nums[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        memcpy(survivor_after[i], get_page(pager, survivor_nums[i]), PAGE_SIZE);
    }

    if (!tinydb_stage_root_height_inner_right_lower_merge(
            kept, PAGE_SIZE, kept_num,
            obsolete, PAGE_SIZE, obsolete_num,
            left_grand_num, right_grand_num,
            removed_leaf_before, PAGE_SIZE, removed_leaf_page_num, removed_key,
            survivor_pages, survivor_nums,
            next_leaf_after, PAGE_SIZE, next_leaf_num, q_num[0])) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t kept_count = 0u;
    uint32_t kept_max = 0u;
    if (!tinydb_leaf_page_count(survivor_after[2], PAGE_SIZE, &kept_count) ||
        kept_count == 0u ||
        !tinydb_leaf_page_key_at(survivor_after[2], PAGE_SIZE,
                                 kept_count - 1u, &kept_max) ||
        kept_max >= removed_key) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t root_child_index = UINT32_MAX;
    bool root_separator_changed = false;
    if (!tinydb_stage_parent_child_max_decrease(
            root_after, PAGE_SIZE, left_grand_num,
            removed_key, kept_max,
            &root_child_index, &root_separator_changed) ||
        root_child_index != 0u || !root_separator_changed ||
        tinydb_parent_stage_key_at(root_after, 0u) != kept_max) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t q0_max = tinydb_parent_stage_key_at(right_grand, 0u);
    uint32_t right_last_num = tinydb_parent_stage_child_at(q_after[1], 1u);
    if (right_last_num == 0u || right_last_num == INVALID_PAGE_NUM ||
        right_last_num >= pager->num_pages ||
        tinydb_internal_borrow_page_free(pager, right_last_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char right_last[PAGE_SIZE];
    memcpy(right_last, get_page(pager, right_last_num), PAGE_SIZE);
    uint32_t right_last_min = 0u, right_last_max = 0u;
    uint32_t right_last_prev = INVALID_PAGE_NUM, right_last_next = INVALID_PAGE_NUM;
    if (!tinydb_nonroot_merge_leaf_valid(
            right_last, PAGE_SIZE, q_num[1],
            &right_last_min, &right_last_max,
            &right_last_prev, &right_last_next) ||
        q0_max >= right_last_max || kept_max >= q0_max) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    (void)right_last_min;
    (void)right_last_prev;
    (void)right_last_next;

    const uint32_t merged_children[3] = {kept_num, q_num[0], q_num[1]};
    const uint32_t merged_maxes[3] = {kept_max, q0_max, right_last_max};
    if (!tinydb_nonroot_merge_build_three_child_internal(
            right_grand, root_num, merged_children, merged_maxes)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    void* direct_children[3] = {kept, q_after[0], q_after[1]};
    uint32_t promoted = INVALID_PAGE_NUM;
    if (!tinydb_stage_internal_root_collapse_to_internal(
            root_after, PAGE_SIZE, root_num,
            right_grand, PAGE_SIZE, right_grand_num, right_last_max,
            left_grand_num, kept_max,
            direct_children, merged_children, 3u, &promoted) ||
        promoted != right_grand_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    TinyDBV2PublishEntry entries[8];
    entries[0] = (TinyDBV2PublishEntry){
        root_num, (unsigned char*)get_page(pager, root_num), root_after};
    entries[1] = (TinyDBV2PublishEntry){
        kept_num, (unsigned char*)get_page(pager, kept_num), kept};
    entries[2] = (TinyDBV2PublishEntry){
        q_num[0], (unsigned char*)get_page(pager, q_num[0]), q_after[0]};
    entries[3] = (TinyDBV2PublishEntry){
        q_num[1], (unsigned char*)get_page(pager, q_num[1]), q_after[1]};
    for (uint32_t i = 0u; i < 3u; i++) {
        entries[4u + i] = (TinyDBV2PublishEntry){
            survivor_nums[i],
            (unsigned char*)get_page(pager, survivor_nums[i]),
            survivor_after[i]};
    }
    entries[7] = (TinyDBV2PublishEntry){
        next_leaf_num,
        (unsigned char*)get_page(pager, next_leaf_num),
        next_leaf_after};

    if (!tinydb_v2_publish_batch(entries, 8u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "inner-right recursive slotted V2 root-height publication failed");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }
    for (uint32_t i = 0u; i < 8u; i++) {
        mark_page_dirty(pager, entries[i].page_num);
    }

    pager_free_page(pager, removed_leaf_page_num);
    pager_free_page(pager, obsolete_num);
    pager_free_page(pager, left_grand_num);
    pager_free_page(pager, right_grand_num);
    if (!table->in_transaction) pager_commit(pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return TINYDB_INTERNAL_MERGE_ROOT_SUCCESS;
}

#endif /* TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_HEIGHT_INNER_RIGHT_H */
