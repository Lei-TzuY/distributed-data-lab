#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_BORROW_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_BORROW_H

#include "generic_index_epoch.h"
#include "internal_child_remove_stage.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "leaf_sibling_relink_stage.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_v2_publish_batch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    TINYDB_INTERNAL_BORROW_NOT_APPLICABLE = 0,
    TINYDB_INTERNAL_BORROW_SUCCESS,
    TINYDB_INTERNAL_BORROW_FAILURE
} TinyDBInternalBorrowResult;

static inline uint32_t tinydb_internal_borrow_read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static inline void tinydb_internal_borrow_write_u32(unsigned char* p,
                                                     uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

static inline void tinydb_internal_borrow_message(char* message,
                                                   size_t message_size,
                                                   const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static inline bool tinydb_internal_borrow_page_free(const Pager* pager,
                                                     uint32_t page_num) {
    if (pager == NULL) return true;
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static inline bool tinydb_internal_borrow_leaf_bounds(
    const unsigned char page[PAGE_SIZE],
    uint32_t* min_key,
    uint32_t* max_key) {
    uint32_t count = 0u;
    if (page == NULL || min_key == NULL || max_key == NULL ||
        page[NODE_TYPE_OFFSET] != (unsigned char)NODE_LEAF ||
        !tinydb_leaf_page_count(page, PAGE_SIZE, &count) || count == 0u ||
        !tinydb_leaf_page_key_at(page, PAGE_SIZE, 0u, min_key) ||
        !tinydb_leaf_page_key_at(page, PAGE_SIZE, count - 1u, max_key)) {
        return false;
    }
    return *min_key <= *max_key;
}

static inline bool tinydb_internal_borrow_build_two_child_parent(
    unsigned char page[PAGE_SIZE],
    uint32_t parent_page_num,
    uint32_t left_child,
    uint32_t left_max,
    uint32_t right_child) {
    if (page == NULL || parent_page_num == INVALID_PAGE_NUM ||
        left_child == 0u || right_child == 0u ||
        left_child == INVALID_PAGE_NUM || right_child == INVALID_PAGE_NUM ||
        left_child == right_child) {
        return false;
    }
    memset(page, 0, PAGE_USABLE_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_INTERNAL;
    page[IS_ROOT_OFFSET] = 0u;
    tinydb_internal_borrow_write_u32(page + PARENT_POINTER_OFFSET,
                                     parent_page_num);
    tinydb_internal_borrow_write_u32(page + INTERNAL_NODE_NUM_KEYS_OFFSET, 1u);
    unsigned char* cell = page + INTERNAL_NODE_HEADER_SIZE;
    tinydb_internal_borrow_write_u32(cell, left_child);
    tinydb_internal_borrow_write_u32(cell + INTERNAL_NODE_CHILD_SIZE,
                                     left_max);
    tinydb_internal_borrow_write_u32(page + INTERNAL_NODE_RIGHT_CHILD_OFFSET,
                                     right_child);
    return tinydb_parent_stage_validate(page, PAGE_SIZE);
}

/*
 * Resolve one deliberately bounded non-root underflow by borrowing the
 * leftmost leaf from the right internal sibling.
 *
 * Shape before DELETE:
 *   root (one key)
 *     left parent  -> [survivor leaf, removed singleton V2 leaf]
 *     right parent -> [donor leaf, ... at least two more leaf children]
 *
 * The removed leaf must be the leaf-list boundary immediately between the
 * survivor and donor. The operation relinks survivor -> donor, reparents the
 * donor into the left parent, removes the donor from the right parent, and
 * raises the root separator to the donor maximum. Both internal parents retain
 * at least two children. All five live page images are validated before one
 * Pager-visible batch publication; allocator reclamation happens only after
 * publication succeeds.
 *
 * Other underflow shapes remain fail-closed and are left for merge/general
 * redistribution support.
 */
static inline TinyDBInternalBorrowResult
 tinydb_try_delete_v2_internal_borrow_from_right(
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

    uint32_t left_parent_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (left_parent_num == 0u || left_parent_num == INVALID_PAGE_NUM ||
        left_parent_num >= table->pager->num_pages ||
        left_parent_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(table->pager, left_parent_num)) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    unsigned char left_before[PAGE_SIZE];
    memcpy(left_before, get_page(table->pager, left_parent_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(left_before, PAGE_SIZE) ||
        left_before[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(left_before + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            1u ||
        tinydb_parent_stage_child_at(left_before, 1u) != removed_leaf_page_num) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t root_num = tinydb_internal_borrow_read_u32(
        left_before + PARENT_POINTER_OFFSET);
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
        tinydb_parent_stage_child_at(root_after, 0u) != left_parent_num ||
        tinydb_parent_stage_key_at(root_after, 0u) != removed_key) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t right_parent_num = tinydb_parent_stage_child_at(root_after, 1u);
    if (right_parent_num == 0u || right_parent_num == INVALID_PAGE_NUM ||
        right_parent_num >= table->pager->num_pages ||
        right_parent_num == left_parent_num ||
        tinydb_internal_borrow_page_free(table->pager, right_parent_num)) {
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    unsigned char right_after[PAGE_SIZE];
    memcpy(right_after, get_page(table->pager, right_parent_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(right_after, PAGE_SIZE) ||
        right_after[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(right_after + PARENT_POINTER_OFFSET) !=
            root_num) {
        tinydb_internal_borrow_message(
            message, message_size,
            "slotted V2 internal redistribution found an invalid right sibling");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }
    uint32_t right_key_count = tinydb_parent_stage_read_u32(
        right_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (right_key_count < 2u) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    uint32_t survivor_num = tinydb_parent_stage_child_at(left_before, 0u);
    uint32_t survivor_separator = tinydb_parent_stage_key_at(left_before, 0u);
    uint32_t donor_num = tinydb_parent_stage_child_at(right_after, 0u);
    uint32_t donor_max = tinydb_parent_stage_key_at(right_after, 0u);
    if (survivor_num == 0u || donor_num == 0u ||
        survivor_num == INVALID_PAGE_NUM || donor_num == INVALID_PAGE_NUM ||
        survivor_num >= table->pager->num_pages ||
        donor_num >= table->pager->num_pages || survivor_num == donor_num ||
        donor_num != removed_next || survivor_num != removed_prev ||
        tinydb_internal_borrow_page_free(table->pager, survivor_num) ||
        tinydb_internal_borrow_page_free(table->pager, donor_num)) {
        return TINYDB_INTERNAL_BORROW_NOT_APPLICABLE;
    }

    unsigned char survivor_after[PAGE_SIZE];
    unsigned char donor_after[PAGE_SIZE];
    memcpy(survivor_after, get_page(table->pager, survivor_num), PAGE_SIZE);
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
        survivor_max != survivor_separator ||
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
                                          survivor_num)) {
        tinydb_internal_borrow_message(
            message, message_size,
            "slotted V2 internal redistribution found inconsistent leaf boundaries");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    tinydb_internal_borrow_write_u32(donor_after + PARENT_POINTER_OFFSET,
                                     left_parent_num);

    unsigned char left_after[PAGE_SIZE];
    memcpy(left_after, left_before, PAGE_SIZE);
    if (!tinydb_internal_borrow_build_two_child_parent(left_after,
                                                       root_num,
                                                       survivor_num,
                                                       survivor_max,
                                                       donor_num)) {
        tinydb_internal_borrow_message(
            message, message_size,
            "slotted V2 internal redistribution could not rebuild the underflow parent");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    uint32_t donated_index = UINT32_MAX;
    bool right_max_changed = false;
    uint32_t right_new_max = 0u;
    if (!tinydb_stage_internal_child_remove(right_after,
                                             PAGE_SIZE,
                                             donor_num,
                                             donor_max,
                                             &donated_index,
                                             &right_max_changed,
                                             &right_new_max) ||
        donated_index != 0u || right_max_changed) {
        tinydb_internal_borrow_message(
            message, message_size,
            "slotted V2 internal redistribution could not remove the donated child");
        return TINYDB_INTERNAL_BORROW_FAILURE;
    }

    tinydb_parent_stage_write_u32(
        root_after + INTERNAL_NODE_HEADER_SIZE + INTERNAL_NODE_CHILD_SIZE,
        donor_max);
    if (!tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        tinydb_parent_stage_child_at(root_after, 0u) != left_parent_num ||
        tinydb_parent_stage_child_at(root_after, 1u) != right_parent_num ||
        tinydb_parent_stage_key_at(root_after, 0u) != donor_max) {
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
    entries[3].page_num = survivor_num;
    entries[3].target = (unsigned char*)get_page(table->pager, survivor_num);
    entries[3].staged = survivor_after;
    entries[4].page_num = donor_num;
    entries[4].target = (unsigned char*)get_page(table->pager, donor_num);
    entries[4].staged = donor_after;
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
