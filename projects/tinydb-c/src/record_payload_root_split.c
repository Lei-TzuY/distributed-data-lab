#include "record_payload_root_split.h"

#include "generic_index_epoch.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_root_split_stage.h"
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
        if (page_nums[i] == 0u || page_nums[i] == INVALID_PAGE_NUM ||
            (i > 0u && page_nums[i] == page_nums[0])) {
            return false;
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

static bool claim_reserved_page_nums(Pager* pager,
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

bool tinydb_record_payload_try_root_split(
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
    if (cursor == NULL || cursor->page_num != schema->root_page_num) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    unsigned char root_before[PAGE_SIZE];
    memcpy(root_before,
           get_page(table->pager, schema->root_page_num),
           PAGE_SIZE);
    uint32_t count = 0u;
    uint32_t found_key = 0u;
    if (tinydb_leaf_format_detect_page(root_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(root_before, PAGE_SIZE) ||
        root_before[IS_ROOT_OFFSET] == 0u ||
        !tinydb_leaf_page_count(root_before, PAGE_SIZE, &count) || count < 2u) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    if (cursor->cell_num < count &&
        tinydb_leaf_page_key_at(root_before,
                                PAGE_SIZE,
                                cursor->cell_num,
                                &found_key) &&
        found_key == key) {
        free(cursor);
        table->root_page_num = previous_root;
        set_message(message, message_size, "duplicate primary key");
        return false;
    }
    free(cursor);
    table->root_page_num = previous_root;

    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length;
    if (required <= tinydb_slotted_leaf_v2_free_bytes(root_before, PAGE_SIZE)) {
        return false;
    }
    if (applicable != NULL) *applicable = true;

    uint32_t reserved_pages[2] = {0u, 0u};
    if (!peek_unused_page_nums(table->pager, reserved_pages) ||
        reserved_pages[0] == schema->root_page_num ||
        reserved_pages[1] == schema->root_page_num) {
        set_message(message,
                    message_size,
                    "unable to reserve two child pages for payload-native V2 root split");
        return false;
    }

    unsigned char root_after[PAGE_SIZE];
    unsigned char left_after[PAGE_SIZE];
    unsigned char right_after[PAGE_SIZE];
    memcpy(root_after, root_before, PAGE_SIZE);
    memset(left_after, 0, PAGE_SIZE);
    memset(right_after, 0, PAGE_SIZE);
    uint32_t separator = 0u;
    if (!tinydb_slotted_leaf_v2_stage_root_split(
            root_after,
            PAGE_SIZE,
            schema->root_page_num,
            left_after,
            PAGE_SIZE,
            reserved_pages[0],
            right_after,
            PAGE_SIZE,
            reserved_pages[1],
            &separator)) {
        set_message(message,
                    message_size,
                    "payload-native V2 root split staging rejected the root topology");
        return false;
    }

    void* destination = key <= separator ? (void*)left_after
                                         : (void*)right_after;
    if (!tinydb_slotted_leaf_v2_insert(destination,
                                       PAGE_SIZE,
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length) ||
        !tinydb_slotted_leaf_v2_validate(left_after, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right_after, PAGE_SIZE) ||
        !tinydb_parent_stage_validate(root_after, PAGE_SIZE)) {
        set_message(message,
                    message_size,
                    "payload-native V2 root split leaves could not accept the pending row");
        return false;
    }

    uint16_t left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint32_t checked_left_max = 0u;
    if (left_count == 0u ||
        !tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &checked_left_max) ||
        checked_left_max != separator ||
        tinydb_parent_stage_key_at(root_after, 0u) != separator ||
        tinydb_parent_stage_child_at(root_after, 0u) != reserved_pages[0] ||
        tinydb_parent_stage_child_at(root_after, 1u) != reserved_pages[1]) {
        set_message(message,
                    message_size,
                    "pending payload invalidated the staged V2 root separator");
        return false;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }

    unsigned char* child_targets[2] = {NULL, NULL};
    uint32_t original_num_pages = 0u;
    uint32_t original_free_page_count = 0u;
    if (!claim_reserved_page_nums(table->pager,
                                  reserved_pages,
                                  child_targets,
                                  &original_num_pages,
                                  &original_free_page_count)) {
        set_message(message,
                    message_size,
                    "payload-native V2 root split page reservation changed before publication");
        return false;
    }

    unsigned char* root_target =
        (unsigned char*)get_page(table->pager, schema->root_page_num);
    TinyDBV2PublishEntry entries[3] = {
        {schema->root_page_num, root_target, root_after},
        {reserved_pages[0], child_targets[0], left_after},
        {reserved_pages[1], child_targets[1], right_after},
    };
    if (root_target == NULL ||
        !tinydb_v2_publish_batch(entries,
                                 3u,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        restore_allocator_reservation(table->pager,
                                      original_num_pages,
                                      original_free_page_count);
        set_message(message,
                    message_size,
                    "payload-native V2 root split atomic page publication failed");
        return false;
    }

    mark_page_dirty(table->pager, schema->root_page_num);
    mark_page_dirty(table->pager, reserved_pages[0]);
    mark_page_dirty(table->pager, reserved_pages[1]);

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}
