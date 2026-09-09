#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_BORROW_LOCAL_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_BORROW_LOCAL_H

#include "record_delete_v2_internal_borrow.h"

/*
 * Extend bounded leaf-child redistribution one level below the root.
 *
 * The target is a two-child internal parent whose direct parent is another
 * non-root internal node (the grandparent of the removed leaf). Borrowing is
 * allowed only across the boundary containing the removed singleton V2 leaf:
 *
 *   target [A, removed] | right sibling [donor, ...]
 *   left sibling [..., donor] | target [removed, D]
 *
 * In both orientations the grandparent keeps the same child count and subtree
 * maximum. Only one non-rightmost grandparent separator changes, so no ancestor
 * propagation is required. Parent/sibling images, grandparent separator,
 * donor parent pointer, and the two affected leaf links are staged before one
 * five-page publication. Other deeper underflow shapes remain fail-closed.
 */
static inline TinyDBInternalBorrowResult
 tinydb_try_delete_v2_internal_borrow_local(
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
        !tinydb_leaf_page_key_at(removed_leaf_before,
                                 PAGE_SIZE,
                                 0u,
                                 &removed_only_key) ||
        removed_only_key != removed_key ||
        !tinydb_leaf_page_prev(removed_leaf_before, PAGE_SIZE, &removed_prev) ||
        !tinydb_leaf_page_next(removed_leaf_before, PAGE_SIZE, &removed_next)) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t target_parent_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (target_parent_num == 0u || target_parent_num == INVALID_PAGE_NUM ||
        target_parent_num >= table->pager->num_pages ||
        target_parent_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, target_parent_num)) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    unsigned char target_before[PAGE_SIZE];
    memcpy(target_before, get_page(table->pager, target_parent_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(target_before, PAGE_SIZE) ||
        target_before[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(target_before + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t grandparent_num = tinydb_internal_borrow_read_u32(
        target_before + PARENT_POINTER_OFFSET);
    if (grandparent_num == 0u || grandparent_num == INVALID_PAGE_NUM ||
        grandparent_num >= table->pager->num_pages ||
        grandparent_num == target_parent_num ||
        grandparent_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, grandparent_num)) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    unsigned char grandparent_after[PAGE_SIZE];
    memcpy(grandparent_after,
           get_page(table->pager, grandparent_num),
           PAGE_SIZE);
    if (!tinydb_parent_stage_validate(grandparent_after, PAGE_SIZE) ||
        grandparent_after[IS_ROOT_OFFSET] != 0u) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }
    uint32_t grandparent_key_count = tinydb_parent_stage_read_u32(
        grandparent_after + INTERNAL_NODE_NUM_KEYS_OFFSET);

    uint32_t target_index = grandparent_key_count + 1u;
    for (uint32_t i = 0u; i <= grandparent_key_count; i++) {
        if (tinydb_parent_stage_child_at(grandparent_after, i) ==
            target_parent_num) {
            if (target_index != grandparent_key_count + 1u) {
                return TINYDB_INTERNAL_BORROW_FAILURE;
            }
            target_index = i;
        }
    }
    if (target_index == grandparent_key_count + 1u) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t target_left = tinydb_parent_stage_child_at(target_before, 0u);
    uint32_t target_right = tinydb_parent_stage_child_at(target_before, 1u);
    bool removed_rightmost = target_right == removed_leaf_page_num;
    bool removed_leftmost = target_left == removed_leaf_page_num;
    if (removed_rightmost == removed_leftmost) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    if (removed_rightmost) {
        if (target_index >= grandparent_key_count ||
            removed_prev != target_left ||
            tinydb_parent_stage_key_at(grandparent_after, target_index) !=
                removed_key) {
            return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
        }
        uint32_t sibling_num = tinydb_parent_stage_child_at(
            grandparent_after,
            target_index + 1u);
        if (sibling_num == 0u || sibling_num == INVALID_PAGE_NUM ||
            sibling_num >= table->pager->num_pages ||
            sibling_num == target_parent_num ||
            tinydb_internal_borrow_page_free(table->pager, sibling_num)) {
            return TINYDB_INTERNAL_BORROW_FAILURE;
        }

        unsigned char sibling_after[PAGE_SIZE];
        memcpy(sibling_after, get_page(table->pager, sibling_num), PAGE_SIZE);
        if (!tinydb_parent_stage_validate(sibling_after, PAGE_SIZE) ||
            sibling_after[IS_ROOT_OFFSET] != 0u ||
            tinydb_internal_borrow_read_u32(
                sibling_after + PARENT_POINTER_OFFSET) != grandparent_num) {
            return TINYDB_INTERNAL_BORROW_FAILURE;
        }
        uint32_t sibling_key_count = tinydb_parent_stage_read_u32(
            sibling_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
        if (sibling_key_count < 2u) {
            return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
        }

        uint32_t donor_num = tinydb_parent_stage_child_at(sibling_after, 0u);
        uint32_t donor_max = tinydb_parent_stage_key_at(sibling_after, 0u);
        if (donor_num == 0u || donor_num == INVALID_PAGE_NUM ||
            donor_num >= table->pager->num_pages || donor_num != removed_next ||
            tinydb_internal_borrow_page_free(table->pager, donor_num)) {
            return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
        }

        unsigned char survivor_after[PAGE_SIZE];
        unsigned char donor_after[PAGE_SIZE];
        memcpy(survivor_after, get_page(table->pager, target_left), PAGE_SIZE);
        memcpy(donor_after, get_page(table->pager, donor_num), PAGE_SIZE);
        uint32_t survivor_min = 0u, survivor_max = 0u;
        uint32_t donor_min = 0u, donor_checked_max = 0u;
        uint32_t survivor_next = INVALID_PAGE_NUM;
        uint32_t donor_prev = INVALID_PAGE_NUM;
        if (!tinydb_internal_borrow_leaf_bounds(survivor_after,
                                                &survivor_min,
                                                &survivor_max) ||
            !tinydb_internal_borrow_leaf_bounds(donor_after,
                                                &donor_min,
                                                &donor_checked_max) ||
            survivor_max != tinydb_parent_stage_key_at(target_before, 0u) ||
            donor_checked_max != donor_max || survivor_max >= removed_key ||
            removed_key >= donor_min ||
            !tinydb_leaf_page_next(survivor_after, PAGE_SIZE, &survivor_next) ||
            survivor_next != removed_leaf_page_num ||
            !tinydb_leaf_page_prev(donor_after, PAGE_SIZE, &donor_prev) ||
            donor_prev != removed_leaf_page_num ||
            !tinydb_stage_leaf_sibling_relink(survivor_after,
                                              PAGE_SIZE,
                                              true,
                                              removed_leaf_page_num,
                                              donor_num) ||
            !tinydb_stage_leaf_sibling_relink(donor_after,
                                              PAGE_SIZE,
                                              false,
                                              removed_leaf_page_num,
                                              target_left)) {
            return TINYDB_INTERNAL_BORROW_FAILURE;
        }

        tinydb_internal_borrow_write_u32(donor_after + PARENT_POINTER_OFFSET,
                                         target_parent_num);
        unsigned char target_after[PAGE_SIZE];
        memcpy(target_after, target_before, PAGE_SIZE);
        if (!tinydb_internal_borrow_build_two_child_parent(target_after,
                                                           grandparent_num,
                                                           target_left,
                                                           survivor_max,
                                                           donor_num)) {
            return TINYDB_INTERNAL_BORROW_FAILURE;
        }

        uint32_t donated_index = UINT32_MAX;
        bool sibling_max_changed = false;
        uint32_t sibling_new_max = 0u;
        if (!tinydb_stage_internal_child_remove(sibling_after,
                                                 PAGE_SIZE,
                                                 donor_num,
                                                 donor_max,
                                                 &donated_index,
                                                 &sibling_max_changed,
                                                 &sibling_new_max) ||
            donated_index != 0u || sibling_max_changed) {
            return TINYDB_INTERNAL_BORROW_FAILURE;
        }

        tinydb_parent_stage_write_u32(
            tinydb_parent_stage_cell(grandparent_after, target_index) +
                INTERNAL_NODE_CHILD_SIZE,
            donor_max);
        if (!tinydb_parent_stage_validate(grandparent_after, PAGE_SIZE) ||
            tinydb_parent_stage_child_at(grandparent_after, target_index) !=
                target_parent_num ||
            tinydb_parent_stage_child_at(grandparent_after,
                                         target_index + 1u) != sibling_num ||
            tinydb_parent_stage_key_at(grandparent_after, target_index) !=
                donor_max) {
            return TINYDB_INTERNAL_BORROW_FAILURE;
        }

        if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
            tinydb_internal_borrow_message(
                message, message_size,
                "unable to persist generic-index mutation epoch");
            return TINYDB_INTERNAL_BORROW_FAILURE;
        }
        TinyDBV2PublishEntry entries[5];
        entries[0].page_num = target_parent_num;
        entries[0].target = (unsigned char*)get_page(table->pager,
                                                     target_parent_num);
        entries[0].staged = target_after;
        entries[1].page_num = sibling_num;
        entries[1].target = (unsigned char*)get_page(table->pager, sibling_num);
        entries[1].staged = sibling_after;
        entries[2].page_num = grandparent_num;
        entries[2].target =
            (unsigned char*)get_page(table->pager, grandparent_num);
        entries[2].staged = grandparent_after;
        entries[3].page_num = target_left;
        entries[3].target = (unsigned char*)get_page(table->pager, target_left);
        entries[3].staged = survivor_after;
        entries[4].page_num = donor_num;
        entries[4].target = (unsigned char*)get_page(table->pager, donor_num);
        entries[4].staged = donor_after;
        if (!tinydb_v2_publish_batch(entries, 5u, TINYDB_V2_PUBLISH_NO_FAIL)) {
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

    if (target_index == 0u ||
        removed_next != target_right ||
        tinydb_parent_stage_key_at(target_before, 0u) != removed_key) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }
    uint32_t sibling_num = tinydb_parent_stage_child_at(grandparent_after,
                                                         target_index - 1u);
    if (sibling_num == 0u || sibling_num == INVALID_PAGE_NUM ||
        sibling_num >= table->pager->num_pages ||
        sibling_num == target_parent_num ||
        tinydb_internal_borrow_page_free(table->pager, sibling_num)) {
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    unsigned char sibling_after[PAGE_SIZE];
    memcpy(sibling_after, get_page(table->pager, sibling_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(sibling_after, PAGE_SIZE) ||
        sibling_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(sibling_after + PARENT_POINTER_OFFSET) !=
            grandparent_num) {
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }
    uint32_t sibling_key_count = tinydb_parent_stage_read_u32(
        sibling_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (sibling_key_count < 2u) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t donor_num = tinydb_parent_stage_child_at(sibling_after,
                                                       sibling_key_count);
    if (donor_num == 0u || donor_num == INVALID_PAGE_NUM ||
        donor_num >= table->pager->num_pages || donor_num != removed_prev ||
        tinydb_internal_borrow_page_free(table->pager, donor_num)) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    unsigned char donor_after[PAGE_SIZE];
    unsigned char survivor_after[PAGE_SIZE];
    memcpy(donor_after, get_page(table->pager, donor_num), PAGE_SIZE);
    memcpy(survivor_after, get_page(table->pager, target_right), PAGE_SIZE);
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
        tinydb_parent_stage_key_at(grandparent_after, target_index - 1u) !=
            donor_max ||
        donor_max >= removed_key || removed_key >= survivor_min ||
        !tinydb_leaf_page_next(donor_after, PAGE_SIZE, &donor_next) ||
        donor_next != removed_leaf_page_num ||
        !tinydb_leaf_page_prev(survivor_after, PAGE_SIZE, &survivor_prev) ||
        survivor_prev != removed_leaf_page_num ||
        !tinydb_stage_leaf_sibling_relink(donor_after,
                                          PAGE_SIZE,
                                          true,
                                          removed_leaf_page_num,
                                          target_right) ||
        !tinydb_stage_leaf_sibling_relink(survivor_after,
                                          PAGE_SIZE,
                                          false,
                                          removed_leaf_page_num,
                                          donor_num)) {
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    tinydb_internal_borrow_write_u32(donor_after + PARENT_POINTER_OFFSET,
                                     target_parent_num);
    unsigned char target_after[PAGE_SIZE];
    memcpy(target_after, target_before, PAGE_SIZE);
    if (!tinydb_internal_borrow_build_two_child_parent(target_after,
                                                       grandparent_num,
                                                       donor_num,
                                                       donor_max,
                                                       target_right)) {
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    uint32_t donated_index = UINT32_MAX;
    bool sibling_max_changed = false;
    uint32_t sibling_new_max = 0u;
    if (!tinydb_stage_internal_child_remove(sibling_after,
                                             PAGE_SIZE,
                                             donor_num,
                                             donor_max,
                                             &donated_index,
                                             &sibling_max_changed,
                                             &sibling_new_max) ||
        donated_index != sibling_key_count || !sibling_max_changed ||
        sibling_new_max == 0u || sibling_new_max >= donor_max) {
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    tinydb_parent_stage_write_u32(
        tinydb_parent_stage_cell(grandparent_after, target_index - 1u) +
            INTERNAL_NODE_CHILD_SIZE,
        sibling_new_max);
    if (!tinydb_parent_stage_validate(grandparent_after, PAGE_SIZE) ||
        tinydb_parent_stage_child_at(grandparent_after, target_index - 1u) !=
            sibling_num ||
        tinydb_parent_stage_child_at(grandparent_after, target_index) !=
            target_parent_num ||
        tinydb_parent_stage_key_at(grandparent_after, target_index - 1u) !=
            sibling_new_max) {
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_internal_borrow_message(
            message, message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }
    TinyDBV2PublishEntry entries[5];
    entries[0].page_num = sibling_num;
    entries[0].target = (unsigned char*)get_page(table->pager, sibling_num);
    entries[0].staged = sibling_after;
    entries[1].page_num = target_parent_num;
    entries[1].target = (unsigned char*)get_page(table->pager, target_parent_num);
    entries[1].staged = target_after;
    entries[2].page_num = grandparent_num;
    entries[2].target = (unsigned char*)get_page(table->pager, grandparent_num);
    entries[2].staged = grandparent_after;
    entries[3].page_num = donor_num;
    entries[3].target = (unsigned char*)get_page(table->pager, donor_num);
    entries[3].staged = donor_after;
    entries[4].page_num = target_right;
    entries[4].target = (unsigned char*)get_page(table->pager, target_right);
    entries[4].staged = survivor_after;
    if (!tinydb_v2_publish_batch(entries, 5u, TINYDB_V2_PUBLISH_NO_FAIL)) {
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
