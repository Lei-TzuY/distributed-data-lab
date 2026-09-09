#include "generic_index_epoch.h"
#include "internal_root_split_stage.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_tree_split_stage.h"
#include "slotted_leaf_v2_tree_split_tail_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool tinydb_record_insert_mixed_base(Table* table,
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

static void begin_root_scope(Table* table,
                             const TableSchema* schema,
                             uint32_t* previous_root) {
    *previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
}

static void end_root_scope(Table* table, uint32_t previous_root) {
    table->root_page_num = previous_root;
}

static uint32_t read_u32_native(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void write_u32_native(unsigned char* bytes, uint32_t value) {
    memcpy(bytes, &value, sizeof(value));
}

static bool peek_unused_page_nums(const Pager* pager,
                                  uint32_t* page_nums,
                                  uint32_t count) {
    if (pager == NULL || page_nums == NULL || count == 0u) return false;

    uint32_t free_count = pager->free_page_count;
    uint32_t appended = count > free_count ? count - free_count : 0u;
    if (appended > 0u &&
        pager->num_pages > INVALID_PAGE_NUM - appended) {
        return false;
    }

    for (uint32_t i = 0u; i < count; i++) {
        if (i < free_count) {
            page_nums[i] = pager->free_pages[free_count - 1u - i];
        } else {
            page_nums[i] = pager->num_pages + (i - free_count);
        }
        if (page_nums[i] == 0u || page_nums[i] == INVALID_PAGE_NUM) return false;
        for (uint32_t j = 0u; j < i; j++) {
            if (page_nums[j] == page_nums[i]) return false;
        }
    }
    return true;
}

static bool claim_reserved_page_nums(Pager* pager,
                                     const uint32_t* page_nums,
                                     uint32_t count) {
    if (pager == NULL || page_nums == NULL || count == 0u) return false;
    for (uint32_t i = 0u; i < count; i++) {
        uint32_t claimed = get_unused_page_num(pager);
        if (claimed != page_nums[i]) return false;
        (void)get_page(pager, claimed);
    }
    return true;
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
    if (!tinydb_record_encode(schema,
                              values,
                              value_count,
                              &record,
                              message,
                              message_size)) {
        return false;
    }

    TinyDBRecordPayload payload;
    if (!tinydb_record_payload_from_record(schema,
                                           &record,
                                           &payload,
                                           message,
                                           message_size)) {
        return false;
    }
    if (payload.length < sizeof(uint32_t)) {
        set_message(message,
                    message_size,
                    "slotted V2 split INSERT requires an encoded primary key");
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
                    "unable to encode compact V2 row for split INSERT");
        return false;
    }
    return true;
}

static bool previous_boundary_allows(Table* table,
                                     uint32_t previous_page_num,
                                     uint32_t key) {
    if (previous_page_num == 0u) return true;
    if (previous_page_num >= table->pager->num_pages) return false;

    unsigned char previous_page[PAGE_SIZE];
    memcpy(previous_page,
           get_page(table->pager, previous_page_num),
           sizeof(previous_page));
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_leaf_page_count(previous_page, PAGE_SIZE, &count) &&
           count > 0u &&
           tinydb_leaf_page_key_at(previous_page,
                                   PAGE_SIZE,
                                   count - 1u,
                                   &max_key) &&
           key > max_key;
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

static bool staged_internal_parent_for_child(
    const unsigned char left_internal[PAGE_SIZE],
    uint32_t left_internal_page_num,
    const unsigned char right_internal[PAGE_SIZE],
    uint32_t right_internal_page_num,
    uint32_t child_page_num,
    uint32_t* parent_page_num) {
    if (parent_page_num == NULL || child_page_num == 0u) return false;

    uint32_t left_keys = tinydb_parent_stage_read_u32(
        left_internal + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= left_keys; i++) {
        if (tinydb_parent_stage_child_at(left_internal, i) == child_page_num) {
            *parent_page_num = left_internal_page_num;
            return true;
        }
    }

    uint32_t right_keys = tinydb_parent_stage_read_u32(
        right_internal + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= right_keys; i++) {
        if (tinydb_parent_stage_child_at(right_internal, i) == child_page_num) {
            *parent_page_num = right_internal_page_num;
            return true;
        }
    }
    return false;
}

static bool validate_full_root_descendants(
    Table* table,
    uint32_t old_root_page_num,
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
                child_page_num == old_root_page_num) {
                return false;
            }
            unsigned char* child =
                (unsigned char*)get_page(table->pager, child_page_num);
            if (child == NULL || get_node_type(child) != NODE_LEAF ||
                child[IS_ROOT_OFFSET] != 0u ||
                read_u32_native(child + PARENT_POINTER_OFFSET) !=
                    old_root_page_num) {
                return false;
            }
        }
    }
    return true;
}

static void reparent_full_root_descendants(
    Table* table,
    uint32_t split_left_page_num,
    uint32_t split_right_page_num,
    uint32_t next_page_num,
    uint32_t left_internal_page_num,
    const unsigned char left_internal[PAGE_SIZE],
    uint32_t right_internal_page_num,
    const unsigned char right_internal[PAGE_SIZE]) {
    const unsigned char* nodes[2] = {left_internal, right_internal};
    const uint32_t parents[2] = {
        left_internal_page_num,
        right_internal_page_num
    };

    for (uint32_t n = 0u; n < 2u; n++) {
        uint32_t keys = tinydb_parent_stage_read_u32(
            nodes[n] + INTERNAL_NODE_NUM_KEYS_OFFSET);
        for (uint32_t i = 0u; i <= keys; i++) {
            uint32_t child_page_num = tinydb_parent_stage_child_at(nodes[n], i);
            if (child_page_num == split_left_page_num ||
                child_page_num == split_right_page_num ||
                child_page_num == next_page_num) {
                continue;
            }
            unsigned char* child =
                (unsigned char*)get_page(table->pager, child_page_num);
            write_u32_native(child + PARENT_POINTER_OFFSET, parents[n]);
            mark_page_dirty(table->pager, child_page_num);
        }
    }
}

static bool split_insert_slotted_v2(Table* table,
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

    uint32_t previous_root = 0u;
    begin_root_scope(table, schema, &previous_root);
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    if (cursor == NULL || cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        end_root_scope(table, previous_root);
        return false;
    }

    uint32_t left_page_num = cursor->page_num;
    unsigned char left_before[PAGE_SIZE];
    memcpy(left_before,
           get_page(table->pager, left_page_num),
           sizeof(left_before));

    uint32_t count = 0u;
    uint32_t found_key = 0u;
    if (tinydb_leaf_format_detect_page(left_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(left_before, PAGE_SIZE) ||
        !tinydb_leaf_page_count(left_before, PAGE_SIZE, &count) || count < 2u) {
        free(cursor);
        end_root_scope(table, previous_root);
        return false;
    }

    if (cursor->cell_num < count &&
        tinydb_leaf_page_key_at(left_before,
                                PAGE_SIZE,
                                cursor->cell_num,
                                &found_key) &&
        found_key == key) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message, message_size, "duplicate primary key");
        return false;
    }

    uint32_t old_left_max = 0u;
    if (!tinydb_leaf_page_key_at(left_before,
                                 PAGE_SIZE,
                                 count - 1u,
                                 &old_left_max)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 split INSERT could not read the child upper boundary");
        return false;
    }

    uint32_t previous_page_num = 0u;
    uint32_t next_page_num = 0u;
    if (!tinydb_leaf_page_prev(left_before,
                               PAGE_SIZE,
                               &previous_page_num) ||
        !tinydb_leaf_page_next(left_before, PAGE_SIZE, &next_page_num) ||
        next_page_num == left_page_num ||
        (next_page_num != 0u && next_page_num >= table->pager->num_pages) ||
        !previous_boundary_allows(table, previous_page_num, key)) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 split INSERT requires a valid sibling range");
        return false;
    }

    bool is_tail = next_page_num == 0u;
    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length;
    if (required <= tinydb_slotted_leaf_v2_free_bytes(left_before, PAGE_SIZE)) {
        if (!is_tail || key <= old_left_max) {
            free(cursor);
            end_root_scope(table, previous_root);
            return false;
        }

        if (applicable != NULL) *applicable = true;
        if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "unable to persist generic-index mutation epoch");
            return false;
        }

        void* page = get_page(table->pager, left_page_num);
        unsigned char before[PAGE_USABLE_SIZE];
        memcpy(before, page, sizeof(before));
        if (!tinydb_slotted_leaf_v2_insert(page,
                                           PAGE_SIZE,
                                           key,
                                           envelope,
                                           (uint16_t)envelope_length) ||
            !tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE)) {
            memcpy(page, before, sizeof(before));
            free(cursor);
            end_root_scope(table, previous_root);
            set_message(message,
                        message_size,
                        "slotted V2 tail append failed without publishing a partial page");
            return false;
        }

        mark_page_dirty(table->pager, left_page_num);
        free(cursor);
        end_root_scope(table, previous_root);
        if (!table->in_transaction) pager_commit(table->pager);
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }
    if (applicable != NULL) *applicable = true;

    if (left_before[IS_ROOT_OFFSET] != 0u) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 root-leaf split INSERT remains fail-closed");
        return false;
    }

    if (!is_tail && key >= old_left_max) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "non-tail slotted V2 split INSERT must preserve the existing child upper boundary");
        return false;
    }

    uint32_t parent_page_num = read_u32_native(
        left_before + PARENT_POINTER_OFFSET);
    if (parent_page_num == 0u || parent_page_num >= table->pager->num_pages ||
        parent_page_num == left_page_num || parent_page_num == next_page_num) {
        free(cursor);
        end_root_scope(table, previous_root);
        set_message(message,
                    message_size,
                    "slotted V2 split INSERT requires an existing parent page");
        return false;
    }

    free(cursor);
    end_root_scope(table, previous_root);

    unsigned char next_before[PAGE_SIZE];
    unsigned char parent_before[PAGE_SIZE];
    memset(next_before, 0, sizeof(next_before));
    if (!is_tail) {
        memcpy(next_before,
               get_page(table->pager, next_page_num),
               sizeof(next_before));
    }
    memcpy(parent_before,
           get_page(table->pager, parent_page_num),
           sizeof(parent_before));
    if (get_node_type(parent_before) != NODE_INTERNAL ||
        (!is_tail &&
         (tinydb_leaf_format_detect_page(next_before, PAGE_SIZE) !=
              TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
          !tinydb_slotted_leaf_v2_validate(next_before, PAGE_SIZE)))) {
        set_message(message,
                    message_size,
                    "production V2 split requires an internal parent and a valid V2 next sibling when non-tail");
        return false;
    }

    uint32_t parent_keys = read_u32_native(
        parent_before + INTERNAL_NODE_NUM_KEYS_OFFSET);
    if (parent_keys > INTERNAL_NODE_MAX_KEYS) {
        set_message(message,
                    message_size,
                    "corrupt internal parent key count blocks slotted V2 split");
        return false;
    }
    bool full_root_parent = parent_keys == INTERNAL_NODE_MAX_KEYS &&
                            parent_before[IS_ROOT_OFFSET] != 0u &&
                            parent_page_num == schema->root_page_num;
    if (parent_keys == INTERNAL_NODE_MAX_KEYS && !full_root_parent) {
        set_message(message,
                    message_size,
                    "slotted V2 recursive non-root internal-node split remains fail-closed");
        return false;
    }

    uint32_t reserved_pages[3] = {0u, 0u, 0u};
    uint32_t reservation_count = full_root_parent ? 3u : 1u;
    if (!peek_unused_page_nums(table->pager,
                               reserved_pages,
                               reservation_count)) {
        set_message(message,
                    message_size,
                    "unable to reserve page numbers for slotted V2 split");
        return false;
    }
    uint32_t right_page_num = reserved_pages[0];
    uint32_t left_internal_page_num = full_root_parent ? reserved_pages[1] : 0u;
    uint32_t right_internal_page_num = full_root_parent ? reserved_pages[2] : 0u;
    if (right_page_num == left_page_num || right_page_num == next_page_num ||
        right_page_num == parent_page_num ||
        (full_root_parent &&
         (left_internal_page_num == left_page_num ||
          left_internal_page_num == next_page_num ||
          left_internal_page_num == parent_page_num ||
          right_internal_page_num == left_page_num ||
          right_internal_page_num == next_page_num ||
          right_internal_page_num == parent_page_num))) {
        set_message(message,
                    message_size,
                    "slotted V2 split page reservation collided with existing topology");
        return false;
    }

    unsigned char left_after[PAGE_SIZE];
    unsigned char right_after[PAGE_SIZE];
    unsigned char next_after[PAGE_SIZE];
    unsigned char parent_after[PAGE_SIZE];
    unsigned char left_internal_after[PAGE_SIZE];
    unsigned char right_internal_after[PAGE_SIZE];
    memcpy(left_after, left_before, sizeof(left_after));
    memset(right_after, 0, sizeof(right_after));
    memset(next_after, 0, sizeof(next_after));
    if (!is_tail) memcpy(next_after, next_before, sizeof(next_after));
    memcpy(parent_after, parent_before, sizeof(parent_after));
    memset(left_internal_after, 0, sizeof(left_internal_after));
    memset(right_internal_after, 0, sizeof(right_internal_after));

    bool staged = false;
    if (full_root_parent) {
        staged = tinydb_slotted_leaf_v2_split_nonroot(
            left_after,
            PAGE_SIZE,
            left_page_num,
            right_after,
            PAGE_SIZE,
            right_page_num,
            NULL);
        if (staged && !is_tail) {
            uint32_t next_prev = 0u;
            staged = tinydb_leaf_page_prev(next_after,
                                            PAGE_SIZE,
                                            &next_prev) &&
                     next_prev == left_page_num;
            if (staged) {
                tinydb_slotted_split_write_u32(
                    next_after + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
                    right_page_num);
                staged = tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE);
            }
        }

        uint16_t staged_left_count =
            tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
        uint16_t staged_right_count =
            tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
        uint32_t staged_left_max = 0u;
        uint32_t staged_right_max = 0u;
        staged = staged && staged_left_count > 0u && staged_right_count > 0u &&
                 tinydb_leaf_page_key_at(left_after,
                                         PAGE_SIZE,
                                         (uint32_t)staged_left_count - 1u,
                                         &staged_left_max) &&
                 tinydb_leaf_page_key_at(right_after,
                                         PAGE_SIZE,
                                         (uint32_t)staged_right_count - 1u,
                                         &staged_right_max) &&
                 staged_right_max == old_left_max;
        if (staged) {
            staged = tinydb_stage_full_root_after_child_split(
                parent_after,
                PAGE_SIZE,
                parent_page_num,
                left_internal_after,
                PAGE_SIZE,
                left_internal_page_num,
                right_internal_after,
                PAGE_SIZE,
                right_internal_page_num,
                left_page_num,
                right_page_num,
                old_left_max,
                staged_left_max,
                staged_right_max,
                NULL);
        }
        if (staged) {
            staged = validate_full_root_descendants(table,
                                                    parent_page_num,
                                                    right_page_num,
                                                    left_internal_after,
                                                    right_internal_after);
        }
        if (staged) {
            uint32_t left_parent = 0u;
            uint32_t right_parent = 0u;
            uint32_t next_parent = 0u;
            staged = staged_internal_parent_for_child(
                         left_internal_after,
                         left_internal_page_num,
                         right_internal_after,
                         right_internal_page_num,
                         left_page_num,
                         &left_parent) &&
                     staged_internal_parent_for_child(
                         left_internal_after,
                         left_internal_page_num,
                         right_internal_after,
                         right_internal_page_num,
                         right_page_num,
                         &right_parent);
            if (staged && !is_tail) {
                staged = staged_internal_parent_for_child(
                    left_internal_after,
                    left_internal_page_num,
                    right_internal_after,
                    right_internal_page_num,
                    next_page_num,
                    &next_parent);
            }
            if (staged) {
                write_u32_native(left_after + PARENT_POINTER_OFFSET, left_parent);
                write_u32_native(right_after + PARENT_POINTER_OFFSET, right_parent);
                if (!is_tail) {
                    write_u32_native(next_after + PARENT_POINTER_OFFSET, next_parent);
                }
            }
        }
    } else {
        staged = is_tail
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
    }
    if (!staged) {
        set_message(message,
                    message_size,
                    full_root_parent
                        ? "full-root slotted V2 split staging rejected inconsistent topology"
                        : "slotted V2 split staging rejected parent overflow or inconsistent topology");
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
                    "slotted V2 split staging changed the existing child upper boundary");
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
        (!is_tail && !tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE))) {
        set_message(message,
                    message_size,
                    "byte-balanced V2 split did not leave enough space for the inserted row");
        return false;
    }

    uint32_t checked_left_max = 0u;
    uint32_t checked_right_max = 0u;
    left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    right_count = tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
    if (!tinydb_leaf_page_key_at(left_after,
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
                    "slotted V2 split INSERT would invalidate staged parent separators");
        return false;
    }

    if (full_root_parent && is_tail) {
        uint32_t right_parent = 0u;
        uint32_t right_parent_keys = tinydb_parent_stage_read_u32(
            right_internal_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
        if (!staged_internal_parent_for_child(left_internal_after,
                                              left_internal_page_num,
                                              right_internal_after,
                                              right_internal_page_num,
                                              right_page_num,
                                              &right_parent) ||
            right_parent != right_internal_page_num ||
            tinydb_parent_stage_child_at(right_internal_after,
                                         right_parent_keys) !=
                right_page_num) {
            set_message(message,
                        message_size,
                        "full-root tail split did not keep the growing leaf rightmost");
            return false;
        }
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }

    if (!claim_reserved_page_nums(table->pager,
                                  reserved_pages,
                                  reservation_count)) {
        set_message(message,
                    message_size,
                    "slotted V2 split page reservation changed before publication");
        return false;
    }

    if (full_root_parent) {
        reparent_full_root_descendants(table,
                                       left_page_num,
                                       right_page_num,
                                       next_page_num,
                                       left_internal_page_num,
                                       left_internal_after,
                                       right_internal_page_num,
                                       right_internal_after);
    }

    if (!publish_page(table->pager, left_page_num, left_after) ||
        !publish_page(table->pager, right_page_num, right_after) ||
        (!is_tail && !publish_page(table->pager, next_page_num, next_after)) ||
        (full_root_parent &&
         (!publish_page(table->pager,
                        left_internal_page_num,
                        left_internal_after) ||
          !publish_page(table->pager,
                        right_internal_page_num,
                        right_internal_after))) ||
        !publish_page(table->pager, parent_page_num, parent_after)) {
        set_message(message,
                    message_size,
                    "unable to publish staged slotted V2 split pages");
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
    if (tinydb_record_insert_mixed_base(table,
                                        schema,
                                        values,
                                        value_count,
                                        base_message,
                                        sizeof(base_message))) {
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    bool applicable = false;
    char split_message[TINYDB_RECORD_MESSAGE_MAX];
    split_message[0] = '\0';
    if (split_insert_slotted_v2(table,
                                schema,
                                values,
                                value_count,
                                &applicable,
                                split_message,
                                sizeof(split_message))) {
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    if (applicable && split_message[0] != '\0') {
        set_message(message, message_size, split_message);
    } else if (base_message[0] != '\0') {
        set_message(message, message_size, base_message);
    } else if (split_message[0] != '\0') {
        set_message(message, message_size, split_message);
    } else {
        set_message(message,
                    message_size,
                    "generic insert is not supported for this leaf layout");
    }
    return false;
}