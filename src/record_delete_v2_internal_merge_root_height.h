#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_HEIGHT_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_HEIGHT_H

#include "generic_index_epoch.h"
#include "internal_merge_root_height_cascade_stage.h"
#include "record_delete_v2_internal_merge_root.h"
#include "slotted_v2_publish_batch.h"

/*
 * Publish the bounded recursive DELETE case where both root grandchildren are
 * minimum and therefore neither can lend after a lower internal merge.
 *
 * The supported orientation removes the singleton leftmost leaf of the right
 * bottom parent under the left grandparent. The lower two minimum bottom
 * parents merge into the kept right bottom parent. Because the left
 * grandparent would then have one child and the right grandparent is also
 * minimum, both grandparents are eliminated and the stable root is contracted
 * one level to own [kept, right-left-parent, right-right-parent] directly.
 *
 * The staging primitive performs every structural rewrite before publication.
 * This live wrapper only discovers the exact bounded topology, persists the
 * generic-index mutation epoch, publishes seven staged pages atomically, then
 * reclaims removed leaf + obsolete bottom parent + both obsolete grandparents.
 */
static inline TinyDBInternalMergeRootResult
 tinydb_try_delete_v2_internal_merge_root_height(
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

    uint32_t kept_bottom_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (kept_bottom_num == 0u || kept_bottom_num == INVALID_PAGE_NUM ||
        kept_bottom_num >= table->pager->num_pages ||
        tinydb_internal_borrow_page_free(table->pager, kept_bottom_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char kept_after[PAGE_SIZE];
    memcpy(kept_after, get_page(table->pager, kept_bottom_num), PAGE_SIZE);
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
        left_grand_num >= table->pager->num_pages ||
        left_grand_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, left_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char left_grand_before[PAGE_SIZE];
    memcpy(left_grand_before,
           get_page(table->pager, left_grand_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(left_grand_before, PAGE_SIZE) ||
        left_grand_before[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            left_grand_before + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(left_grand_before, 1u) != kept_bottom_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t obsolete_bottom_num =
        tinydb_parent_stage_child_at(left_grand_before, 0u);
    if (obsolete_bottom_num == 0u ||
        obsolete_bottom_num == INVALID_PAGE_NUM ||
        obsolete_bottom_num >= table->pager->num_pages ||
        obsolete_bottom_num == kept_bottom_num ||
        tinydb_internal_borrow_page_free(table->pager, obsolete_bottom_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char obsolete_before[PAGE_SIZE];
    memcpy(obsolete_before,
           get_page(table->pager, obsolete_bottom_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(obsolete_before, PAGE_SIZE) ||
        obsolete_before[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(obsolete_before + PARENT_POINTER_OFFSET) !=
            left_grand_num ||
        tinydb_parent_stage_read_u32(
            obsolete_before + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t root_num = tinydb_internal_borrow_read_u32(
        left_grand_before + PARENT_POINTER_OFFSET);
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
        tinydb_parent_stage_child_at(root_after, 0u) != left_grand_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t right_grand_num = tinydb_parent_stage_child_at(root_after, 1u);
    if (right_grand_num == 0u || right_grand_num == INVALID_PAGE_NUM ||
        right_grand_num >= table->pager->num_pages ||
        right_grand_num == left_grand_num ||
        tinydb_internal_borrow_page_free(table->pager, right_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char right_grand_before[PAGE_SIZE];
    memcpy(right_grand_before,
           get_page(table->pager, right_grand_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(right_grand_before, PAGE_SIZE) ||
        right_grand_before[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(
            right_grand_before + PARENT_POINTER_OFFSET) != root_num ||
        tinydb_parent_stage_read_u32(
            right_grand_before + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t right_left_num =
        tinydb_parent_stage_child_at(right_grand_before, 0u);
    uint32_t right_right_num =
        tinydb_parent_stage_child_at(right_grand_before, 1u);
    if (right_left_num == 0u || right_left_num == INVALID_PAGE_NUM ||
        right_left_num >= table->pager->num_pages ||
        right_right_num == 0u || right_right_num == INVALID_PAGE_NUM ||
        right_right_num >= table->pager->num_pages ||
        right_left_num == right_right_num ||
        tinydb_internal_borrow_page_free(table->pager, right_left_num) ||
        tinydb_internal_borrow_page_free(table->pager, right_right_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    unsigned char right_left_after[PAGE_SIZE];
    unsigned char right_right_after[PAGE_SIZE];
    memcpy(right_left_after,
           get_page(table->pager, right_left_num),
           PAGE_SIZE);
    memcpy(right_right_after,
           get_page(table->pager, right_right_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(right_left_after, PAGE_SIZE) ||
        right_left_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(
            right_left_after + PARENT_POINTER_OFFSET) != right_grand_num ||
        tinydb_parent_stage_read_u32(
            right_left_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        !tinydb_parent_stage_validate(right_right_after, PAGE_SIZE) ||
        right_right_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(
            right_right_after + PARENT_POINTER_OFFSET) != right_grand_num ||
        tinydb_parent_stage_read_u32(
            right_right_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t survivor_nums[3] = {
        tinydb_parent_stage_child_at(obsolete_before, 0u),
        tinydb_parent_stage_child_at(obsolete_before, 1u),
        tinydb_parent_stage_child_at(kept_after, 1u)
    };
    uint32_t right_first_num =
        tinydb_parent_stage_child_at(right_left_after, 0u);
    uint32_t right_last_num =
        tinydb_parent_stage_child_at(right_right_after, 1u);

    unsigned char survivor_after[3][PAGE_SIZE];
    void* survivor_pages[3] = {
        survivor_after[0], survivor_after[1], survivor_after[2]
    };
    unsigned char right_first_before[PAGE_SIZE];
    unsigned char right_last_before[PAGE_SIZE];

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
    if (right_first_num == 0u || right_first_num == INVALID_PAGE_NUM ||
        right_first_num >= table->pager->num_pages ||
        right_last_num == 0u || right_last_num == INVALID_PAGE_NUM ||
        right_last_num >= table->pager->num_pages ||
        tinydb_internal_borrow_page_free(table->pager, right_first_num) ||
        tinydb_internal_borrow_page_free(table->pager, right_last_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    memcpy(right_first_before,
           get_page(table->pager, right_first_num),
           PAGE_SIZE);
    memcpy(right_last_before,
           get_page(table->pager, right_last_num),
           PAGE_SIZE);

    if (!tinydb_stage_internal_merge_root_height_cascade(
            root_after,
            PAGE_SIZE,
            root_num,
            left_grand_before,
            PAGE_SIZE,
            left_grand_num,
            right_grand_before,
            PAGE_SIZE,
            right_grand_num,
            obsolete_before,
            PAGE_SIZE,
            obsolete_bottom_num,
            kept_after,
            PAGE_SIZE,
            kept_bottom_num,
            right_left_after,
            PAGE_SIZE,
            right_left_num,
            right_right_after,
            PAGE_SIZE,
            right_right_num,
            removed_leaf_before,
            PAGE_SIZE,
            removed_leaf_page_num,
            removed_key,
            survivor_pages,
            survivor_nums,
            right_first_before,
            PAGE_SIZE,
            right_first_num,
            right_last_before,
            PAGE_SIZE,
            right_last_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    TinyDBV2PublishEntry entries[7];
    entries[0].page_num = root_num;
    entries[0].target = (unsigned char*)get_page(table->pager, root_num);
    entries[0].staged = root_after;
    entries[1].page_num = kept_bottom_num;
    entries[1].target = (unsigned char*)get_page(table->pager, kept_bottom_num);
    entries[1].staged = kept_after;
    entries[2].page_num = right_left_num;
    entries[2].target = (unsigned char*)get_page(table->pager, right_left_num);
    entries[2].staged = right_left_after;
    entries[3].page_num = right_right_num;
    entries[3].target = (unsigned char*)get_page(table->pager, right_right_num);
    entries[3].staged = right_right_after;
    for (uint32_t i = 0u; i < 3u; i++) {
        entries[4u + i].page_num = survivor_nums[i];
        entries[4u + i].target =
            (unsigned char*)get_page(table->pager, survivor_nums[i]);
        entries[4u + i].staged = survivor_after[i];
    }

    if (!tinydb_v2_publish_batch(entries, 7u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "recursive slotted V2 root-height publication failed");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }
    for (uint32_t i = 0u; i < 7u; i++) {
        mark_page_dirty(table->pager, entries[i].page_num);
    }

    pager_free_page(table->pager, removed_leaf_page_num);
    pager_free_page(table->pager, obsolete_bottom_num);
    pager_free_page(table->pager, left_grand_num);
    pager_free_page(table->pager, right_grand_num);
    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return TINYDB_INTERNAL_MERGE_ROOT_SUCCESS;
}

#endif /* TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_HEIGHT_H */
