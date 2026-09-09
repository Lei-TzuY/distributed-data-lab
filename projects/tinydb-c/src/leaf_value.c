#include "leaf_value.h"
#include "leaf_format.h"
#include "leaf_mutation_policy.h"
#include "leaf_page_access.h"

#include <stddef.h>
#include <string.h>

bool tinydb_leaf_value_legacy_layout_compatible(void) {
    const TinyDBLeafFormatDescriptor* format = tinydb_leaf_format_current();
    return tinydb_leaf_format_validate_current() &&
           format->format_version == TINYDB_LEAF_FORMAT_FIXED_V1 &&
           format->value_capacity == ROW_SIZE &&
           offsetof(Row, id) == ID_OFFSET &&
           offsetof(Row, username) == USERNAME_OFFSET &&
           offsetof(Row, email) == EMAIL_OFFSET &&
           sizeof(Row) >= format->value_capacity;
}

static bool valid_cursor(const Cursor* cursor) {
    return cursor != NULL &&
           cursor->table != NULL &&
           cursor->table->pager != NULL &&
           cursor->page_num < cursor->table->pager->num_pages;
}

static bool valid_args(Cursor* cursor,
                       const void* bytes,
                       uint32_t length) {
    return valid_cursor(cursor) &&
           bytes != NULL &&
           tinydb_leaf_format_can_store_value(length);
}

static bool cursor_is_fixed_v1(Cursor* cursor) {
    if (!valid_cursor(cursor)) return false;
    void* page = get_page(cursor->table->pager, cursor->page_num);
    return tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE);
}

static bool tree_is_mutable_v1(Cursor* cursor) {
    if (!valid_cursor(cursor)) return false;
    char message[128];
    return tinydb_leaf_tree_mutation_supported(cursor->table,
                                               cursor->table->root_page_num,
                                               message,
                                               sizeof(message));
}

bool tinydb_leaf_value_insert(Cursor* cursor,
                              uint32_t key,
                              const void* bytes,
                              uint32_t length) {
    if (!valid_args(cursor, bytes, length) ||
        !tinydb_leaf_value_legacy_layout_compatible() ||
        !tree_is_mutable_v1(cursor) ||
        !cursor_is_fixed_v1(cursor)) {
        return false;
    }

    /* The legacy leaf API accepts Row only because its serializer predates
     * schema-aware records. The explicit compatibility predicate above makes
     * this temporary carrier fail closed if the compiler ever lays Row out
     * differently from the canonical serialized legacy value slot. */
    Row carrier;
    memset(&carrier, 0, sizeof(carrier));
    memcpy(&carrier, bytes, length);
    leaf_node_insert(cursor, key, &carrier);
    return true;
}

bool tinydb_leaf_value_write(Cursor* cursor,
                             const void* bytes,
                             uint32_t length) {
    if (!valid_args(cursor, bytes, length) ||
        !tree_is_mutable_v1(cursor) ||
        !cursor_is_fixed_v1(cursor)) {
        return false;
    }

    const TinyDBLeafFormatDescriptor* format = tinydb_leaf_format_current();
    void* slot = cursor_value(cursor);
    if (slot == NULL) return false;
    memset(slot, 0, format->value_capacity);
    memcpy(slot, bytes, length);
    mark_page_dirty(cursor->table->pager, cursor->page_num);
    return true;
}

bool tinydb_leaf_value_read(Cursor* cursor,
                            void* output,
                            size_t output_capacity,
                            uint32_t length) {
    if (!valid_cursor(cursor) || output == NULL || length == 0u ||
        output_capacity < length) {
        return false;
    }

    void* page = get_page(cursor->table->pager, cursor->page_num);
    const void* value = NULL;
    uint32_t stored_length = 0u;
    if (!tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &value,
                                   &stored_length) ||
        stored_length < length) {
        return false;
    }

    memcpy(output, value, length);
    return true;
}
