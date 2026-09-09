#include "record_payload_nonroot_internal_split.h"

#include "generic_index_epoch.h"
#include "internal_nonroot_split_stage.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_leaf_v2_split.h"
#include "slotted_v2_publish_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text != NULL ? text : "");
    }
}

static uint32_t read_u32_native(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void write_u32_native(unsigned char* bytes, uint32_t value) {
    memcpy(bytes, &value, sizeof(value));
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

    unsigned char previous[PAGE_SIZE];
    memcpy(previous,
           get_page(table->pager, previous_page_num),
           PAGE_SIZE);
    uint32_t previous_next = 0u;
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_leaf_format_detect_page(previous, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(previous, PAGE_SIZE) &&
           tinydb_leaf_page_next(previous, PAGE_SIZE, &previous_next) &&
           previous_next == current_page_num &&
           tinydb_leaf_page_count(previous, PAGE_SIZE, &count) && count > 0u &&
           tinydb_leaf_page_key_at(previous,
                                   PAGE_SIZE,
                                   count - 1u,
                                   &max_key) &&
           key > max_key;
}

static bool next_boundary_allows(const unsigned char next_page[PAGE_SIZE],
                                 uint32_t current_page_num,
                                 uint32_t current_max_key) {
    uint32_t previous = 0u;
    uint32_t count = 0u;
    uint32_t min_key = 0u;
    return next_page != NULL &&
           tinydb_leaf_format_detect_page(next_page, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(next_page, PAGE_SIZE) &&
           tinydb_leaf_page_prev(next_page, PAGE_SIZE, &previous) &&
           previous == current_page_num &&
           tinydb_leaf_page_count(next_page, PAGE_SIZE, &count) && count > 0u &&
           tinydb_leaf_page_key_at(next_page, PAGE_SIZE, 0u, &min_key) &&
           min_key > current_max_key;
}

static bool parent_child_index(const unsigned char parent[PAGE_SIZE],
                               uint32_t child_page_num,
                               uint32_t* index_out) {
    if (parent == NULL || index_out == NULL ||
        !tinydb_parent_stage_validate(parent, PAGE_SIZE)) {
        return false;
    }
    uint32_t keys = tinydb_parent_stage_read_u32(
        parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= keys; i++) {
        if (tinydb_parent_stage_child_at(parent, i) == child_page_num) {
            *index_out = i;
            return true;
        }
    }
    return false;
}

static bool leaf_parent_actual_max(Table* table,
                                   const unsigned char parent[PAGE_SIZE],
                                   uint32_t* max_key_out) {
    if (table == NULL || table->pager == NULL || parent == NULL ||
        max_key_out == NULL || !tinydb_parent_stage_validate(parent, PAGE_SIZE)) {
        return false;
    }
    uint32_t keys = tinydb_parent_stage_read_u32(
        parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    uint32_t right_child = tinydb_parent_stage_child_at(parent, keys);
    if (right_child == 0u || right_child >= table->pager->num_pages) {
        return false;
    }

    unsigned char leaf[PAGE_SIZE];
    memcpy(leaf, get_page(table->pager, right_child), PAGE_SIZE);
    uint32_t count = 0u;
    return get_node_type(leaf) == NODE_LEAF &&
           tinydb_leaf_format_detect_page(leaf, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(leaf, PAGE_SIZE) &&
           tinydb_leaf_page_count(leaf, PAGE_SIZE, &count) && count > 0u &&
           tinydb_leaf_page_key_at(leaf,
                                   PAGE_SIZE,
                                   count - 1u,
                                   max_key_out);
}

static bool staged_parent_for_child(
    const unsigned char left_internal[PAGE_SIZE],
    uint32_t left_internal_page_num,
    const unsigned char right_internal[PAGE_SIZE],
    uint32_t right_internal_page_num,
    uint32_t child_page_num,
    uint32_t* parent_page_num) {
    if (parent_page_num == NULL || child_page_num == 0u) return false;
    const unsigned char* nodes[2] = {left_internal, right_internal};
    const uint32_t page_nums[2] = {
        left_internal_page_num,
        right_internal_page_num
    };

    for (uint32_t n = 0u; n < 2u; n++) {
        uint32_t keys = tinydb_parent_stage_read_u32(
            nodes[n] + INTERNAL_NODE_NUM_KEYS_OFFSET);
        for (uint32_t i = 0u; i <= keys; i++) {
            if (tinydb_parent_stage_child_at(nodes[n], i) == child_page_num) {
                *parent_page_num = page_nums[n];
                return true;
            }
        }
    }
    return false;
}

static bool validate_existing_children(
    Table* table,
    uint32_t old_parent_page_num,
    uint32_t new_leaf_page_num,
    const unsigned char left_internal[PAGE_SIZE],
    const unsigned char right_internal[PAGE_SIZE]) {
    if (table == NULL || table->pager == NULL) return false;
    const unsigned char* nodes[2] = {left_internal, right_internal};

    for (uint32_t n = 0u; n < 2u; n++) {
        uint32_t keys = tinydb_parent_stage_read_u32(
            nodes[n] + INTERNAL_NODE_NUM_KEYS_OFFSET);
        for (uint32_t i = 0u; i <= keys; i++) {
            uint32_t child_page_num = tinydb_parent_stage_child_at(nodes[n], i);
            if (child_page_num == new_leaf_page_num) continue;
            if (child_page_num == 0u ||
                child_page_num >= table->pager->num_pages ||
                child_page_num == old_parent_page_num) {
                return false;
            }
            unsigned char* child =
                (unsigned char*)get_page(table->pager, child_page_num);
            if (child == NULL || get_node_type(child) != NODE_LEAF ||
                tinydb_leaf_format_detect_page(child, PAGE_SIZE) !=
                    TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
                !tinydb_slotted_leaf_v2_validate(child, PAGE_SIZE) ||
                child[IS_ROOT_OFFSET] != 0u ||
                read_u32_native(child + PARENT_POINTER_OFFSET) !=
                    old_parent_page_num) {
                return false;
            }
        }
    }
    return true;
}

static void reparent_moved_children(
    Table* table,
    uint32_t old_parent_page_num,
    uint32_t new_right_internal_page_num,
    const unsigned char right_internal[PAGE_SIZE],
    uint32_t split_left_page_num,
    uint32_t split_right_page_num,
    uint32_t next_page_num) {
    uint32_t keys = tinydb_parent_stage_read_u32(
        right_internal + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= keys; i++) {
        uint32_t child_page_num = tinydb_parent_stage_child_at(right_internal, i);
        if (child_page_num == split_left_page_num ||
            child_page_num == split_right_page_num ||
            child_page_num == next_page_num) {
            continue;
        }
        unsigned char* child =
            (unsigned char*)get_page(table->pager, child_page_num);
        if (child != NULL &&
            read_u32_native(child + PARENT_POINTER_OFFSET) ==
                old_parent_page_num) {
            write_u32_native(child + PARENT_POINTER_OFFSET,
                             new_right_internal_page_num);
            mark_page_dirty(table->pager, child_page_num);
        }
    }
}

static bool peek_unused_page_nums(const Pager* pager,
                                  uint32_t page_nums[2]) {
    if (pager == NULL || page_nums == NULL) return false;
    uint32_t free_count = pager->free_page_count;
    uint32_t appended = free_count >= 2u ? 0u : 2u - free_count;
    if (appended > 0u && pager->num_pages > INVALID_PAGE_NUM - appended) {
        return false;
    }

    for (uint32_t i = 0u; i < 2u; i++) {
        page_nums[i] = i < free_count
            ? pager->free_pages[free_count - 1u - i]
            : pager->num_pages + (i - free_count);
        if (page_nums[i] == 0u || page_nums[i] == INVALID_PAGE_NUM) {
            return false;
        }
        if (i > 0u && page_nums[i] == page_nums[0]) return false;
    }
    return true;
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

static bool claim_reserved_pages(Pager* pager,
                                 const uint32_t page_nums[2],
                                 unsigned char* targets[2],
                                 uint32_t* original_num_pages,
                                 uint32_t* original_free_page_count) {
    if (pager == NULL || page_nums == NULL || targets == NULL ||
        original_num_pages == NULL || original_free_page_count == NULL) {
        return false;
    }

    *original_num_pages = pager->num_pages;
    *original_free_page_count = pager->free_page_count;
    targets[0] = NULL;
    targets[1] = NULL;
    for (uint32_t i = 0u; i < 2u; i++) {
        uint32_t claimed = get_unused_page_num(pager);
        if (claimed != page_nums[i]) {
            restore_allocator_reservation(pager,
                                          *original_num_pages,
                                          *original_free_page_count);
            return false;
        }
        targets[i] = (unsigned char*)get_page(pager, claimed);
        if (targets[i] == NULL) {
            restore_allocator_reservation(pager,
                                          *original_num_pages,
                                          *original_free_page_count);
            return false;
        }
    }
    return true;
}

bool tinydb_record_payload_try_full_nonroot_parent_split(
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
    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length;
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
        required <= tinydb_slotted_leaf_v2_free_bytes(left_before, PAGE_SIZE) ||
        next_page_num == left_page_num ||
        (next_page_num != 0u && next_page_num >= table->pager->num_pages) ||
        !previous_boundary_allows(table,
                                  previous_page_num,
                                  left_page_num,
                                  key)) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    bool is_tail = next_page_num == 0u;
    if (!is_tail && key >= old_left_max) {
        free(cursor);
        table->root_page_num = previous_root;
        set_message(message,
                    message_size,
                    "payload-native non-tail internal split must preserve the existing leaf maximum");
        return false;
    }

    uint32_t parent_page_num = read_u32_native(
        left_before + PARENT_POINTER_OFFSET);
    free(cursor);
    table->root_page_num = previous_root;
    if (parent_page_num == schema->root_page_num ||
        parent_page_num >= table->pager->num_pages ||
        parent_page_num == left_page_num) {
        return false;
    }

    unsigned char parent_before[PAGE_SIZE];
    memcpy(parent_before,
           get_page(table->pager, parent_page_num),
           PAGE_SIZE);
    if (get_node_type(parent_before) != NODE_INTERNAL ||
        parent_before[IS_ROOT_OFFSET] != 0u ||
        !tinydb_parent_stage_validate(parent_before, PAGE_SIZE) ||
        read_u32_native(parent_before + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS) {
        return false;
    }
    if (applicable != NULL) *applicable = true;

    uint32_t grandparent_page_num = read_u32_native(
        parent_before + PARENT_POINTER_OFFSET);
    if (grandparent_page_num >= table->pager->num_pages ||
        grandparent_page_num == parent_page_num ||
        grandparent_page_num == left_page_num) {
        set_message(message,
                    message_size,
                    "full non-root internal parent has no valid grandparent");
        return false;
    }

    unsigned char grandparent_before[PAGE_SIZE];
    memcpy(grandparent_before,
           get_page(table->pager, grandparent_page_num),
           PAGE_SIZE);
    uint32_t grandparent_keys = read_u32_native(
        grandparent_before + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (get_node_type(grandparent_before) != NODE_INTERNAL ||
        grandparent_keys == 0u || grandparent_keys > INTERNAL_NODE_MAX_KEYS ||
        !tinydb_parent_stage_validate(grandparent_before, PAGE_SIZE)) {
        set_message(message,
                    message_size,
                    "invalid grandparent blocks payload-native non-root internal split");
        return false;
    }
    if (grandparent_keys >= INTERNAL_NODE_MAX_KEYS) {
        set_message(message,
                    message_size,
                    "payload-native recursive internal overflow beyond the grandparent remains fail-closed");
        return false;
    }

    uint32_t parent_index = 0u;
    if (!parent_child_index(grandparent_before,
                            parent_page_num,
                            &parent_index)) {
        set_message(message,
                    message_size,
                    "grandparent does not reference the full internal parent");
        return false;
    }
    bool parent_was_rightmost = parent_index == grandparent_keys;
    uint32_t old_parent_max = 0u;
    if (parent_was_rightmost) {
        if (!leaf_parent_actual_max(table,
                                    parent_before,
                                    &old_parent_max)) {
            set_message(message,
                        message_size,
                        "unable to determine full internal subtree maximum");
            return false;
        }
    } else {
        old_parent_max = tinydb_parent_stage_key_at(grandparent_before,
                                                    parent_index);
    }
    if (is_tail && !parent_was_rightmost) {
        set_message(message,
                    message_size,
                    "global tail leaf cannot belong to a non-rightmost grandparent child");
        return false;
    }

    unsigned char next_before[PAGE_SIZE];
    memset(next_before, 0, PAGE_SIZE);
    if (!is_tail) {
        memcpy(next_before,
               get_page(table->pager, next_page_num),
               PAGE_SIZE);
        if (!next_boundary_allows(next_before,
                                  left_page_num,
                                  old_left_max)) {
            set_message(message,
                        message_size,
                        "invalid next sibling blocks payload-native non-root internal split");
            return false;
        }
    }

    uint32_t reserved_pages[2] = {0u, 0u};
    if (!peek_unused_page_nums(table->pager, reserved_pages)) {
        set_message(message,
                    message_size,
                    "unable to reserve pages for payload-native non-root internal split");
        return false;
    }
    uint32_t right_leaf_page_num = reserved_pages[0];
    uint32_t right_internal_page_num = reserved_pages[1];
    const uint32_t existing_pages[5] = {
        left_page_num,
        parent_page_num,
        grandparent_page_num,
        next_page_num,
        INVALID_PAGE_NUM
    };
    for (uint32_t i = 0u; i < 2u; i++) {
        for (uint32_t j = 0u; j < 4u; j++) {
            if (reserved_pages[i] == existing_pages[j]) {
                set_message(message,
                            message_size,
                            "payload-native non-root internal reservation collided with existing topology");
                return false;
            }
        }
    }

    unsigned char left_after[PAGE_SIZE];
    unsigned char right_leaf_after[PAGE_SIZE];
    unsigned char next_after[PAGE_SIZE];
    unsigned char parent_after[PAGE_SIZE];
    unsigned char right_internal_after[PAGE_SIZE];
    unsigned char grandparent_after[PAGE_SIZE];
    memcpy(left_after, left_before, PAGE_SIZE);
    memset(right_leaf_after, 0, PAGE_SIZE);
    memset(next_after, 0, PAGE_SIZE);
    if (!is_tail) memcpy(next_after, next_before, PAGE_SIZE);
    memcpy(parent_after, parent_before, PAGE_SIZE);
    memset(right_internal_after, 0, PAGE_SIZE);
    memcpy(grandparent_after, grandparent_before, PAGE_SIZE);

    bool staged = tinydb_slotted_leaf_v2_split_nonroot(
        left_after,
        PAGE_SIZE,
        left_page_num,
        right_leaf_after,
        PAGE_SIZE,
        right_leaf_page_num,
        NULL);
    if (staged && !is_tail) {
        tinydb_slotted_split_write_u32(
            next_after + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            right_leaf_page_num);
        staged = tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE);
    }

    uint16_t staged_left_count =
        tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint16_t staged_right_count =
        tinydb_slotted_leaf_v2_count(right_leaf_after, PAGE_SIZE);
    uint32_t staged_left_max = 0u;
    uint32_t staged_right_max = 0u;
    staged = staged && staged_left_count > 0u && staged_right_count > 0u &&
             tinydb_leaf_page_key_at(left_after,
                                     PAGE_SIZE,
                                     (uint32_t)staged_left_count - 1u,
                                     &staged_left_max) &&
             tinydb_leaf_page_key_at(right_leaf_after,
                                     PAGE_SIZE,
                                     (uint32_t)staged_right_count - 1u,
                                     &staged_right_max) &&
             staged_right_max == old_left_max;

    uint32_t promoted_left_max = 0u;
    if (staged) {
        staged = tinydb_stage_full_nonroot_after_child_split(
            parent_after,
            PAGE_SIZE,
            parent_page_num,
            right_internal_after,
            PAGE_SIZE,
            right_internal_page_num,
            left_page_num,
            right_leaf_page_num,
            old_left_max,
            staged_left_max,
            staged_right_max,
            &promoted_left_max,
            NULL);
    }
    if (staged) {
        staged = validate_existing_children(table,
                                            parent_page_num,
                                            right_leaf_page_num,
                                            parent_after,
                                            right_internal_after);
    }

    uint32_t left_leaf_parent = 0u;
    uint32_t right_leaf_parent = 0u;
    uint32_t next_parent = 0u;
    if (staged) {
        staged = staged_parent_for_child(parent_after,
                                         parent_page_num,
                                         right_internal_after,
                                         right_internal_page_num,
                                         left_page_num,
                                         &left_leaf_parent) &&
                 staged_parent_for_child(parent_after,
                                         parent_page_num,
                                         right_internal_after,
                                         right_internal_page_num,
                                         right_leaf_page_num,
                                         &right_leaf_parent);
        if (staged && !is_tail) {
            staged = staged_parent_for_child(parent_after,
                                             parent_page_num,
                                             right_internal_after,
                                             right_internal_page_num,
                                             next_page_num,
                                             &next_parent);
        }
        if (staged) {
            write_u32_native(left_after + PARENT_POINTER_OFFSET,
                             left_leaf_parent);
            write_u32_native(right_leaf_after + PARENT_POINTER_OFFSET,
                             right_leaf_parent);
            if (!is_tail && next_parent != 0u) {
                write_u32_native(next_after + PARENT_POINTER_OFFSET,
                                 next_parent);
            }
        }
    }
    if (!staged) {
        set_message(message,
                    message_size,
                    "payload-native full non-root internal staging rejected descendants");
        return false;
    }

    void* destination = key <= staged_left_max
        ? (void*)left_after
        : (void*)right_leaf_after;
    if (!tinydb_slotted_leaf_v2_insert(destination,
                                       PAGE_SIZE,
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length) ||
        !tinydb_slotted_leaf_v2_validate(left_after, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right_leaf_after, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(parent_after, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(right_internal_after, PAGE_SIZE) ||
        (!is_tail && !tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE))) {
        set_message(message,
                    message_size,
                    "payload-native non-root split leaves could not accept the pending row");
        return false;
    }

    uint16_t checked_left_count =
        tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint16_t checked_right_count =
        tinydb_slotted_leaf_v2_count(right_leaf_after, PAGE_SIZE);
    uint32_t checked_left_max = 0u;
    uint32_t checked_right_max = 0u;
    if (checked_left_count == 0u || checked_right_count == 0u ||
        !tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)checked_left_count - 1u,
                                 &checked_left_max) ||
        !tinydb_leaf_page_key_at(right_leaf_after,
                                 PAGE_SIZE,
                                 (uint32_t)checked_right_count - 1u,
                                 &checked_right_max) ||
        checked_left_max != staged_left_max ||
        (!is_tail && checked_right_max != old_left_max) ||
        (is_tail && checked_right_max <= checked_left_max)) {
        set_message(message,
                    message_size,
                    "pending payload invalidated staged non-root split boundaries");
        return false;
    }

    uint32_t new_parent_max = is_tail ? checked_right_max : old_parent_max;
    if (!tinydb_slotted_leaf_v2_stage_parent_split(
            grandparent_after,
            PAGE_SIZE,
            parent_page_num,
            right_internal_page_num,
            old_parent_max,
            promoted_left_max,
            new_parent_max,
            NULL)) {
        set_message(message,
                    message_size,
                    "grandparent rejected payload-native non-root internal split");
        return false;
    }

    unsigned char* reserved_targets[2] = {NULL, NULL};
    uint32_t original_num_pages = 0u;
    uint32_t original_free_page_count = 0u;
    if (!claim_reserved_pages(table->pager,
                              reserved_pages,
                              reserved_targets,
                              &original_num_pages,
                              &original_free_page_count)) {
        set_message(message,
                    message_size,
                    "payload-native non-root internal reservation changed before publication");
        return false;
    }

    unsigned char* left_target =
        (unsigned char*)get_page(table->pager, left_page_num);
    unsigned char* parent_target =
        (unsigned char*)get_page(table->pager, parent_page_num);
    unsigned char* grandparent_target =
        (unsigned char*)get_page(table->pager, grandparent_page_num);
    unsigned char* next_target = is_tail
        ? NULL
        : (unsigned char*)get_page(table->pager, next_page_num);
    if (left_target == NULL || parent_target == NULL ||
        grandparent_target == NULL || (!is_tail && next_target == NULL)) {
        restore_allocator_reservation(table->pager,
                                      original_num_pages,
                                      original_free_page_count);
        set_message(message,
                    message_size,
                    "payload-native non-root internal split could not acquire publication targets");
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

    TinyDBV2PublishEntry entries[6];
    uint32_t entry_count = 0u;
    entries[entry_count++] = (TinyDBV2PublishEntry){
        left_page_num, left_target, left_after};
    entries[entry_count++] = (TinyDBV2PublishEntry){
        right_leaf_page_num, reserved_targets[0], right_leaf_after};
    if (!is_tail) {
        entries[entry_count++] = (TinyDBV2PublishEntry){
            next_page_num, next_target, next_after};
    }
    entries[entry_count++] = (TinyDBV2PublishEntry){
        parent_page_num, parent_target, parent_after};
    entries[entry_count++] = (TinyDBV2PublishEntry){
        right_internal_page_num, reserved_targets[1], right_internal_after};
    entries[entry_count++] = (TinyDBV2PublishEntry){
        grandparent_page_num, grandparent_target, grandparent_after};

    if (!tinydb_v2_publish_batch(entries,
                                 entry_count,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        restore_allocator_reservation(table->pager,
                                      original_num_pages,
                                      original_free_page_count);
        set_message(message,
                    message_size,
                    "payload-native non-root internal atomic publication failed");
        return false;
    }

    /* Mark the staged batch dirty before walking the many descendants that may
     * have moved into the new right internal half. Otherwise the small LRU
     * buffer pool could evict a freshly published page as clean while the
     * reparent walk is still in progress and reload the old on-disk topology. */
    mark_page_dirty(table->pager, left_page_num);
    mark_page_dirty(table->pager, right_leaf_page_num);
    mark_page_dirty(table->pager, parent_page_num);
    mark_page_dirty(table->pager, right_internal_page_num);
    mark_page_dirty(table->pager, grandparent_page_num);
    if (!is_tail) mark_page_dirty(table->pager, next_page_num);

    reparent_moved_children(table,
                            parent_page_num,
                            right_internal_page_num,
                            right_internal_after,
                            left_page_num,
                            right_leaf_page_num,
                            next_page_num);

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}
