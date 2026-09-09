#ifndef TINYDB_RECORD_DELETE_V2_EMPTY_LEAF_H
#define TINYDB_RECORD_DELETE_V2_EMPTY_LEAF_H

#include "generic_index_epoch.h"
#include "internal_child_remove_stage.h"
#include "internal_max_decrease_cascade_stage.h"
#include "leaf_page_access.h"
#include "leaf_sibling_relink_stage.h"
#include "slotted_leaf_v2.h"
#include "slotted_v2_publish_batch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char* pages;
    uint32_t* page_nums;
    uint32_t count;
    uint32_t capacity;
} TinyDBEmptyLeafAncestorPath;

static inline void tinydb_empty_leaf_set_message(char* message,
                                                  size_t message_size,
                                                  const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static inline uint32_t tinydb_empty_leaf_read_u32(const unsigned char* p) {
    uint32_t value = 0u;
    memcpy(&value, p, sizeof(value));
    return value;
}

static inline void tinydb_empty_leaf_path_free(TinyDBEmptyLeafAncestorPath* path) {
    if (path == NULL) return;
    free(path->pages);
    free(path->page_nums);
    memset(path, 0, sizeof(*path));
}

static inline bool tinydb_empty_leaf_path_reserve(
    TinyDBEmptyLeafAncestorPath* path,
    uint32_t capacity) {
    if (path == NULL) return false;
    if (capacity <= path->capacity) return true;

    size_t page_bytes = (size_t)capacity * PAGE_SIZE;
    size_t num_bytes = (size_t)capacity * sizeof(uint32_t);
    if (page_bytes / PAGE_SIZE != (size_t)capacity ||
        num_bytes / sizeof(uint32_t) != (size_t)capacity) {
        return false;
    }

    unsigned char* pages = (unsigned char*)realloc(path->pages, page_bytes);
    if (pages == NULL) return false;
    path->pages = pages;
    uint32_t* page_nums = (uint32_t*)realloc(path->page_nums, num_bytes);
    if (page_nums == NULL) return false;
    path->page_nums = page_nums;
    path->capacity = capacity;
    return true;
}

static inline bool tinydb_empty_leaf_collect_ancestors(
    Table* table,
    const TableSchema* schema,
    uint32_t leaf_page_num,
    const unsigned char leaf_page[PAGE_SIZE],
    TinyDBEmptyLeafAncestorPath* path) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        leaf_page == NULL || path == NULL || leaf_page_num == schema->root_page_num) {
        return false;
    }
    memset(path, 0, sizeof(*path));

    uint32_t current_page_num = tinydb_empty_leaf_read_u32(
        leaf_page + PARENT_POINTER_OFFSET);
    if (current_page_num == INVALID_PAGE_NUM ||
        current_page_num >= table->pager->num_pages ||
        current_page_num == leaf_page_num ||
        (current_page_num == 0u && schema->root_page_num != 0u)) {
        return false;
    }

    while (true) {
        if (path->count == path->capacity) {
            uint32_t next_capacity = path->capacity == 0u
                ? 4u
                : path->capacity * 2u;
            if (next_capacity <= path->capacity ||
                !tinydb_empty_leaf_path_reserve(path, next_capacity)) {
                tinydb_empty_leaf_path_free(path);
                return false;
            }
        }
        for (uint32_t i = 0u; i < path->count; i++) {
            if (path->page_nums[i] == current_page_num) {
                tinydb_empty_leaf_path_free(path);
                return false;
            }
        }

        unsigned char* destination =
            path->pages + (size_t)path->count * PAGE_SIZE;
        memcpy(destination, get_page(table->pager, current_page_num), PAGE_SIZE);
        if (!tinydb_parent_stage_validate(destination, PAGE_SIZE)) {
            tinydb_empty_leaf_path_free(path);
            return false;
        }
        path->page_nums[path->count] = current_page_num;
        path->count++;

        bool is_root = destination[IS_ROOT_OFFSET] != 0u;
        if (current_page_num == schema->root_page_num) {
            if (!is_root ||
                tinydb_empty_leaf_read_u32(destination + PARENT_POINTER_OFFSET) != 0u) {
                tinydb_empty_leaf_path_free(path);
                return false;
            }
            return true;
        }
        if (is_root) {
            tinydb_empty_leaf_path_free(path);
            return false;
        }

        uint32_t next_page_num = tinydb_empty_leaf_read_u32(
            destination + PARENT_POINTER_OFFSET);
        if (next_page_num == INVALID_PAGE_NUM ||
            next_page_num >= table->pager->num_pages ||
            next_page_num == current_page_num ||
            (next_page_num == 0u && schema->root_page_num != 0u)) {
            tinydb_empty_leaf_path_free(path);
            return false;
        }
        current_page_num = next_page_num;
    }
}

static inline bool tinydb_empty_leaf_page_is_free(const Pager* pager,
                                                   uint32_t page_num) {
    if (pager == NULL) return true;
    for (uint32_t i = 0u; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static inline bool tinydb_empty_leaf_neighbor_guard(
    const unsigned char page[PAGE_SIZE],
    uint32_t removed_key,
    bool previous) {
    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count) || count == 0u) {
        return false;
    }
    uint32_t boundary = 0u;
    uint32_t index = previous ? count - 1u : 0u;
    if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, index, &boundary)) return false;
    return previous ? boundary < removed_key : boundary > removed_key;
}

static inline bool tinydb_delete_v2_empty_leaf(
    Table* table,
    const TableSchema* schema,
    uint32_t leaf_page_num,
    const unsigned char leaf_before[PAGE_SIZE],
    uint32_t removed_key,
    char* message,
    size_t message_size) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        leaf_before == NULL || leaf_page_num == 0u ||
        leaf_page_num == INVALID_PAGE_NUM ||
        leaf_page_num >= table->pager->num_pages ||
        leaf_page_num == schema->root_page_num ||
        tinydb_empty_leaf_page_is_free(table->pager, leaf_page_num) ||
        tinydb_leaf_format_detect_page(leaf_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(leaf_before, PAGE_SIZE)) {
        tinydb_empty_leaf_set_message(message,
                                      message_size,
                                      "slotted V2 empty-leaf removal requires a live non-root V2 leaf");
        return false;
    }

    uint32_t leaf_count = 0u;
    uint32_t only_key = 0u;
    uint32_t previous_page_num = 0u;
    uint32_t next_page_num = 0u;
    if (!tinydb_leaf_page_count(leaf_before, PAGE_SIZE, &leaf_count) ||
        leaf_count != 1u ||
        !tinydb_leaf_page_key_at(leaf_before, PAGE_SIZE, 0u, &only_key) ||
        only_key != removed_key ||
        !tinydb_leaf_page_prev(leaf_before, PAGE_SIZE, &previous_page_num) ||
        !tinydb_leaf_page_next(leaf_before, PAGE_SIZE, &next_page_num) ||
        previous_page_num == leaf_page_num || next_page_num == leaf_page_num ||
        previous_page_num == INVALID_PAGE_NUM ||
        next_page_num == INVALID_PAGE_NUM ||
        (previous_page_num == 0u && next_page_num == 0u) ||
        (previous_page_num != 0u && previous_page_num >= table->pager->num_pages) ||
        (next_page_num != 0u && next_page_num >= table->pager->num_pages) ||
        (previous_page_num != 0u && previous_page_num == next_page_num)) {
        tinydb_empty_leaf_set_message(message,
                                      message_size,
                                      "slotted V2 empty-leaf removal requires consistent sibling topology");
        return false;
    }

    TinyDBEmptyLeafAncestorPath ancestors;
    memset(&ancestors, 0, sizeof(ancestors));
    if (!tinydb_empty_leaf_collect_ancestors(table,
                                             schema,
                                             leaf_page_num,
                                             leaf_before,
                                             &ancestors) ||
        ancestors.count == 0u) {
        tinydb_empty_leaf_path_free(&ancestors);
        tinydb_empty_leaf_set_message(message,
                                      message_size,
                                      "slotted V2 empty-leaf removal could not collect the ancestor chain");
        return false;
    }

    uint32_t parent_page_num = ancestors.page_nums[0];
    unsigned char* parent_after = ancestors.pages;
    uint32_t removed_index = UINT32_MAX;
    bool parent_max_changed = false;
    uint32_t new_parent_max = 0u;
    if (!tinydb_stage_internal_child_remove(parent_after,
                                             PAGE_SIZE,
                                             leaf_page_num,
                                             removed_key,
                                             &removed_index,
                                             &parent_max_changed,
                                             &new_parent_max)) {
        tinydb_empty_leaf_path_free(&ancestors);
        tinydb_empty_leaf_set_message(message,
                                      message_size,
                                      "slotted V2 empty-leaf removal would underflow or corrupt its parent");
        return false;
    }

    bool ancestor_changed = false;
    uint32_t changed_ancestor_index = UINT32_MAX;
    if (parent_max_changed && parent_page_num != schema->root_page_num) {
        if (ancestors.count < 2u) {
            tinydb_empty_leaf_path_free(&ancestors);
            tinydb_empty_leaf_set_message(message,
                                          message_size,
                                          "slotted V2 empty-leaf removal is missing a parent ancestor");
            return false;
        }
        uint32_t stop_level = UINT32_MAX;
        bool cascade_changed = false;
        if (!tinydb_stage_internal_max_decrease_cascade(
                ancestors.pages + PAGE_SIZE,
                PAGE_SIZE,
                ancestors.page_nums + 1u,
                ancestors.count - 1u,
                parent_page_num,
                removed_key,
                new_parent_max,
                &stop_level,
                &cascade_changed) ||
            stop_level == UINT32_MAX ||
            stop_level >= ancestors.count - 1u) {
            tinydb_empty_leaf_path_free(&ancestors);
            tinydb_empty_leaf_set_message(message,
                                          message_size,
                                          "slotted V2 empty-leaf removal could not propagate the new parent maximum");
            return false;
        }
        ancestor_changed = cascade_changed;
        if (ancestor_changed) changed_ancestor_index = stop_level + 1u;
    }

    unsigned char previous_after[PAGE_SIZE];
    unsigned char next_after[PAGE_SIZE];
    bool have_previous = previous_page_num != 0u;
    bool have_next = next_page_num != 0u;
    if (have_previous) {
        memcpy(previous_after,
               get_page(table->pager, previous_page_num),
               PAGE_SIZE);
        uint32_t reciprocal_next = INVALID_PAGE_NUM;
        if (!tinydb_empty_leaf_neighbor_guard(previous_after, removed_key, true) ||
            !tinydb_leaf_page_next(previous_after,
                                   PAGE_SIZE,
                                   &reciprocal_next) ||
            reciprocal_next != leaf_page_num ||
            !tinydb_stage_leaf_sibling_relink(previous_after,
                                              PAGE_SIZE,
                                              true,
                                              leaf_page_num,
                                              next_page_num)) {
            tinydb_empty_leaf_path_free(&ancestors);
            tinydb_empty_leaf_set_message(message,
                                          message_size,
                                          "slotted V2 empty-leaf removal found an invalid previous sibling backlink");
            return false;
        }
    }
    if (have_next) {
        memcpy(next_after, get_page(table->pager, next_page_num), PAGE_SIZE);
        uint32_t reciprocal_previous = INVALID_PAGE_NUM;
        if (!tinydb_empty_leaf_neighbor_guard(next_after, removed_key, false) ||
            !tinydb_leaf_page_prev(next_after,
                                   PAGE_SIZE,
                                   &reciprocal_previous) ||
            reciprocal_previous != leaf_page_num ||
            !tinydb_stage_leaf_sibling_relink(next_after,
                                              PAGE_SIZE,
                                              false,
                                              leaf_page_num,
                                              previous_page_num)) {
            tinydb_empty_leaf_path_free(&ancestors);
            tinydb_empty_leaf_set_message(message,
                                          message_size,
                                          "slotted V2 empty-leaf removal found an invalid next sibling backlink");
            return false;
        }
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_empty_leaf_path_free(&ancestors);
        tinydb_empty_leaf_set_message(message,
                                      message_size,
                                      "unable to persist generic-index mutation epoch");
        return false;
    }

    TinyDBV2PublishEntry entries[4];
    uint32_t entry_count = 0u;
    entries[entry_count].page_num = parent_page_num;
    entries[entry_count].target =
        (unsigned char*)get_page(table->pager, parent_page_num);
    entries[entry_count].staged = parent_after;
    entry_count++;

    if (ancestor_changed) {
        uint32_t page_num = ancestors.page_nums[changed_ancestor_index];
        entries[entry_count].page_num = page_num;
        entries[entry_count].target =
            (unsigned char*)get_page(table->pager, page_num);
        entries[entry_count].staged =
            ancestors.pages + (size_t)changed_ancestor_index * PAGE_SIZE;
        entry_count++;
    }
    if (have_previous) {
        entries[entry_count].page_num = previous_page_num;
        entries[entry_count].target =
            (unsigned char*)get_page(table->pager, previous_page_num);
        entries[entry_count].staged = previous_after;
        entry_count++;
    }
    if (have_next) {
        entries[entry_count].page_num = next_page_num;
        entries[entry_count].target =
            (unsigned char*)get_page(table->pager, next_page_num);
        entries[entry_count].staged = next_after;
        entry_count++;
    }

    if (!tinydb_v2_publish_batch(entries,
                                 entry_count,
                                 TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_empty_leaf_path_free(&ancestors);
        tinydb_empty_leaf_set_message(message,
                                      message_size,
                                      "unable to atomically publish slotted V2 empty-leaf topology removal");
        return false;
    }
    for (uint32_t i = 0u; i < entry_count; i++) {
        mark_page_dirty(table->pager, entries[i].page_num);
    }

    pager_free_page(table->pager, leaf_page_num);
    tinydb_empty_leaf_path_free(&ancestors);

    if (!table->in_transaction) pager_commit(table->pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

#endif
