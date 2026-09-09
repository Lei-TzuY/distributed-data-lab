#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_BORROW_LEFT_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_BORROW_LEFT_H

#include "record_delete_v2_internal_borrow.h"

/*
 * Resolve the mirror image of tinydb_try_delete_v2_internal_borrow_from_right:
 * a two-child right internal parent loses its leftmost singleton V2 leaf and
 * borrows the rightmost leaf from a left internal sibling that still has at
 * least three children.
 *
 * Before DELETE:
 *   root (one key)
 *     left parent  -> [..., donor leaf]
 *     right parent -> [removed singleton V2 leaf, survivor leaf]
 *
 * The donor is the left subtree maximum, so removing it lowers the root
 * separator. The donor is reparented into the right parent, donor/survivor leaf
 * links are repaired across the removed page, and all five live page images
 * are published atomically before the removed leaf enters the Pager free list.
 * Other shapes remain fail-closed for later general merge/redistribution work.
 */
static inline TinyDBInternalBorrowResult
 tinydb_try_delete_v2_internal_borrow_from_left(
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
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t removed_count = 0u;
    uint32_t removed_only_key = 0u;
    uint32_t removed_prev = INVALID_PAGE_NUM;
    uint32_t removed_next = INVALID_PAGE_NUM;
    if (!tinydb_leaf_page_count(removed_leaf_before, PAGE_SIZE, &removed_count) ||
        removed_count != 1u ||
        !tinydb_leaf_page_key_at(removed_leaf_before, PAGE_SIZE, 0u,
                                 &removed_only_key) ||
        removed_only_key != removed_key ||
        !tinydb_leaf_page_prev(removed_leaf_before, PAGE_SIZE, &removed_prev) ||
        !tinydb_leaf_page_next(removed_leaf_before, PAGE_SIZE, &removed_next) ||
        removed_prev == 0u || removed_next == 0u ||
        removed_prev == INVALID_PAGE_NUM || removed_next == INVALID_PAGE_NUM ||
        removed_prev >= table->pager->num_pages ||
        removed_next >= table->pager->num_pages) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t right_parent_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (right_parent_num == 0u || right_parent_num == INVALID_PAGE_NUM ||
        right_parent_num >= table->pager->num_pages ||
        right_parent_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, right_parent_num)) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    unsigned char right_before[PAGE_SIZE];
    memcpy(right_before, get_page(table->pager, right_parent_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(right_before, PAGE_SIZE) ||
        right_before[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(right_before + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        tinydb_parent_stage_child_at(right_before, 0u) != removed_leaf_page_num ||
        tinydb_parent_stage_key_at(right_before, 0u) != removed_key) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t root_num = tinydb_internal_borrow_read_u32(
        right_before + PARENT_POINTER_OFFSET);
    if (root_num != schema->root_page_num || root_num >= table->pager->num_pages ||
        tinydb_internal_borrow_page_free(table->pager, root_num)) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    unsigned char root_after[PAGE_SIZE];
    memcpy(root_after, get_page(table->pager, root_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        root_after[IS_ROOT_OFFSET] == 0u ||
        tinydb_internal_borrow_read_u32(root_after + PARENT_POINTER_OFFSET) != 0u ||
        tinydb_parent_stage_read_u32(root_after + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        tinydb_parent_stage_child_at(root_after, 1u) != right_parent_num) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t left_parent_num = tinydb_parent_stage_child_at(root_after, 0u);
    uint32_t root_separator = tinydb_parent_stage_key_at(root_after, 0u);
    if (left_parent_num == 0u || left_parent_num == INVALID_PAGE_NUM ||
        left_parent_num >= table->pager->num_pages ||
        left_parent_num == right_parent_num ||
        tinydb_internal_borrow_page_free(table->pager, left_parent_num)) {
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    unsigned char left_after[PAGE_SIZE];
    memcpy(left_after, get_page(table->pager, left_parent_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(left_after, PAGE_SIZE) ||
        left_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(left_after + PARENT_POINTER_OFFSET) !=
            root_num) {
        tinydb_internal_borrow_message(
            message, message_size,
            "slotted V2 internal redistribution found an invalid left sibling");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }
    uint32_t left_key_count = tinydb_parent_stage_read_u32(
        left_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (left_key_count < 2u) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t donor_num = tinydb_parent_stage_child_at(left_after, left_key_count);
    uint32_t survivor_num = tinydb_parent_stage_child_at(right_before, 1u);
    if (donor_num == 0u || survivor_num == 0u ||
        donor_num == INVALID_PAGE_NUM || survivor_num == INVALID_PAGE_NUM ||
        donor_num >= table->pager->num_pages ||
        survivor_num >= table->pager->num_pages || donor_num == survivor_num ||
        donor_num != removed_prev || survivor_num != removed_next ||
        tinydb_internal_borrow_page_free(table->pager, donor_num) ||
        tinydb_internal_borrow_page_free(table->pager, survivor_num)) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    unsigned char donor_after[PAGE_SIZE];
    unsigned char survivor_after[PAGE_SIZE];
    memcpy(donor_after, get_page(table->pager, donor_num), PAGE_SIZE);
    memcpy(survivor_after, get_page(table->pager, survivor_num), PAGE_SIZE);
    uint32_t donor_min = 0u, donor_max = 0u;
    uint32_t survivor_min = 0u, survivor_max = 0u;
    uint32_t donor_next = INVALID_PAGE_NUM;
    uint32_t survivor_prev = INVALID_PAGE_NUM;
    if (!tinydb_internal_borrow_leaf_bounds(donor_after,
                                            &donor_min,
                                            &donor_max) ||
        !tinydb_internal_borrow_leaf_bounds(survivor_after,
                                            &survivor_min,
                                            &survivor_max) ||
        donor_max != root_separator || donor_max >= removed_key ||
        removed_key >= survivor_min ||
        !tinydb_leaf_page_next(donor_after, PAGE_SIZE, &donor_next) ||
        donor_next != removed_leaf_page_num ||
        !tinydb_leaf_page_prev(survivor_after, PAGE_SIZE, &survivor_prev) ||
        survivor_prev != removed_leaf_page_num ||
        !tinydb_stage_leaf_sibling_relink(donor_after,
                                          PAGE_SIZE,
                                          true,
                                          removed_leaf_page_num,
                                          survivor_num) ||
        !tinydb_stage_leaf_sibling_relink(survivor_after,
                                          PAGE_SIZE,
                                          false,
                                          removed_leaf_page_num,
                                          donor_num)) {
        tinydb_internal_borrow_message(
            message, message_size,
            "slotted V2 internal redistribution found inconsistent leaf boundaries");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    tinydb_internal_borrow_write_u32(donor_after + PARENT_POINTER_OFFSET,
                                     right_parent_num);

    unsigned char right_after[PAGE_SIZE];
    memcpy(right_after, right_before, PAGE_SIZE);
    if (!tinydb_internal_borrow_build_two_child_parent(right_after,
                                                       root_num,
                                                       donor_num,
                                                       donor_max,
                                                       survivor_num)) {
        tinydb_internal_borrow_message(
            message, message_size,
            "slotted V2 internal redistribution could not rebuild the underflow parent");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    uint32_t donated_index = UINT32_MAX;
    bool left_max_changed = false;
    uint32_t left_new_max = 0u;
    if (!tinydb_stage_internal_child_remove(left_after,
                                             PAGE_SIZE,
                                             donor_num,
                                             donor_max,
                                             &donated_index,
                                             &left_max_changed,
                                             &left_new_max) ||
        donated_index != left_key_count || !left_max_changed ||
        left_new_max == 0u || left_new_max >= donor_max) {
        tinydb_internal_borrow_message(
            message, message_size,
            "slotted V2 internal redistribution could not remove the donated child");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    tinydb_parent_stage_write_u32(
        root_after + INTERNAL_NODE_HEADER_SIZE + INTERNAL_NODE_CHILD_SIZE,
        left_new_max);
    if (!tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        tinydb_parent_stage_child_at(root_after, 0u) != left_parent_num ||
        tinydb_parent_stage_child_at(root_after, 1u) != right_parent_num ||
        tinydb_parent_stage_key_at(root_after, 0u) != left_new_max) {
        tinydb_internal_borrow_message(
            message, message_size,
            "slotted V2 internal redistribution produced an invalid root separator");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_internal_borrow_message(
            message, message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    TinyDBV2PublishEntry entries[5];
    entries[0].page_num = left_parent_num;
    entries[0].target = (unsigned char*)get_page(table->pager, left_parent_num);
    entries[0].staged = left_after;
    entries[1].page_num = right_parent_num;
    entries[1].target = (unsigned char*)get_page(table->pager, right_parent_num);
    entries[1].staged = right_after;
    entries[2].page_num = root_num;
    entries[2].target = (unsigned char*)get_page(table->pager, root_num);
    entries[2].staged = root_after;
    entries[3].page_num = donor_num;
    entries[3].target = (unsigned char*)get_page(table->pager, donor_num);
    entries[3].staged = donor_after;
    entries[4].page_num = survivor_num;
    entries[4].target = (unsigned char*)get_page(table->pager, survivor_num);
    entries[4].staged = survivor_after;
    if (!tinydb_v2_publish_batch(entries, 5u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_internal_borrow_message(
            message, message_size,
            "unable to atomically publish slotted V2 internal redistribution");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }
    for (uint32_t i = 0u; i < 5u; i++) {
        mark_page_dirty(table->pager, entries[i].page_num);
    }
    pager_free_page(table->pager, removed_leaf_page_num);
    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return TINYDB_INTERNAL_BORROW_SUCCESS;
}

#endif
