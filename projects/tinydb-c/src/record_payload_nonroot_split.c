#include "record_payload_nonroot_split.h"

#include "generic_index_epoch.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_tree_split_stage.h"
#include "slotted_leaf_v2_tree_split_tail_stage.h"
#include "slotted_v2_publish_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static uint32_t read_u32_native(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static bool previous_boundary_allows(Table* table,
                                     uint32_t previous_page_num,
                                     uint32_t current_page_num,
                                     uint32_t key) {
    if (previous_page_num == 0u) return true;
    if (table == NULL || table->pager == NULL ||
        previous_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char previous_page[PAGE_SIZE];
    memcpy(previous_page,
           get_page(table->pager, previous_page_num),
           PAGE_SIZE);
    uint32_t previous_next = 0u;
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_leaf_format_detect_page(previous_page, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(previous_page, PAGE_SIZE) &&
           tinydb_leaf_page_next(previous_page, PAGE_SIZE, &previous_next) &&
           previous_next == current_page_num &&
           tinydb_leaf_page_count(previous_page, PAGE_SIZE, &count) &&
           count > 0u &&
           tinydb_leaf_page_key_at(previous_page,
                                   PAGE_SIZE,
                                   count - 1u,
                                   &max_key) &&
           key > max_key;
}

static bool next_boundary_allows(const unsigned char next_page[PAGE_SIZE],
                                 uint32_t current_page_num,
                                 uint32_t current_max_key) {
    uint32_t next_previous = 0u;
    uint32_t count = 0u;
    uint32_t min_key = 0u;
    return next_page != NULL &&
           tinydb_leaf_format_detect_page(next_page, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(next_page, PAGE_SIZE) &&
           tinydb_leaf_page_prev(next_page, PAGE_SIZE, &next_previous) &&
           next_previous == current_page_num &&
           tinydb_leaf_page_count(next_page, PAGE_SIZE, &count) &&
           count > 0u &&
           tinydb_leaf_page_key_at(next_page, PAGE_SIZE, 0u, &min_key) &&
           min_key > current_max_key;
}

static bool peek_unused_page_num(const Pager* pager, uint32_t* page_num) {
    if (pager == NULL || page_num == NULL) return false;
    if (pager->free_page_count > 0u) {
        *page_num = pager->free_pages[pager->free_page_count - 1u];
    } else {
        if (pager->num_pages == INVALID_PAGE_NUM) return false;
        *page_num = pager->num_pages;
    }
    return *page_num != 0u && *page_num != INVALID_PAGE_NUM;
}

static void restore_allocator_reservation(Pager* pager,
                                          uint32_t original_num_pages,
                                          uint32_t original_free_page_count) {
    if (pager == NULL) return;
    if (pager->num_pages > original_num_pages) {
        pager_shrink(pager, original_num_pages);
    }
    pager->free_page_count = original_free_page_count;
}

bool tinydb_record_payload_try_nonroot_split(
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

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    if (cursor == NULL || cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    uint32_t left_page_num = cursor->page_num;
    unsigned char left_before[PAGE_SIZE];
    memcpy(left_before,
           get_page(table->pager, left_page_num),
           PAGE_SIZE);

    uint32_t count = 0u;
    uint32_t found_key = 0u;
    if (left_page_num == schema->root_page_num ||
        tinydb_leaf_format_detect_page(left_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(left_before, PAGE_SIZE) ||
        !tinydb_leaf_page_count(left_before, PAGE_SIZE, &count) || count < 2u) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    if (cursor->cell_num < count &&
        tinydb_leaf_page_key_at(left_before,
                                PAGE_SIZE,
                                cursor->cell_num,
                                &found_key) &&
        found_key == key) {
        free(cursor);
        table->root_page_num = previous_root;
        set_message(message, message_size, "duplicate primary key");
        return false;
    }

    uint32_t old_left_max = 0u;
    uint32_t previous_page_num = 0u;
    uint32_t next_page_num = 0u;
    if (!tinydb_leaf_page_key_at(left_before,
                                 PAGE_SIZE,
                                 count - 1u,
                                 &old_left_max) ||
        !tinydb_leaf_page_prev(left_before,
                               PAGE_SIZE,
                               &previous_page_num) ||
        !tinydb_leaf_page_next(left_before,
                               PAGE_SIZE,
                               &next_page_num) ||
        next_page_num == left_page_num ||
        (next_page_num != 0u && next_page_num >= table->pager->num_pages) ||
        !previous_boundary_allows(table,
                                  previous_page_num,
                                  left_page_num,
                                  key)) {
        free(cursor);
        table->root_page_num = previous_root;
        set_message(message,
                    message_size,
                    "payload-native non-root split requires a valid sibling key range");
        return false;
    }

    bool is_tail = next_page_num == 0u;
    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length;
    if (required <= tinydb_slotted_leaf_v2_free_bytes(left_before, PAGE_SIZE)) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }
    if (applicable != NULL) *applicable = true;

    if (!is_tail && key >= old_left_max) {
        free(cursor);
        table->root_page_num = previous_root;
        set_message(message,
                    message_size,
                    "payload-native non-tail split must preserve the existing child upper boundary");
        return false;
    }

    uint32_t parent_page_num = read_u32_native(
        left_before + PARENT_POINTER_OFFSET);
    free(cursor);
    table->root_page_num = previous_root;
    /* Page zero is a valid catalog-stable root and therefore a valid parent
     * for any non-root child. Zero is only a sentinel for leaf sibling links;
     * do not reuse that sentinel rule for the parent pointer. In particular,
     * a tail child under root page zero legitimately has parent == 0 and
     * next == 0 at the same time. */
    if (parent_page_num >= table->pager->num_pages ||
        parent_page_num == left_page_num ||
        (!is_tail && parent_page_num == next_page_num)) {
        set_message(message,
                    message_size,
                    "payload-native non-root split requires an existing internal parent");
        return false;
    }

    unsigned char parent_before[PAGE_SIZE];
    unsigned char next_before[PAGE_SIZE];
    memcpy(parent_before,
           get_page(table->pager, parent_page_num),
           PAGE_SIZE);
    memset(next_before, 0, PAGE_SIZE);
    if (!is_tail) {
        memcpy(next_before,
               get_page(table->pager, next_page_num),
               PAGE_SIZE);
    }

    if (get_node_type(parent_before) != NODE_INTERNAL ||
        !tinydb_parent_stage_validate(parent_before, PAGE_SIZE) ||
        (!is_tail &&
         !next_boundary_allows(next_before, left_page_num, old_left_max))) {
        set_message(message,
                    message_size,
                    "payload-native non-root split found inconsistent parent/sibling topology");
        return false;
    }

    uint32_t parent_keys = read_u32_native(
        parent_before + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (parent_keys >= INTERNAL_NODE_MAX_KEYS) {
        set_message(message,
                    message_size,
                    "payload-native INSERT reached a full internal parent; recursive parent overflow is not implemented yet");
        return false;
    }

    uint32_t right_page_num = 0u;
    if (!peek_unused_page_num(table->pager, &right_page_num) ||
        right_page_num == left_page_num || right_page_num == parent_page_num ||
        right_page_num == next_page_num) {
        set_message(message,
                    message_size,
                    "unable to reserve a new V2 leaf for payload-native split");
        return false;
    }

    unsigned char left_after[PAGE_SIZE];
    unsigned char right_after[PAGE_SIZE];
    unsigned char parent_after[PAGE_SIZE];
    unsigned char next_after[PAGE_SIZE];
    memcpy(left_after, left_before, PAGE_SIZE);
    memset(right_after, 0, PAGE_SIZE);
    memcpy(parent_after, parent_before, PAGE_SIZE);
    memset(next_after, 0, PAGE_SIZE);
    if (!is_tail) memcpy(next_after, next_before, PAGE_SIZE);

    bool staged = is_tail
        ? tinydb_slotted_leaf_v2_stage_tree_split_nonroot_tail(
              left_after,
              PAGE_SIZE,
              left_page_num,
              right_after,
              PAGE_SIZE,
              right_page_num,
              parent_after,
              PAGE_SIZE,
              NULL,
              NULL)
        : tinydb_slotted_leaf_v2_stage_tree_split_nonroot_with_next(
              left_after,
              PAGE_SIZE,
              left_page_num,
              right_after,
              PAGE_SIZE,
              right_page_num,
              next_after,
              PAGE_SIZE,
              next_page_num,
              parent_after,
              PAGE_SIZE,
              NULL,
              NULL);
    if (!staged) {
        set_message(message,
                    message_size,
                    "payload-native V2 leaf split staging rejected parent overflow or inconsistent topology");
        return false;
    }

    uint16_t left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint16_t right_count = tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
    uint32_t left_max = 0u;
    uint32_t right_max = 0u;
    if (left_count == 0u || right_count == 0u ||
        !tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &left_max) ||
        !tinydb_leaf_page_key_at(right_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &right_max) ||
        right_max != old_left_max) {
        set_message(message,
                    message_size,
                    "payload-native V2 leaf split changed the existing child boundary before pending insert");
        return false;
    }

    void* destination = key <= left_max ? (void*)left_after : (void*)right_after;
    if (!tinydb_slotted_leaf_v2_insert(destination,
                                       PAGE_SIZE,
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length) ||
        !tinydb_slotted_leaf_v2_validate(left_after, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right_after, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(parent_after, PAGE_SIZE) ||
        (!is_tail && !tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE))) {
        set_message(message,
                    message_size,
                    "byte-balanced payload-native V2 split did not leave space for the pending row");
        return false;
    }

    uint32_t checked_left_max = 0u;
    uint32_t checked_right_max = 0u;
    left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    right_count = tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
    if (left_count == 0u || right_count == 0u ||
        !tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &checked_left_max) ||
        !tinydb_leaf_page_key_at(right_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &checked_right_max) ||
        checked_left_max != left_max ||
        (!is_tail && checked_right_max != old_left_max) ||
        (is_tail &&
         (checked_right_max < old_left_max ||
          checked_right_max <= checked_left_max))) {
        set_message(message,
                    message_size,
                    "payload-native split pending row invalidated staged parent separators");
        return false;
    }

    /* Claim the page reservation before publishing the durable generic-index
     * mutation epoch. A reservation mismatch is a pre-mutation failure and
     * must not advance the epoch or make an otherwise current secondary-index
     * snapshot look stale. The reservation itself is reversible until the
     * staged page batch is published. */
    uint32_t original_num_pages = table->pager->num_pages;
    uint32_t original_free_page_count = table->pager->free_page_count;
    uint32_t claimed = get_unused_page_num(table->pager);
    if (claimed != right_page_num) {
        restore_allocator_reservation(table->pager,
                                      original_num_pages,
                                      original_free_page_count);
        set_message(message,
                    message_size,
                    "payload-native V2 split page reservation changed before publication");
        return false;
    }
    unsigned char* right_target =
        (unsigned char*)get_page(table->pager, right_page_num);
    unsigned char* left_target =
        (unsigned char*)get_page(table->pager, left_page_num);
    unsigned char* parent_target =
        (unsigned char*)get_page(table->pager, parent_page_num);
    unsigned char* next_target = is_tail
        ? NULL
        : (unsigned char*)get_page(table->pager, next_page_num);
    if (right_target == NULL || left_target == NULL || parent_target == NULL ||
        (!is_tail && next_target == NULL)) {
        restore_allocator_reservation(table->pager,
                                      original_num_pages,
                                      original_free_page_count);
        set_message(message,
                    message_size,
                    "payload-native V2 split could not acquire publication targets");
        return false;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        restore_allocator_reservation(table->pager,
                                      original_num_pages,
                                      original_free_page_count);
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }

    TinyDBV2PublishEntry entries[4];
    uint32_t entry_count = 0u;
    entries[entry_count++] = (TinyDBV2PublishEntry){
        left_page_num, left_target, left_after};
    entries[entry_count++] = (TinyDBV2PublishEntry){
        right_page_num, right_target, right_after};
    if (!is_tail) {
        entries[entry_count++] = (TinyDBV2PublishEntry){
            next_page_num, next_target, next_after};
    }
    entries[entry_count++] = (TinyDBV2PublishEntry){
        parent_page_num, parent_target, parent_after};

    if (!tinydb_v2_publish_batch(entries,
                                 entry_count,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        restore_allocator_reservation(table->pager,
                                      original_num_pages,
                                      original_free_page_count);
        set_message(message,
                    message_size,
                    "payload-native V2 split atomic page publication failed");
        return false;
    }

    mark_page_dirty(table->pager, left_page_num);
    mark_page_dirty(table->pager, right_page_num);
    mark_page_dirty(table->pager, parent_page_num);
    if (!is_tail) mark_page_dirty(table->pager, next_page_num);

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}