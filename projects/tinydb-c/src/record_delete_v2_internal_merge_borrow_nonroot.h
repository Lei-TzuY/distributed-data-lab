#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_BORROW_NONROOT_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_BORROW_NONROOT_H

#include "generic_index_epoch.h"
#include "internal_merge_borrow_nonroot_window_stage.h"
#include "record_delete_v2_internal_merge_root.h"
#include "slotted_v2_publish_batch.h"

/*
 * Publish a height-five V2 DELETE merge->borrow inside a non-root ancestor.
 *
 * The target grandparent may now occupy any non-rightmost child position of a
 * wider great-parent. Its immediate right sibling grandparent is the donor.
 * Only the separator between that adjacent pair changes; siblings outside the
 * pair and the ancestor's own subtree maximum remain unchanged. The stable
 * root is ownership-validated only and never enters the publication batch.
 */
static inline TinyDBInternalMergeRootResult
 tinydb_try_delete_v2_internal_merge_borrow_nonroot(
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
    if (!tinydb_leaf_page_count(removed_leaf_before, PAGE_SIZE, &removed_count) ||
        removed_count != 1u ||
        !tinydb_leaf_page_key_at(removed_leaf_before, PAGE_SIZE, 0u,
                                 &removed_only_key) ||
        removed_only_key != removed_key) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t kept_bottom_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (kept_bottom_num == 0u || kept_bottom_num == INVALID_PAGE_NUM ||
        kept_bottom_num >= pager->num_pages ||
        tinydb_internal_borrow_page_free(pager, kept_bottom_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char kept_after[PAGE_SIZE];
    memcpy(kept_after, get_page(pager, kept_bottom_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(kept_after, PAGE_SIZE) ||
        kept_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            kept_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(kept_after, 0u) != removed_leaf_page_num ||
        tinydb_parent_stage_key_at(kept_after, 0u) != removed_key) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t left_grand_num = tinydb_internal_borrow_read_u32(
        kept_after + PARENT_POINTER_OFFSET);
    if (left_grand_num == 0u || left_grand_num == INVALID_PAGE_NUM ||
        left_grand_num >= pager->num_pages ||
        left_grand_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(pager, left_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char left_grand_after[PAGE_SIZE];
    memcpy(left_grand_after, get_page(pager, left_grand_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(left_grand_after, PAGE_SIZE) ||
        left_grand_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            left_grand_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(left_grand_after, 1u) != kept_bottom_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t obsolete_bottom_num =
        tinydb_parent_stage_child_at(left_grand_after, 0u);
    if (obsolete_bottom_num == 0u ||
        obsolete_bottom_num == INVALID_PAGE_NUM ||
        obsolete_bottom_num >= pager->num_pages ||
        obsolete_bottom_num == kept_bottom_num ||
        tinydb_internal_borrow_page_free(pager, obsolete_bottom_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char obsolete_before[PAGE_SIZE];
    memcpy(obsolete_before, get_page(pager, obsolete_bottom_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(obsolete_before, PAGE_SIZE) ||
        obsolete_before[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(
            obsolete_before + PARENT_POINTER_OFFSET) != left_grand_num ||
        tinydb_parent_stage_read_u32(
            obsolete_before + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t ancestor_num = tinydb_internal_borrow_read_u32(
        left_grand_after + PARENT_POINTER_OFFSET);
    if (ancestor_num == 0u || ancestor_num == INVALID_PAGE_NUM ||
        ancestor_num >= pager->num_pages ||
        ancestor_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(pager, ancestor_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char ancestor_after[PAGE_SIZE];
    memcpy(ancestor_after, get_page(pager, ancestor_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(ancestor_after, PAGE_SIZE) ||
        ancestor_after[IS_ROOT_OFFSET] != 0u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    uint32_t ancestor_keys = tinydb_parent_stage_read_u32(
        ancestor_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
    uint32_t pair_index = UINT32_MAX;
    for (uint32_t i = 0u; i < ancestor_keys; i++) {
        if (tinydb_parent_stage_child_at(ancestor_after, i) == left_grand_num) {
            if (pair_index != UINT32_MAX) {
                return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
            }
            pair_index = i;
        }
    }
    if (pair_index == UINT32_MAX) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t root_num = tinydb_internal_borrow_read_u32(
        ancestor_after + PARENT_POINTER_OFFSET);
    if (root_num != schema->root_page_num || root_num >= pager->num_pages ||
        tinydb_internal_borrow_page_free(pager, root_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char root_before[PAGE_SIZE];
    memcpy(root_before, get_page(pager, root_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root_before, PAGE_SIZE) ||
        root_before[IS_ROOT_OFFSET] == 0u ||
        tinydb_internal_borrow_read_u32(root_before + PARENT_POINTER_OFFSET) != 0u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    uint32_t root_keys = tinydb_parent_stage_read_u32(
        root_before + INTERNAL_NODE_NUM_KEYS_OFFSET);
    uint32_t ancestor_refs = 0u;
    for (uint32_t i = 0u; i <= root_keys; i++) {
        if (tinydb_parent_stage_child_at(root_before, i) == ancestor_num) {
            ancestor_refs++;
        }
    }
    if (ancestor_refs != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t right_grand_num =
        tinydb_parent_stage_child_at(ancestor_after, pair_index + 1u);
    if (right_grand_num == 0u || right_grand_num == INVALID_PAGE_NUM ||
        right_grand_num >= pager->num_pages ||
        right_grand_num == left_grand_num ||
        tinydb_internal_borrow_page_free(pager, right_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char right_grand_after[PAGE_SIZE];
    memcpy(right_grand_after, get_page(pager, right_grand_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(right_grand_after, PAGE_SIZE) ||
        right_grand_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(
            right_grand_after + PARENT_POINTER_OFFSET) != ancestor_num ||
        tinydb_parent_stage_read_u32(
            right_grand_after + INTERNAL_NODE_NUM_KEYS_OFFSET) < 2u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t donor_parent_num =
        tinydb_parent_stage_child_at(right_grand_after, 0u);
    uint32_t next_parent_num =
        tinydb_parent_stage_child_at(right_grand_after, 1u);
    if (donor_parent_num == 0u || donor_parent_num == INVALID_PAGE_NUM ||
        donor_parent_num >= pager->num_pages ||
        next_parent_num == 0u || next_parent_num == INVALID_PAGE_NUM ||
        next_parent_num >= pager->num_pages ||
        donor_parent_num == next_parent_num ||
        tinydb_internal_borrow_page_free(pager, donor_parent_num) ||
        tinydb_internal_borrow_page_free(pager, next_parent_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char donor_after[PAGE_SIZE];
    unsigned char next_parent[PAGE_SIZE];
    memcpy(donor_after, get_page(pager, donor_parent_num), PAGE_SIZE);
    memcpy(next_parent, get_page(pager, next_parent_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(donor_after, PAGE_SIZE) ||
        donor_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(donor_after + PARENT_POINTER_OFFSET) !=
            right_grand_num ||
        tinydb_parent_stage_read_u32(
            donor_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_parent_stage_validate(next_parent, PAGE_SIZE) ||
        next_parent[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(next_parent + PARENT_POINTER_OFFSET) !=
            right_grand_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t survivor_nums[3] = {
        tinydb_parent_stage_child_at(obsolete_before, 0u),
        tinydb_parent_stage_child_at(obsolete_before, 1u),
        tinydb_parent_stage_child_at(kept_after, 1u)
    };
    uint32_t donor_leaf_nums[2] = {
        tinydb_parent_stage_child_at(donor_after, 0u),
        tinydb_parent_stage_child_at(donor_after, 1u)
    };
    uint32_t next_left_num = tinydb_parent_stage_child_at(next_parent, 0u);

    unsigned char survivor_after[3][PAGE_SIZE];
    void* survivor_pages[3] = {
        survivor_after[0], survivor_after[1], survivor_after[2]
    };
    unsigned char donor_leaf_before[2][PAGE_SIZE];
    const void* donor_leaf_pages[2] = {
        donor_leaf_before[0], donor_leaf_before[1]
    };
    unsigned char next_left_before[PAGE_SIZE];
    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_nums[i] == 0u || survivor_nums[i] == INVALID_PAGE_NUM ||
            survivor_nums[i] >= pager->num_pages ||
            tinydb_internal_borrow_page_free(pager, survivor_nums[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        memcpy(survivor_after[i], get_page(pager, survivor_nums[i]), PAGE_SIZE);
    }
    for (uint32_t i = 0u; i < 2u; i++) {
        if (donor_leaf_nums[i] == 0u || donor_leaf_nums[i] == INVALID_PAGE_NUM ||
            donor_leaf_nums[i] >= pager->num_pages ||
            tinydb_internal_borrow_page_free(pager, donor_leaf_nums[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        memcpy(donor_leaf_before[i],
               get_page(pager, donor_leaf_nums[i]), PAGE_SIZE);
    }
    if (next_left_num == 0u || next_left_num == INVALID_PAGE_NUM ||
        next_left_num >= pager->num_pages ||
        tinydb_internal_borrow_page_free(pager, next_left_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    memcpy(next_left_before, get_page(pager, next_left_num), PAGE_SIZE);

    if (!tinydb_stage_internal_merge_borrow_window_from_right(
            ancestor_after, PAGE_SIZE, ancestor_num, root_num, pair_index,
            left_grand_after, PAGE_SIZE, left_grand_num,
            right_grand_after, PAGE_SIZE, right_grand_num,
            obsolete_before, PAGE_SIZE, obsolete_bottom_num,
            kept_after, PAGE_SIZE, kept_bottom_num,
            donor_after, PAGE_SIZE, donor_parent_num,
            removed_leaf_before, PAGE_SIZE, removed_leaf_page_num, removed_key,
            survivor_pages, survivor_nums,
            donor_leaf_pages, donor_leaf_nums,
            next_left_before, PAGE_SIZE, next_left_num)) {
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
        ancestor_num, (unsigned char*)get_page(pager, ancestor_num), ancestor_after};
    entries[1] = (TinyDBV2PublishEntry){
        left_grand_num, (unsigned char*)get_page(pager, left_grand_num),
        left_grand_after};
    entries[2] = (TinyDBV2PublishEntry){
        right_grand_num, (unsigned char*)get_page(pager, right_grand_num),
        right_grand_after};
    entries[3] = (TinyDBV2PublishEntry){
        kept_bottom_num, (unsigned char*)get_page(pager, kept_bottom_num), kept_after};
    entries[4] = (TinyDBV2PublishEntry){
        donor_parent_num, (unsigned char*)get_page(pager, donor_parent_num), donor_after};
    for (uint32_t i = 0u; i < 3u; i++) {
        entries[5u + i] = (TinyDBV2PublishEntry){
            survivor_nums[i], (unsigned char*)get_page(pager, survivor_nums[i]),
            survivor_after[i]};
    }
    if (!tinydb_v2_publish_batch(entries, 8u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "height-five recursive slotted V2 merge-borrow publication failed");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }
    for (uint32_t i = 0u; i < 8u; i++) {
        mark_page_dirty(pager, entries[i].page_num);
    }

    pager_free_page(pager, removed_leaf_page_num);
    pager_free_page(pager, obsolete_bottom_num);
    if (!table->in_transaction) pager_commit(pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return TINYDB_INTERNAL_MERGE_ROOT_SUCCESS;
}

#endif /* TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_BORROW_NONROOT_H */
