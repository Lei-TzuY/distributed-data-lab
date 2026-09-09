#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_BORROW_ROOT_LEFT_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_BORROW_ROOT_LEFT_H

#include "generic_index_epoch.h"
#include "internal_merge_borrow_root_cascade_left_stage.h"
#include "record_delete_v2_internal_merge_root.h"
#include "slotted_v2_publish_batch.h"

/*
 * Publish the mirrored two-level V2 DELETE cascade.
 *
 * A two-child right grandparent would underflow after its two minimum bottom
 * parents merge. The rightmost minimum bottom-parent subtree of a >=3-child
 * left grandparent is therefore borrowed across the stable root boundary. The
 * root separator decreases to the left grandparent's new maximum. The shape is
 * deliberately bounded to leaf-parent grandchildren, matching the proven
 * staging contract; unsupported deeper shapes remain fail-closed.
 */
static inline TinyDBInternalMergeRootResult
 tinydb_try_delete_v2_internal_merge_borrow_root_from_left(
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

    uint32_t obsolete_bottom_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (obsolete_bottom_num == 0u ||
        obsolete_bottom_num == INVALID_PAGE_NUM ||
        obsolete_bottom_num >= table->pager->num_pages ||
        tinydb_internal_borrow_page_free(table->pager, obsolete_bottom_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char obsolete_before[PAGE_SIZE];
    memcpy(obsolete_before,
           get_page(table->pager, obsolete_bottom_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(obsolete_before, PAGE_SIZE) ||
        obsolete_before[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            obsolete_before + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(obsolete_before, 1u) !=
            removed_leaf_page_num ||
        tinydb_parent_stage_key_at(obsolete_before, 0u) >= removed_key) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t right_grand_num = tinydb_internal_borrow_read_u32(
        obsolete_before + PARENT_POINTER_OFFSET);
    if (right_grand_num == 0u || right_grand_num == INVALID_PAGE_NUM ||
        right_grand_num >= table->pager->num_pages ||
        right_grand_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, right_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char right_grand_after[PAGE_SIZE];
    memcpy(right_grand_after,
           get_page(table->pager, right_grand_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(right_grand_after, PAGE_SIZE) ||
        right_grand_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            right_grand_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(right_grand_after, 0u) !=
            obsolete_bottom_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t kept_bottom_num = tinydb_parent_stage_child_at(right_grand_after, 1u);
    if (kept_bottom_num == 0u || kept_bottom_num == INVALID_PAGE_NUM ||
        kept_bottom_num >= table->pager->num_pages ||
        kept_bottom_num == obsolete_bottom_num ||
        tinydb_internal_borrow_page_free(table->pager, kept_bottom_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char kept_after[PAGE_SIZE];
    memcpy(kept_after, get_page(table->pager, kept_bottom_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(kept_after, PAGE_SIZE) ||
        kept_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(kept_after + PARENT_POINTER_OFFSET) !=
            right_grand_num ||
        tinydb_parent_stage_read_u32(
            kept_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t root_num = tinydb_internal_borrow_read_u32(
        right_grand_after + PARENT_POINTER_OFFSET);
    if (root_num != schema->root_page_num || root_num >= table->pager->num_pages ||
        tinydb_internal_borrow_page_free(table->pager, root_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char root_after[PAGE_SIZE];
    memcpy(root_after, get_page(table->pager, root_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        root_after[IS_ROOT_OFFSET] == 0u ||
        tinydb_internal_borrow_read_u32(root_after + PARENT_POINTER_OFFSET) != 0u ||
        tinydb_parent_stage_read_u32(root_after + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        tinydb_parent_stage_child_at(root_after, 1u) != right_grand_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t left_grand_num = tinydb_parent_stage_child_at(root_after, 0u);
    if (left_grand_num == 0u || left_grand_num == INVALID_PAGE_NUM ||
        left_grand_num >= table->pager->num_pages ||
        left_grand_num == right_grand_num ||
        tinydb_internal_borrow_page_free(table->pager, left_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char left_grand_after[PAGE_SIZE];
    memcpy(left_grand_after,
           get_page(table->pager, left_grand_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(left_grand_after, PAGE_SIZE) ||
        left_grand_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(left_grand_after + PARENT_POINTER_OFFSET) !=
            root_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    uint32_t left_key_count = tinydb_parent_stage_read_u32(
        left_grand_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (left_key_count < 2u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t donor_parent_num =
        tinydb_parent_stage_child_at(left_grand_after, left_key_count);
    uint32_t previous_parent_num =
        tinydb_parent_stage_child_at(left_grand_after, left_key_count - 1u);
    if (donor_parent_num == 0u || donor_parent_num == INVALID_PAGE_NUM ||
        donor_parent_num >= table->pager->num_pages ||
        previous_parent_num == 0u || previous_parent_num == INVALID_PAGE_NUM ||
        previous_parent_num >= table->pager->num_pages ||
        donor_parent_num == previous_parent_num ||
        tinydb_internal_borrow_page_free(table->pager, donor_parent_num) ||
        tinydb_internal_borrow_page_free(table->pager, previous_parent_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char donor_after[PAGE_SIZE];
    unsigned char previous_parent[PAGE_SIZE];
    memcpy(donor_after, get_page(table->pager, donor_parent_num), PAGE_SIZE);
    memcpy(previous_parent,
           get_page(table->pager, previous_parent_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(donor_after, PAGE_SIZE) ||
        donor_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(donor_after + PARENT_POINTER_OFFSET) !=
            left_grand_num ||
        tinydb_parent_stage_read_u32(donor_after + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        !tinydb_parent_stage_validate(previous_parent, PAGE_SIZE) ||
        previous_parent[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(
            previous_parent + PARENT_POINTER_OFFSET) != left_grand_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t previous_parent_key_count = tinydb_parent_stage_read_u32(
        previous_parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    uint32_t previous_right_num = tinydb_parent_stage_child_at(
        previous_parent, previous_parent_key_count);
    if (previous_right_num == 0u || previous_right_num == INVALID_PAGE_NUM ||
        previous_right_num >= table->pager->num_pages ||
        tinydb_internal_borrow_page_free(table->pager, previous_right_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t survivor_nums[3] = {
        tinydb_parent_stage_child_at(obsolete_before, 0u),
        tinydb_parent_stage_child_at(kept_after, 0u),
        tinydb_parent_stage_child_at(kept_after, 1u)
    };
    uint32_t donor_leaf_nums[2] = {
        tinydb_parent_stage_child_at(donor_after, 0u),
        tinydb_parent_stage_child_at(donor_after, 1u)
    };

    unsigned char survivor_after[3][PAGE_SIZE];
    void* survivor_pages[3] = {
        survivor_after[0], survivor_after[1], survivor_after[2]
    };
    unsigned char donor_leaf_before[2][PAGE_SIZE];
    const void* donor_leaf_pages[2] = {
        donor_leaf_before[0], donor_leaf_before[1]
    };
    unsigned char previous_right_before[PAGE_SIZE];

    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_nums[i] == 0u || survivor_nums[i] == INVALID_PAGE_NUM ||
            survivor_nums[i] >= table->pager->num_pages ||
            tinydb_internal_borrow_page_free(table->pager, survivor_nums[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        memcpy(survivor_after[i],
               get_page(table->pager, survivor_nums[i]),
               PAGE_SIZE);
    }
    for (uint32_t i = 0u; i < 2u; i++) {
        if (donor_leaf_nums[i] == 0u || donor_leaf_nums[i] == INVALID_PAGE_NUM ||
            donor_leaf_nums[i] >= table->pager->num_pages ||
            tinydb_internal_borrow_page_free(table->pager, donor_leaf_nums[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        memcpy(donor_leaf_before[i],
               get_page(table->pager, donor_leaf_nums[i]),
               PAGE_SIZE);
    }
    memcpy(previous_right_before,
           get_page(table->pager, previous_right_num),
           PAGE_SIZE);

    if (!tinydb_stage_internal_merge_borrow_from_left_grandparent(
            root_after,
            PAGE_SIZE,
            root_num,
            left_grand_after,
            PAGE_SIZE,
            left_grand_num,
            right_grand_after,
            PAGE_SIZE,
            right_grand_num,
            donor_after,
            PAGE_SIZE,
            donor_parent_num,
            obsolete_before,
            PAGE_SIZE,
            obsolete_bottom_num,
            kept_after,
            PAGE_SIZE,
            kept_bottom_num,
            removed_leaf_before,
            PAGE_SIZE,
            removed_leaf_page_num,
            removed_key,
            survivor_pages,
            survivor_nums,
            donor_leaf_pages,
            donor_leaf_nums,
            previous_right_before,
            PAGE_SIZE,
            previous_right_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    TinyDBV2PublishEntry entries[8];
    entries[0].page_num = root_num;
    entries[0].target = (unsigned char*)get_page(table->pager, root_num);
    entries[0].staged = root_after;
    entries[1].page_num = left_grand_num;
    entries[1].target = (unsigned char*)get_page(table->pager, left_grand_num);
    entries[1].staged = left_grand_after;
    entries[2].page_num = right_grand_num;
    entries[2].target = (unsigned char*)get_page(table->pager, right_grand_num);
    entries[2].staged = right_grand_after;
    entries[3].page_num = donor_parent_num;
    entries[3].target = (unsigned char*)get_page(table->pager, donor_parent_num);
    entries[3].staged = donor_after;
    entries[4].page_num = kept_bottom_num;
    entries[4].target = (unsigned char*)get_page(table->pager, kept_bottom_num);
    entries[4].staged = kept_after;
    for (uint32_t i = 0u; i < 3u; i++) {
        entries[5u + i].page_num = survivor_nums[i];
        entries[5u + i].target =
            (unsigned char*)get_page(table->pager, survivor_nums[i]);
        entries[5u + i].staged = survivor_after[i];
    }

    if (!tinydb_v2_publish_batch(entries, 8u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "mirrored recursive slotted V2 merge-borrow publication failed");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }
    for (uint32_t i = 0u; i < 8u; i++) {
        mark_page_dirty(table->pager, entries[i].page_num);
    }

    pager_free_page(table->pager, removed_leaf_page_num);
    pager_free_page(table->pager, obsolete_bottom_num);
    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return TINYDB_INTERNAL_MERGE_ROOT_SUCCESS;
}

#endif /* TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_BORROW_ROOT_LEFT_H */
