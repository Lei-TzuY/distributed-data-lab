#include "record_payload_root_internal_split.h"

#include "generic_index_epoch.h"
#include "internal_root_split_stage.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "slotted_leaf_v2.h"
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
    uint32_t next = 0u;
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_leaf_format_detect_page(previous, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(previous, PAGE_SIZE) &&
           tinydb_leaf_page_next(previous, PAGE_SIZE, &next) &&
           next == current_page_num &&
           tinydb_leaf_page_count(previous, PAGE_SIZE, &count) &&
           count > 0u &&
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
           tinydb_leaf_page_count(next_page, PAGE_SIZE, &count) &&
           count > 0u &&
           tinydb_leaf_page_key_at(next_page, PAGE_SIZE, 0u, &min_key) &&
           min_key > current_max_key;
}

static bool peek_unused_page_nums(const Pager* pager,
                                  uint32_t page_nums[3]) {
    if (pager == NULL || page_nums == NULL) return false;
    uint32_t free_count = pager->free_page_count;
    uint32_t appended = free_count >= 3u ? 0u : 3u - free_count;
    if (appended > 0u &&
        pager->num_pages > INVALID_PAGE_NUM - appended) {
        return false;
    }

    for (uint32_t i = 0u; i < 3u; i++) {
        page_nums[i] = i < free_count
            ? pager->free_pages[free_count - 1u - i]
            : pager->num_pages + (i - free_count);
        if (page_nums[i] == 0u || page_nums[i] == INVALID_PAGE_NUM) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (page_nums[j] == page_nums[i]) return false;
        }
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
                                 const uint32_t page_nums[3],
                                 unsigned char* targets[3],
                                 uint32_t* original_num_pages,
                                 uint32_t* original_free_page_count) {
    if (pager == NULL || page_nums == NULL || targets == NULL ||
        original_num_pages == NULL || original_free_page_count == NULL) {
        return false;
    }

    *original_num_pages = pager->num_pages;
    *original_free_page_count = pager->free_page_count;
    for (uint32_t i = 0u; i < 3u; i++) targets[i] = NULL;

    for (uint32_t i = 0u; i < 3u; i++) {
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

static bool staged_internal_parent_for_child(
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

static bool validate_existing_descendants(
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

static void reparent_existing_descendants(
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

bool tinydb_record_payload_try_full_root_parent_split(
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
                    "payload-native full-root split must preserve a non-tail child maximum");
        return false;
    }

    uint32_t root_page_num = read_u32_native(left_before + PARENT_POINTER_OFFSET);
    free(cursor);
    table->root_page_num = previous_root;
    if (root_page_num != schema->root_page_num ||
        root_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char root_before[PAGE_SIZE];
    memcpy(root_before,
           get_page(table->pager, root_page_num),
           PAGE_SIZE);
    if (get_node_type(root_before) != NODE_INTERNAL ||
        root_before[IS_ROOT_OFFSET] == 0u ||
        !tinydb_parent_stage_validate(root_before, PAGE_SIZE) ||
        read_u32_native(root_before + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS) {
        return false;
    }
    if (applicable != NULL) *applicable = true;

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
                        "invalid V2 next sibling blocks payload-native full-root growth");
            return false;
        }
    }

    uint32_t reserved_pages[3] = {0u, 0u, 0u};
    if (!peek_unused_page_nums(table->pager, reserved_pages)) {
        set_message(message,
                    message_size,
                    "unable to reserve leaf/internal pages for payload-native full-root growth");
        return false;
    }
    uint32_t right_leaf_page_num = reserved_pages[0];
    uint32_t left_internal_page_num = reserved_pages[1];
    uint32_t right_internal_page_num = reserved_pages[2];
    const uint32_t existing_pages[4] = {
        left_page_num,
        next_page_num,
        root_page_num,
        INVALID_PAGE_NUM
    };
    for (uint32_t i = 0u; i < 3u; i++) {
        for (uint32_t j = 0u; j < 3u; j++) {
            if (reserved_pages[i] == existing_pages[j]) {
                set_message(message,
                            message_size,
                            "payload-native full-root reservation collided with existing topology");
                return false;
            }
        }
    }

    unsigned char left_after[PAGE_SIZE];
    unsigned char right_leaf_after[PAGE_SIZE];
    unsigned char next_after[PAGE_SIZE];
    unsigned char root_after[PAGE_SIZE];
    unsigned char left_internal_after[PAGE_SIZE];
    unsigned char right_internal_after[PAGE_SIZE];
    memcpy(left_after, left_before, PAGE_SIZE);
    memset(right_leaf_after, 0, PAGE_SIZE);
    memset(next_after, 0, PAGE_SIZE);
    if (!is_tail) memcpy(next_after, next_before, PAGE_SIZE);
    memcpy(root_after, root_before, PAGE_SIZE);
    memset(left_internal_after, 0, PAGE_SIZE);
    memset(right_internal_after, 0, PAGE_SIZE);

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
    if (staged) {
        staged = tinydb_stage_full_root_after_child_split(
            root_after,
            PAGE_SIZE,
            root_page_num,
            left_internal_after,
            PAGE_SIZE,
            left_internal_page_num,
            right_internal_after,
            PAGE_SIZE,
            right_internal_page_num,
            left_page_num,
            right_leaf_page_num,
            old_left_max,
            staged_left_max,
            staged_right_max,
            NULL);
    }
    if (staged) {
        staged = validate_existing_descendants(table,
                                               root_page_num,
                                               right_leaf_page_num,
                                               left_internal_after,
                                               right_internal_after);
    }

    uint32_t left_leaf_parent = 0u;
    uint32_t right_leaf_parent = 0u;
    uint32_t next_parent = 0u;
    if (staged) {
        staged = staged_internal_parent_for_child(
                     left_internal_after,
                     left_internal_page_num,
                     right_internal_after,
                     right_internal_page_num,
                     left_page_num,
                     &left_leaf_parent) &&
                 staged_internal_parent_for_child(
                     left_internal_after,
                     left_internal_page_num,
                     right_internal_after,
                     right_internal_page_num,
                     right_leaf_page_num,
                     &right_leaf_parent);
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
            write_u32_native(left_after + PARENT_POINTER_OFFSET,
                             left_leaf_parent);
            write_u32_native(right_leaf_after + PARENT_POINTER_OFFSET,
                             right_leaf_parent);
            if (!is_tail) {
                write_u32_native(next_after + PARENT_POINTER_OFFSET,
                                 next_parent);
            }
        }
    }
    if (!staged) {
        set_message(message,
                    message_size,
                    "payload-native full-root staging rejected inconsistent descendants");
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
        !tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(left_internal_after, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(right_internal_after, PAGE_SIZE) ||
        (!is_tail && !tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE))) {
        set_message(message,
                    message_size,
                    "payload-native full-root split leaves could not accept the pending row");
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
                    "pending payload invalidated staged full-root separators");
        return false;
    }

    if (is_tail) {
        uint32_t parent = 0u;
        uint32_t right_keys = tinydb_parent_stage_read_u32(
            right_internal_after + INTERNAL_NODE_NUM_KEYS_OFFSET);
        if (!staged_internal_parent_for_child(
                left_internal_after,
                left_internal_page_num,
                right_internal_after,
                right_internal_page_num,
                right_leaf_page_num,
                &parent) ||
            parent != right_internal_page_num ||
            tinydb_parent_stage_child_at(right_internal_after, right_keys) !=
                right_leaf_page_num) {
            set_message(message,
                        message_size,
                        "payload-native full-root tail split lost the global rightmost leaf");
            return false;
        }
    }

    unsigned char* reserved_targets[3] = {NULL, NULL, NULL};
    uint32_t original_num_pages = 0u;
    uint32_t original_free_page_count = 0u;
    if (!claim_reserved_pages(table->pager,
                              reserved_pages,
                              reserved_targets,
                              &original_num_pages,
                              &original_free_page_count)) {
        set_message(message,
                    message_size,
                    "payload-native full-root page reservation changed before publication");
        return false;
    }

    unsigned char* left_target =
        (unsigned char*)get_page(table->pager, left_page_num);
    unsigned char* next_target = is_tail
        ? NULL
        : (unsigned char*)get_page(table->pager, next_page_num);
    unsigned char* root_target =
        (unsigned char*)get_page(table->pager, root_page_num);
    if (left_target == NULL || root_target == NULL ||
        (!is_tail && next_target == NULL)) {
        restore_allocator_reservation(table->pager,
                                      original_num_pages,
                                      original_free_page_count);
        set_message(message,
                    message_size,
                    "payload-native full-root could not acquire publication targets");
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
        left_internal_page_num, reserved_targets[1], left_internal_after};
    entries[entry_count++] = (TinyDBV2PublishEntry){
        right_internal_page_num, reserved_targets[2], right_internal_after};
    entries[entry_count++] = (TinyDBV2PublishEntry){
        root_page_num, root_target, root_after};

    if (!tinydb_v2_publish_batch(entries,
                                 entry_count,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        restore_allocator_reservation(table->pager,
                                      original_num_pages,
                                      original_free_page_count);
        set_message(message,
                    message_size,
                    "payload-native full-root atomic page publication failed");
        return false;
    }

    /* Publication copies are not durable/no-steal protected until their
     * pages are marked dirty. The descendant reparent walk touches hundreds of
     * leaves for a full root and therefore creates heavy LRU pressure. Mark the
     * complete staged batch dirty before that walk so page zero, the split
     * leaves, and the two new internal pages cannot be evicted as clean frames
     * and silently reloaded from the old on-disk topology. */
    mark_page_dirty(table->pager, left_page_num);
    mark_page_dirty(table->pager, right_leaf_page_num);
    mark_page_dirty(table->pager, left_internal_page_num);
    mark_page_dirty(table->pager, right_internal_page_num);
    mark_page_dirty(table->pager, root_page_num);
    if (!is_tail) mark_page_dirty(table->pager, next_page_num);

    reparent_existing_descendants(table,
                                  left_page_num,
                                  right_leaf_page_num,
                                  next_page_num,
                                  left_internal_page_num,
                                  left_internal_after,
                                  right_internal_page_num,
                                  right_internal_after);

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}
