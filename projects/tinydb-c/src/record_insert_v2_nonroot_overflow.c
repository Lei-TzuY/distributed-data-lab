#include "generic_index_epoch.h"
#include "internal_nonroot_split_stage.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_parent_stage.h"
#include "slotted_leaf_v2_split.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool tinydb_record_insert_v2_split_base(Table* table,
                                        const TableSchema* schema,
                                        const TinyDBValue* values,
                                        uint32_t value_count,
                                        char* message,
                                        size_t message_size);

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

static void write_u32_native(unsigned char* bytes, uint32_t value) {
    memcpy(bytes, &value, sizeof(value));
}

static bool encode_compact_insert(const TableSchema* schema,
                                  const TinyDBValue* values,
                                  uint32_t value_count,
                                  uint32_t* key,
                                  unsigned char envelope[PAGE_SIZE],
                                  uint32_t* envelope_length,
                                  char* message,
                                  size_t message_size) {
    if (schema == NULL || values == NULL || key == NULL || envelope == NULL ||
        envelope_length == NULL ||
        !tinydb_schema_supports_records(schema, message, message_size)) {
        return false;
    }

    TinyDBRecord record;
    TinyDBRecordPayload payload;
    if (!tinydb_record_encode(schema,
                              values,
                              value_count,
                              &record,
                              message,
                              message_size) ||
        !tinydb_record_payload_from_record(schema,
                                           &record,
                                           &payload,
                                           message,
                                           message_size) ||
        payload.length < sizeof(uint32_t)) {
        return false;
    }
    memcpy(key, payload.bytes, sizeof(*key));

    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               &payload,
                                               envelope,
                                               PAGE_SIZE,
                                               envelope_length) ||
        *envelope_length == 0u || *envelope_length > UINT16_MAX) {
        set_message(message,
                    message_size,
                    "unable to encode compact V2 row for non-root parent overflow");
        return false;
    }
    return true;
}

static bool previous_boundary_allows(Table* table,
                                     uint32_t previous_page_num,
                                     uint32_t key) {
    if (previous_page_num == 0u) return true;
    if (table == NULL || table->pager == NULL ||
        previous_page_num >= table->pager->num_pages) {
        return false;
    }
    unsigned char previous[PAGE_SIZE];
    memcpy(previous,
           get_page(table->pager, previous_page_num),
           sizeof(previous));
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_leaf_page_count(previous, PAGE_SIZE, &count) &&
           count > 0u &&
           tinydb_leaf_page_key_at(previous,
                                   PAGE_SIZE,
                                   count - 1u,
                                   &max_key) &&
           key > max_key;
}

static bool peek_unused_page_nums(const Pager* pager,
                                  uint32_t page_nums[2]) {
    if (pager == NULL || page_nums == NULL) return false;
    uint32_t free_count = pager->free_page_count;
    uint32_t appended = free_count >= 2u ? 0u : 2u - free_count;
    if (appended > 0u &&
        pager->num_pages > INVALID_PAGE_NUM - appended) {
        return false;
    }
    for (uint32_t i = 0u; i < 2u; i++) {
        page_nums[i] = i < free_count
            ? pager->free_pages[free_count - 1u - i]
            : pager->num_pages + (i - free_count);
        if (page_nums[i] == 0u || page_nums[i] == INVALID_PAGE_NUM ||
            (i > 0u && page_nums[i] == page_nums[0])) {
            return false;
        }
    }
    return true;
}

static bool claim_reserved_page_nums(Pager* pager,
                                     const uint32_t page_nums[2]) {
    if (pager == NULL || page_nums == NULL) return false;
    for (uint32_t i = 0u; i < 2u; i++) {
        uint32_t claimed = get_unused_page_num(pager);
        if (claimed != page_nums[i]) return false;
        (void)get_page(pager, claimed);
    }
    return true;
}

static bool publish_page(Pager* pager,
                         uint32_t page_num,
                         const unsigned char image[PAGE_SIZE]) {
    if (pager == NULL || image == NULL || page_num == INVALID_PAGE_NUM) {
        return false;
    }
    void* page = get_page(pager, page_num);
    if (page == NULL) return false;
    memcpy(page, image, PAGE_USABLE_SIZE);
    mark_page_dirty(pager, page_num);
    return true;
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
    if (right_child == 0u || right_child >= table->pager->num_pages) return false;

    unsigned char leaf[PAGE_SIZE];
    memcpy(leaf, get_page(table->pager, right_child), sizeof(leaf));
    uint32_t count = 0u;
    return get_node_type(leaf) == NODE_LEAF &&
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
        if (read_u32_native(child + PARENT_POINTER_OFFSET) == old_parent_page_num) {
            write_u32_native(child + PARENT_POINTER_OFFSET,
                             new_right_internal_page_num);
            mark_page_dirty(table->pager, child_page_num);
        }
    }
}

static bool try_full_nonroot_parent_split(
    Table* table,
    const TableSchema* schema,
    const TinyDBValue* values,
    uint32_t value_count,
    bool* applicable,
    char* message,
    size_t message_size) {
    if (applicable != NULL) *applicable = false;
    if (table == NULL || table->pager == NULL || schema == NULL) return false;

    uint32_t key = 0u;
    uint32_t envelope_length = 0u;
    unsigned char envelope[PAGE_SIZE];
    memset(envelope, 0, sizeof(envelope));
    if (!encode_compact_insert(schema,
                               values,
                               value_count,
                               &key,
                               envelope,
                               &envelope_length,
                               message,
                               message_size)) {
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
        table->root_page_num = previous_root;
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
        !tinydb_leaf_page_next(left_before, PAGE_SIZE, &next_page_num) ||
        required <= tinydb_slotted_leaf_v2_free_bytes(left_before, PAGE_SIZE) ||
        !previous_boundary_allows(table, previous_page_num, key) ||
        (next_page_num != 0u &&
         (next_page_num >= table->pager->num_pages ||
          next_page_num == left_page_num))) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    bool is_tail = next_page_num == 0u;
    if (!is_tail && key >= old_left_max) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    uint32_t parent_page_num = read_u32_native(
        left_before + PARENT_POINTER_OFFSET);
    free(cursor);
    table->root_page_num = previous_root;
    if (parent_page_num == 0u || parent_page_num >= table->pager->num_pages ||
        parent_page_num == schema->root_page_num) {
        return false;
    }

    unsigned char parent_before[PAGE_SIZE];
    memcpy(parent_before, get_page(table->pager, parent_page_num), PAGE_SIZE);
    if (get_node_type(parent_before) != NODE_INTERNAL ||
        parent_before[IS_ROOT_OFFSET] != 0u ||
        read_u32_native(parent_before + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS) {
        return false;
    }
    if (applicable != NULL) *applicable = true;

    uint32_t grandparent_page_num = read_u32_native(
        parent_before + PARENT_POINTER_OFFSET);
    if (grandparent_page_num == 0u ||
        grandparent_page_num >= table->pager->num_pages ||
        grandparent_page_num == parent_page_num) {
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
                    "invalid grandparent blocks non-root internal split");
        return false;
    }
    if (grandparent_keys >= INTERNAL_NODE_MAX_KEYS) {
        set_message(message,
                    message_size,
                    "recursive internal overflow beyond the grandparent remains fail-closed");
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
                        "unable to determine rightmost internal subtree maximum");
            return false;
        }
    } else {
        old_parent_max = tinydb_parent_stage_key_at(grandparent_before,
                                                    parent_index);
    }
    if (is_tail && !parent_was_rightmost) {
        set_message(message,
                    message_size,
                    "tail leaf cannot belong to a non-rightmost grandparent child");
        return false;
    }

    unsigned char next_before[PAGE_SIZE];
    memset(next_before, 0, sizeof(next_before));
    if (!is_tail) {
        memcpy(next_before,
               get_page(table->pager, next_page_num),
               PAGE_SIZE);
        uint32_t next_prev = 0u;
        if (tinydb_leaf_format_detect_page(next_before, PAGE_SIZE) !=
                TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
            !tinydb_slotted_leaf_v2_validate(next_before, PAGE_SIZE) ||
            !tinydb_leaf_page_prev(next_before,
                                    PAGE_SIZE,
                                    &next_prev) ||
            next_prev != left_page_num) {
            set_message(message,
                        message_size,
                        "invalid V2 next sibling blocks non-root internal split");
            return false;
        }
    }

    uint32_t reserved_pages[2] = {0u, 0u};
    if (!peek_unused_page_nums(table->pager, reserved_pages)) {
        set_message(message,
                    message_size,
                    "unable to reserve pages for non-root internal split");
        return false;
    }
    uint32_t right_leaf_page_num = reserved_pages[0];
    uint32_t right_internal_page_num = reserved_pages[1];
    if (right_leaf_page_num == left_page_num ||
        right_leaf_page_num == parent_page_num ||
        right_leaf_page_num == grandparent_page_num ||
        right_leaf_page_num == next_page_num ||
        right_internal_page_num == left_page_num ||
        right_internal_page_num == parent_page_num ||
        right_internal_page_num == grandparent_page_num ||
        right_internal_page_num == next_page_num) {
        set_message(message,
                    message_size,
                    "non-root internal split page reservation collided with topology");
        return false;
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

    if (!tinydb_slotted_leaf_v2_split_nonroot(left_after,
                                               PAGE_SIZE,
                                               left_page_num,
                                               right_leaf_after,
                                               PAGE_SIZE,
                                               right_leaf_page_num,
                                               NULL)) {
        set_message(message,
                    message_size,
                    "unable to stage V2 leaf split before internal overflow");
        return false;
    }
    if (!is_tail) {
        tinydb_slotted_split_write_u32(
            next_after + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            right_leaf_page_num);
        if (!tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE)) {
            set_message(message,
                        message_size,
                        "next sibling backlink staging failed");
            return false;
        }
    }

    uint16_t staged_left_count =
        tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint16_t staged_right_count =
        tinydb_slotted_leaf_v2_count(right_leaf_after, PAGE_SIZE);
    uint32_t staged_left_max = 0u;
    uint32_t staged_right_max = 0u;
    if (staged_left_count == 0u || staged_right_count == 0u ||
        !tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)staged_left_count - 1u,
                                 &staged_left_max) ||
        !tinydb_leaf_page_key_at(right_leaf_after,
                                 PAGE_SIZE,
                                 (uint32_t)staged_right_count - 1u,
                                 &staged_right_max) ||
        staged_right_max != old_left_max) {
        set_message(message,
                    message_size,
                    "leaf split changed its pre-insert upper boundary");
        return false;
    }

    uint32_t promoted_left_max = 0u;
    if (!tinydb_stage_full_nonroot_after_child_split(
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
            NULL) ||
        !validate_existing_children(table,
                                    parent_page_num,
                                    right_leaf_page_num,
                                    parent_after,
                                    right_internal_after)) {
        set_message(message,
                    message_size,
                    "full non-root internal staging rejected descendant topology");
        return false;
    }

    uint32_t left_leaf_parent = 0u;
    uint32_t right_leaf_parent = 0u;
    if (!staged_parent_for_child(parent_after,
                                 parent_page_num,
                                 right_internal_after,
                                 right_internal_page_num,
                                 left_page_num,
                                 &left_leaf_parent) ||
        !staged_parent_for_child(parent_after,
                                 parent_page_num,
                                 right_internal_after,
                                 right_internal_page_num,
                                 right_leaf_page_num,
                                 &right_leaf_parent)) {
        set_message(message,
                    message_size,
                    "staged internal split lost the split leaf children");
        return false;
    }
    write_u32_native(left_after + PARENT_POINTER_OFFSET, left_leaf_parent);
    write_u32_native(right_leaf_after + PARENT_POINTER_OFFSET,
                     right_leaf_parent);

    if (!is_tail) {
        uint32_t next_parent = 0u;
        if (staged_parent_for_child(parent_after,
                                    parent_page_num,
                                    right_internal_after,
                                    right_internal_page_num,
                                    next_page_num,
                                    &next_parent)) {
            write_u32_native(next_after + PARENT_POINTER_OFFSET, next_parent);
        }
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
        (!is_tail && !tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE))) {
        set_message(message,
                    message_size,
                    "split leaves could not accept the pending row");
        return false;
    }

    uint32_t checked_left_max = 0u;
    uint32_t checked_right_max = 0u;
    uint16_t checked_left_count =
        tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint16_t checked_right_count =
        tinydb_slotted_leaf_v2_count(right_leaf_after, PAGE_SIZE);
    if (!tinydb_leaf_page_key_at(left_after,
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
                    "pending row invalidated non-root split boundaries");
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
                    "grandparent insertion rejected staged internal split");
        return false;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }
    if (!claim_reserved_page_nums(table->pager, reserved_pages)) {
        set_message(message,
                    message_size,
                    "non-root internal split page reservation changed before publication");
        return false;
    }

    reparent_moved_children(table,
                            parent_page_num,
                            right_internal_page_num,
                            right_internal_after,
                            left_page_num,
                            right_leaf_page_num,
                            next_page_num);

    if (!publish_page(table->pager, left_page_num, left_after) ||
        !publish_page(table->pager,
                      right_leaf_page_num,
                      right_leaf_after) ||
        (!is_tail && !publish_page(table->pager, next_page_num, next_after)) ||
        !publish_page(table->pager, parent_page_num, parent_after) ||
        !publish_page(table->pager,
                      right_internal_page_num,
                      right_internal_after) ||
        !publish_page(table->pager,
                      grandparent_page_num,
                      grandparent_after)) {
        set_message(message,
                    message_size,
                    "unable to publish staged non-root internal split");
        return false;
    }

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_insert(Table* table,
                          const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    char base_message[TINYDB_RECORD_MESSAGE_MAX];
    base_message[0] = '\0';
    if (tinydb_record_insert_v2_split_base(table,
                                           schema,
                                           values,
                                           value_count,
                                           base_message,
                                           sizeof(base_message))) {
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    bool applicable = false;
    char overflow_message[TINYDB_RECORD_MESSAGE_MAX];
    overflow_message[0] = '\0';
    if (try_full_nonroot_parent_split(table,
                                      schema,
                                      values,
                                      value_count,
                                      &applicable,
                                      overflow_message,
                                      sizeof(overflow_message))) {
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    if (applicable && overflow_message[0] != '\0') {
        set_message(message, message_size, overflow_message);
    } else if (base_message[0] != '\0') {
        set_message(message, message_size, base_message);
    } else if (overflow_message[0] != '\0') {
        set_message(message, message_size, overflow_message);
    } else {
        set_message(message,
                    message_size,
                    "generic insert is not supported for this tree topology");
    }
    return false;
}
