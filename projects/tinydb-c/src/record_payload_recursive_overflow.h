#ifndef TINYDB_RECORD_PAYLOAD_RECURSIVE_OVERFLOW_H
#define TINYDB_RECORD_PAYLOAD_RECURSIVE_OVERFLOW_H

#include "generic_index_epoch.h"
#include "internal_split_cascade_stage.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record_payload_ancestor_chain.h"
#include "record_payload_overflow_reservation.h"
#include "record_payload_page_claim_plan.h"
#include "record_payload_page_claim_transaction.h"
#include "slotted_leaf_v2.h"
#include "slotted_v2_publish_batch.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * First production recursive payload-overflow slice.
 *
 * It handles exactly two consecutive full non-root internal ancestors above
 * the overflowing V2 leaf and lets the third ancestor absorb the promoted
 * separator. Keeping the slice at two full levels is deliberate: leaf/next,
 * three ancestor images, and two new internal pages fit in the existing
 * eight-page atomic publication batch. Deeper propagation remains fail-closed
 * until the publication transaction can stage more than eight page images.
 */

static void tinydb_payload_recursive_set_message(char* message,
                                                 size_t message_size,
                                                 const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text != NULL ? text : "");
    }
}

static uint32_t tinydb_payload_recursive_read_u32(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void tinydb_payload_recursive_write_u32(unsigned char* bytes,
                                               uint32_t value) {
    memcpy(bytes, &value, sizeof(value));
}

static bool tinydb_payload_recursive_subtree_max(Table* table,
                                                 uint32_t page_num,
                                                 uint32_t* max_key_out) {
    if (table == NULL || table->pager == NULL || max_key_out == NULL ||
        page_num >= table->pager->num_pages) {
        return false;
    }

    uint32_t current = page_num;
    for (uint32_t depth = 0u; depth <= table->pager->num_pages; depth++) {
        if (current >= table->pager->num_pages) return false;
        unsigned char page[PAGE_SIZE];
        memcpy(page, get_page(table->pager, current), PAGE_SIZE);
        if (get_node_type(page) == NODE_LEAF) {
            uint32_t count = 0u;
            return tinydb_leaf_page_count(page, PAGE_SIZE, &count) &&
                   count > 0u &&
                   tinydb_leaf_page_key_at(page,
                                           PAGE_SIZE,
                                           count - 1u,
                                           max_key_out);
        }
        if (get_node_type(page) != NODE_INTERNAL ||
            !tinydb_parent_stage_validate(page, PAGE_SIZE)) {
            return false;
        }
        uint32_t keys = tinydb_parent_stage_read_u32(
            page + INTERNAL_NODE_NUM_KEYS_OFFSET);
        uint32_t child = tinydb_parent_stage_child_at(page, keys);
        if (child == current || child >= table->pager->num_pages) return false;
        current = child;
    }
    return false;
}

static bool tinydb_payload_recursive_previous_ok(Table* table,
                                                 uint32_t previous_page_num,
                                                 uint32_t current_page_num,
                                                 uint32_t key) {
    if (previous_page_num == 0u) return true;
    if (table == NULL || table->pager == NULL ||
        previous_page_num >= table->pager->num_pages) {
        return false;
    }
    unsigned char page[PAGE_SIZE];
    memcpy(page, get_page(table->pager, previous_page_num), PAGE_SIZE);
    uint32_t next = 0u;
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_leaf_format_detect_page(page, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE) &&
           tinydb_leaf_page_next(page, PAGE_SIZE, &next) &&
           next == current_page_num &&
           tinydb_leaf_page_count(page, PAGE_SIZE, &count) && count > 0u &&
           tinydb_leaf_page_key_at(page,
                                   PAGE_SIZE,
                                   count - 1u,
                                   &max_key) &&
           key > max_key;
}

static bool tinydb_payload_recursive_assign_children(Pager* pager,
                                                     uint32_t parent_page_num,
                                                     const unsigned char parent[PAGE_SIZE]) {
    if (pager == NULL || parent == NULL ||
        !tinydb_parent_stage_validate(parent, PAGE_SIZE)) {
        return false;
    }
    uint32_t keys = tinydb_parent_stage_read_u32(
        parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= keys; i++) {
        uint32_t child_page_num = tinydb_parent_stage_child_at(parent, i);
        if (child_page_num >= pager->num_pages || child_page_num == parent_page_num) {
            return false;
        }
        unsigned char* child = (unsigned char*)get_page(pager, child_page_num);
        if (child == NULL) return false;
        if (tinydb_payload_recursive_read_u32(child + PARENT_POINTER_OFFSET) !=
            parent_page_num) {
            tinydb_payload_recursive_write_u32(child + PARENT_POINTER_OFFSET,
                                               parent_page_num);
            mark_page_dirty(pager, child_page_num);
        }
    }
    return true;
}

static bool tinydb_record_payload_try_two_level_recursive_overflow(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size) {
    if (applicable != NULL) *applicable = false;
    if (table == NULL || table->pager == NULL || schema == NULL ||
        envelope == NULL || envelope_length == 0u ||
        envelope_length > UINT16_MAX ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }

    TinyDBPayloadAncestorChain chain;
    TinyDBPayloadOverflowPlan plan;
    TinyDBPayloadOverflowReservation reservation = {0};
    TinyDBPayloadPageClaimPlan claim_plan = {0};
    char local_message[TINYDB_RECORD_MESSAGE_MAX];
    local_message[0] = '\0';

    if (!tinydb_record_payload_collect_ancestor_chain(table,
                                                       schema,
                                                       key,
                                                       &chain,
                                                       local_message,
                                                       sizeof(local_message))) {
        return false;
    }
    bool planned = tinydb_record_payload_plan_overflow_chain(table,
                                                              schema,
                                                              &chain,
                                                              &plan,
                                                              local_message,
                                                              sizeof(local_message));
    if (!planned || plan.requires_root_growth ||
        plan.full_internal_levels != 2u ||
        plan.stopping_ancestor_index != 2u || chain.count < 3u ||
        chain.internal_pages[2] == 0u) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        return false;
    }
    if (applicable != NULL) *applicable = true;

    if (!tinydb_record_payload_size_overflow_reservation(&chain,
                                                         &plan,
                                                         &reservation,
                                                         local_message,
                                                         sizeof(local_message)) ||
        reservation.new_leaf_pages != 1u ||
        reservation.new_internal_pages != 2u ||
        reservation.total_pages != 3u ||
        !tinydb_record_payload_prepare_page_claim_plan(table->pager,
                                                       &chain,
                                                       &reservation,
                                                       &claim_plan,
                                                       local_message,
                                                       sizeof(local_message))) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message,
            message_size,
            local_message[0] != '\0'
                ? local_message
                : "unable to prepare two-level recursive payload overflow");
        return false;
    }

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    table->root_page_num = previous_root;
    if (cursor == NULL || cursor->page_num != chain.leaf_page_num ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive payload target changed after ancestry preflight");
        return false;
    }

    uint32_t left_page_num = cursor->page_num;
    unsigned char left_before[PAGE_SIZE];
    memcpy(left_before, get_page(table->pager, left_page_num), PAGE_SIZE);
    uint32_t count = 0u;
    uint32_t found_key = 0u;
    if (tinydb_leaf_format_detect_page(left_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(left_before, PAGE_SIZE) ||
        !tinydb_leaf_page_count(left_before, PAGE_SIZE, &count) || count < 2u ||
        (cursor->cell_num < count &&
         tinydb_leaf_page_key_at(left_before,
                                 PAGE_SIZE,
                                 cursor->cell_num,
                                 &found_key) &&
         found_key == key)) {
        free(cursor);
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "invalid recursive payload target leaf");
        return false;
    }
    free(cursor);

    uint32_t old_left_max = 0u;
    uint32_t previous_page_num = 0u;
    uint32_t next_page_num = 0u;
    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length;
    if (!tinydb_leaf_page_key_at(left_before,
                                 PAGE_SIZE,
                                 count - 1u,
                                 &old_left_max) ||
        !tinydb_leaf_page_prev(left_before,
                               PAGE_SIZE,
                               &previous_page_num) ||
        !tinydb_leaf_page_next(left_before, PAGE_SIZE, &next_page_num) ||
        required <= tinydb_slotted_leaf_v2_free_bytes(left_before, PAGE_SIZE) ||
        !tinydb_payload_recursive_previous_ok(table,
                                              previous_page_num,
                                              left_page_num,
                                              key) ||
        next_page_num == left_page_num ||
        (next_page_num != 0u && next_page_num >= table->pager->num_pages)) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive payload leaf boundary validation failed");
        return false;
    }

    bool is_tail = next_page_num == 0u;
    if (!is_tail && key >= old_left_max) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive non-tail payload split would change an encoded upper boundary");
        return false;
    }

    unsigned char next_before[PAGE_SIZE];
    memset(next_before, 0, PAGE_SIZE);
    if (!is_tail) {
        memcpy(next_before, get_page(table->pager, next_page_num), PAGE_SIZE);
        uint32_t next_prev = 0u;
        if (tinydb_leaf_format_detect_page(next_before, PAGE_SIZE) !=
                TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
            !tinydb_slotted_leaf_v2_validate(next_before, PAGE_SIZE) ||
            !tinydb_leaf_page_prev(next_before, PAGE_SIZE, &next_prev) ||
            next_prev != left_page_num) {
            tinydb_record_payload_ancestor_chain_release(&chain);
            tinydb_record_payload_page_claim_plan_release(&claim_plan);
            tinydb_payload_recursive_set_message(
                message, message_size, "recursive payload next sibling is not reciprocal");
            return false;
        }
    }

    unsigned char ancestor_images[3u][PAGE_SIZE];
    uint32_t old_maxes[3u];
    for (uint32_t i = 0u; i < 3u; i++) {
        uint32_t page_num = chain.internal_pages[i];
        memcpy(ancestor_images[i], get_page(table->pager, page_num), PAGE_SIZE);
        if (!tinydb_parent_stage_validate(ancestor_images[i], PAGE_SIZE) ||
            !tinydb_payload_recursive_subtree_max(table,
                                                  page_num,
                                                  &old_maxes[i])) {
            tinydb_record_payload_ancestor_chain_release(&chain);
            tinydb_record_payload_page_claim_plan_release(&claim_plan);
            tinydb_payload_recursive_set_message(
                message, message_size, "recursive payload ancestor image validation failed");
            return false;
        }
    }
    if (tinydb_parent_stage_read_u32(
            ancestor_images[0] + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS ||
        tinydb_parent_stage_read_u32(
            ancestor_images[1] + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS ||
        tinydb_parent_stage_read_u32(
            ancestor_images[2] + INTERNAL_NODE_NUM_KEYS_OFFSET) >=
            INTERNAL_NODE_MAX_KEYS) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive payload ancestry changed after overflow planning");
        return false;
    }

    const uint32_t right_leaf_page_num = claim_plan.page_nums[0];
    const uint32_t* new_internal_page_nums = claim_plan.page_nums + 1u;
    unsigned char left_after[PAGE_SIZE];
    unsigned char right_after[PAGE_SIZE];
    unsigned char next_after[PAGE_SIZE];
    unsigned char new_internal_pages[2u][PAGE_SIZE];
    memcpy(left_after, left_before, PAGE_SIZE);
    memset(right_after, 0, PAGE_SIZE);
    memset(next_after, 0, PAGE_SIZE);
    memset(new_internal_pages, 0, sizeof(new_internal_pages));
    if (!is_tail) memcpy(next_after, next_before, PAGE_SIZE);

    if (!tinydb_slotted_leaf_v2_split_nonroot(left_after,
                                               PAGE_SIZE,
                                               left_page_num,
                                               right_after,
                                               PAGE_SIZE,
                                               right_leaf_page_num,
                                               NULL)) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "unable to stage recursive payload leaf split");
        return false;
    }
    if (!is_tail) {
        tinydb_slotted_split_write_u32(
            next_after + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            right_leaf_page_num);
        if (!tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE)) {
            tinydb_record_payload_ancestor_chain_release(&chain);
            tinydb_record_payload_page_claim_plan_release(&claim_plan);
            tinydb_payload_recursive_set_message(
                message, message_size, "recursive payload sibling relink staging failed");
            return false;
        }
    }

    uint16_t left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint16_t right_count = tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
    uint32_t staged_left_max = 0u;
    uint32_t staged_right_max = 0u;
    if (left_count == 0u || right_count == 0u ||
        !tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &staged_left_max) ||
        !tinydb_leaf_page_key_at(right_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &staged_right_max) ||
        staged_right_max != old_left_max) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive payload split changed its pre-insert boundary");
        return false;
    }

    void* destination = key <= staged_left_max
        ? (void*)left_after
        : (void*)right_after;
    if (!tinydb_slotted_leaf_v2_insert(destination,
                                       PAGE_SIZE,
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length) ||
        !tinydb_slotted_leaf_v2_validate(left_after, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right_after, PAGE_SIZE) ||
        (!is_tail && !tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE))) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive payload split leaves could not accept the pending row");
        return false;
    }

    left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    right_count = tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
    uint32_t checked_left_max = 0u;
    uint32_t checked_right_max = 0u;
    if (!tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &checked_left_max) ||
        !tinydb_leaf_page_key_at(right_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &checked_right_max) ||
        checked_left_max != staged_left_max ||
        (!is_tail && checked_right_max != old_left_max) ||
        (is_tail && checked_right_max <= checked_left_max)) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive payload pending row invalidated split boundaries");
        return false;
    }

    uint32_t new_pages_used = 0u;
    uint32_t stop_level = 0u;
    bool root_grew = false;
    if (!tinydb_stage_internal_split_cascade(ancestor_images,
                                             PAGE_SIZE,
                                             chain.internal_pages,
                                             old_maxes,
                                             3u,
                                             new_internal_pages,
                                             PAGE_SIZE,
                                             new_internal_page_nums,
                                             2u,
                                             left_page_num,
                                             right_leaf_page_num,
                                             old_left_max,
                                             checked_left_max,
                                             checked_right_max,
                                             &new_pages_used,
                                             &stop_level,
                                             &root_grew) ||
        new_pages_used != 2u || stop_level != 2u || root_grew) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive payload internal cascade staging failed");
        return false;
    }

    uint32_t claimed_count = 0u;
    if (!tinydb_record_payload_claim_prepared_pages(table->pager,
                                                    &claim_plan,
                                                    &claimed_count,
                                                    local_message,
                                                    sizeof(local_message))) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message,
            message_size,
            local_message[0] != '\0'
                ? local_message
                : "unable to claim recursive payload split pages");
        return false;
    }

    unsigned char* left_target = (unsigned char*)get_page(table->pager, left_page_num);
    unsigned char* right_target = (unsigned char*)get_page(table->pager, right_leaf_page_num);
    unsigned char* next_target = is_tail
        ? NULL
        : (unsigned char*)get_page(table->pager, next_page_num);
    unsigned char* ancestor_targets[3u];
    unsigned char* new_internal_targets[2u];
    for (uint32_t i = 0u; i < 3u; i++) {
        ancestor_targets[i] =
            (unsigned char*)get_page(table->pager, chain.internal_pages[i]);
    }
    for (uint32_t i = 0u; i < 2u; i++) {
        new_internal_targets[i] =
            (unsigned char*)get_page(table->pager, new_internal_page_nums[i]);
    }
    if (left_target == NULL || right_target == NULL ||
        (!is_tail && next_target == NULL) || ancestor_targets[0] == NULL ||
        ancestor_targets[1] == NULL || ancestor_targets[2] == NULL ||
        new_internal_targets[0] == NULL || new_internal_targets[1] == NULL) {
        (void)tinydb_record_payload_rollback_claimed_pages(table->pager,
                                                           &claim_plan,
                                                           claimed_count,
                                                           local_message,
                                                           sizeof(local_message));
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "unable to acquire recursive payload publication targets");
        return false;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        (void)tinydb_record_payload_rollback_claimed_pages(table->pager,
                                                           &claim_plan,
                                                           claimed_count,
                                                           local_message,
                                                           sizeof(local_message));
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "unable to persist generic-index mutation epoch");
        return false;
    }

    TinyDBV2PublishEntry entries[TINYDB_V2_PUBLISH_BATCH_MAX_PAGES];
    uint32_t entry_count = 0u;
    entries[entry_count++] = (TinyDBV2PublishEntry){left_page_num,
                                                    left_target,
                                                    left_after};
    entries[entry_count++] = (TinyDBV2PublishEntry){right_leaf_page_num,
                                                    right_target,
                                                    right_after};
    if (!is_tail) {
        entries[entry_count++] = (TinyDBV2PublishEntry){next_page_num,
                                                        next_target,
                                                        next_after};
    }
    for (uint32_t i = 0u; i < 3u; i++) {
        entries[entry_count++] = (TinyDBV2PublishEntry){chain.internal_pages[i],
                                                        ancestor_targets[i],
                                                        ancestor_images[i]};
    }
    for (uint32_t i = 0u; i < 2u; i++) {
        entries[entry_count++] = (TinyDBV2PublishEntry){new_internal_page_nums[i],
                                                        new_internal_targets[i],
                                                        new_internal_pages[i]};
    }
    if (entry_count > TINYDB_V2_PUBLISH_BATCH_MAX_PAGES ||
        !tinydb_v2_publish_batch(entries,
                                 entry_count,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        (void)tinydb_record_payload_rollback_claimed_pages(table->pager,
                                                           &claim_plan,
                                                           claimed_count,
                                                           local_message,
                                                           sizeof(local_message));
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive payload atomic page publication failed");
        return false;
    }

    mark_page_dirty(table->pager, left_page_num);
    mark_page_dirty(table->pager, right_leaf_page_num);
    if (!is_tail) mark_page_dirty(table->pager, next_page_num);
    for (uint32_t i = 0u; i < 3u; i++) {
        mark_page_dirty(table->pager, chain.internal_pages[i]);
    }
    for (uint32_t i = 0u; i < 2u; i++) {
        mark_page_dirty(table->pager, new_internal_page_nums[i]);
    }

    bool parents_ok = true;
    for (uint32_t i = 0u; i < 3u; i++) {
        parents_ok = tinydb_payload_recursive_assign_children(
                         table->pager,
                         chain.internal_pages[i],
                         ancestor_images[i]) &&
                     parents_ok;
    }
    for (uint32_t i = 0u; i < 2u; i++) {
        parents_ok = tinydb_payload_recursive_assign_children(
                         table->pager,
                         new_internal_page_nums[i],
                         new_internal_pages[i]) &&
                     parents_ok;
    }
    if (!parents_ok) {
        /* The staged internal topology was fully validated before publication,
         * so this can only indicate an unexpected Pager/topology corruption.
         * Leave the transaction dirty and fail closed; an explicit transaction
         * can roll back and autocommit callers will not be committed here. */
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        tinydb_payload_recursive_set_message(
            message, message_size, "recursive payload descendant reparenting failed");
        return false;
    }

    if (!table->in_transaction) pager_commit(table->pager);
    tinydb_record_payload_ancestor_chain_release(&chain);
    tinydb_record_payload_page_claim_plan_release(&claim_plan);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

#endif /* TINYDB_RECORD_PAYLOAD_RECURSIVE_OVERFLOW_H */
