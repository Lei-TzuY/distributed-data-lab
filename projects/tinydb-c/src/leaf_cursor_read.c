#include "leaf_cursor_read.h"
#include "leaf_page_access.h"

#include <stdlib.h>

Cursor* table_find_v1_base(Table* table, uint32_t key);
Cursor* table_start_v1_base(Table* table);
Cursor* table_end_v1_base(Table* table);
void* cursor_value_v1_base(Cursor* cursor);
void cursor_advance_v1_base(Cursor* cursor);
void cursor_retreat_v1_base(Cursor* cursor);

static Cursor* make_cursor(Table* table,
                           uint32_t page_num,
                           uint32_t cell_num,
                           bool end_of_table) {
    Cursor* cursor = (Cursor*)malloc(sizeof(Cursor));
    if (cursor == NULL) return NULL;
    cursor->table = table;
    cursor->page_num = page_num;
    cursor->cell_num = cell_num;
    cursor->end_of_table = end_of_table;
    return cursor;
}

static bool valid_page_number(const Table* table, uint32_t page_num) {
    return table != NULL && table->pager != NULL &&
           page_num != INVALID_PAGE_NUM && page_num < table->pager->num_pages;
}

static bool leaf_key_at(Table* table,
                        uint32_t page_num,
                        uint32_t cell_num,
                        uint32_t* key) {
    if (!valid_page_number(table, page_num) || key == NULL) return false;
    void* page = get_page(table->pager, page_num);
    return get_node_type(page) == NODE_LEAF &&
           tinydb_leaf_page_key_at(page, PAGE_SIZE, cell_num, key);
}

static bool ordered_forward_transition(Table* table,
                                       uint32_t current_page,
                                       uint32_t current_count,
                                       uint32_t next_page,
                                       uint32_t next_count) {
    if (current_count == 0u || next_count == 0u) return true;
    uint32_t current_last = 0u;
    uint32_t next_first = 0u;
    return leaf_key_at(table, current_page, current_count - 1u, &current_last) &&
           leaf_key_at(table, next_page, 0u, &next_first) &&
           current_last < next_first;
}

static bool ordered_backward_transition(Table* table,
                                        uint32_t current_page,
                                        uint32_t current_count,
                                        uint32_t prev_page,
                                        uint32_t prev_count) {
    if (current_count == 0u || prev_count == 0u) return true;
    uint32_t current_first = 0u;
    uint32_t prev_last = 0u;
    return leaf_key_at(table, current_page, 0u, &current_first) &&
           leaf_key_at(table, prev_page, prev_count - 1u, &prev_last) &&
           prev_last < current_first;
}

static bool reciprocal_forward_transition(Table* table,
                                          uint32_t current_page,
                                          const void* next_page) {
    (void)table;
    uint32_t back_link = 0u;
    return tinydb_leaf_page_prev(next_page, PAGE_SIZE, &back_link) &&
           back_link == current_page;
}

static bool reciprocal_backward_transition(Table* table,
                                           uint32_t current_page,
                                           const void* prev_page) {
    (void)table;
    uint32_t forward_link = 0u;
    return tinydb_leaf_page_next(prev_page, PAGE_SIZE, &forward_link) &&
           forward_link == current_page;
}

/* Validate only invariants encoded in the current internal page image. The
 * Pager has a bounded frame pool, so fetching every child here can evict the
 * very frame backing `node` and invalidate subsequent pointer arithmetic.
 * The selected child is loaded and type-checked only after routing. Stronger
 * cross-page topology checks remain the job of PRAGMA integrity_check. */
static bool internal_routing_valid(Table* table,
                                   uint32_t page_num,
                                   void* node) {
    if (table == NULL || node == NULL || get_node_type(node) != NODE_INTERNAL) {
        return false;
    }

    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys == 0u || num_keys > INTERNAL_NODE_MAX_KEYS) return false;

    uint32_t right_child = *internal_node_right_child(node);
    if (!valid_page_number(table, right_child) || right_child == page_num) {
        return false;
    }

    uint32_t previous_key = 0u;
    for (uint32_t i = 0u; i < num_keys; i++) {
        uint32_t separator = *internal_node_key(node, i);
        if (i > 0u && separator <= previous_key) return false;
        previous_key = separator;

        uint32_t child_page = *internal_node_child(node, i);
        if (!valid_page_number(table, child_page) ||
            child_page == page_num || child_page == right_child) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (*internal_node_child(node, j) == child_page) return false;
        }
    }
    return true;
}

static Cursor* leaf_find(Table* table, uint32_t page_num, uint32_t key) {
    if (!valid_page_number(table, page_num)) return NULL;
    void* page = get_page(table->pager, page_num);
    if (get_node_type(page) != NODE_LEAF) return NULL;
    uint32_t cell_index = 0u;
    bool exact_match = false;
    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count) ||
        !tinydb_leaf_page_lower_bound(page,
                                      PAGE_SIZE,
                                      key,
                                      &cell_index,
                                      &exact_match) ||
        cell_index > count) {
        return NULL;
    }
    (void)exact_match;

    Cursor* cursor = make_cursor(table, page_num, cell_index, false);
    if (cursor == NULL || cell_index < count) return cursor;

    /* lower_bound may legitimately return count when the sought key is above
     * this leaf's current maximum (for example after deleting that maximum).
     * Normalize that insertion position to the first row of the next non-empty
     * sibling, or to a clean end-of-table cursor when this is the tail leaf.
     * Treat broken sibling reciprocity/order as corruption rather than letting
     * range readers later misclassify the out-of-range cell index. */
    uint32_t current_page = page_num;
    uint32_t current_count = count;
    uint32_t traversed = 0u;
    while (true) {
        uint32_t next_page = 0u;
        page = get_page(table->pager, current_page);
        if (!tinydb_leaf_page_next(page, PAGE_SIZE, &next_page)) {
            free(cursor);
            return NULL;
        }
        if (next_page == 0u) {
            cursor->page_num = current_page;
            cursor->cell_num = current_count;
            cursor->end_of_table = true;
            return cursor;
        }
        if (!valid_page_number(table, next_page) ||
            next_page == current_page || traversed++ >= table->pager->num_pages) {
            free(cursor);
            return NULL;
        }

        void* next = get_page(table->pager, next_page);
        uint32_t next_count = 0u;
        if (get_node_type(next) != NODE_LEAF ||
            !tinydb_leaf_page_count(next, PAGE_SIZE, &next_count) ||
            !reciprocal_forward_transition(table, current_page, next) ||
            !ordered_forward_transition(table,
                                        current_page,
                                        current_count,
                                        next_page,
                                        next_count)) {
            free(cursor);
            return NULL;
        }

        cursor->page_num = next_page;
        if (next_count > 0u) {
            cursor->cell_num = 0u;
            return cursor;
        }
        current_page = next_page;
        current_count = next_count;
    }
}

static Cursor* internal_find(Table* table,
                             uint32_t page_num,
                             uint32_t key,
                             uint32_t depth) {
    if (!valid_page_number(table, page_num) ||
        depth > table->pager->num_pages) {
        return NULL;
    }
    void* node = get_page(table->pager, page_num);
    if (!internal_routing_valid(table, page_num, node)) return NULL;

    uint32_t child_page = *internal_node_find_child(node, key);
    if (!valid_page_number(table, child_page) || child_page == page_num) {
        return NULL;
    }
    void* child = get_page(table->pager, child_page);
    if (get_node_type(child) == NODE_LEAF) {
        return leaf_find(table, child_page, key);
    }
    if (get_node_type(child) == NODE_INTERNAL) {
        return internal_find(table, child_page, key, depth + 1u);
    }
    return NULL;
}

Cursor* tinydb_leaf_read_find(Table* table, uint32_t key) {
    if (table == NULL || table->pager == NULL ||
        !valid_page_number(table, table->root_page_num)) {
        return NULL;
    }
    void* root = get_page(table->pager, table->root_page_num);
    if (get_node_type(root) == NODE_LEAF) {
        return leaf_find(table, table->root_page_num, key);
    }
    if (get_node_type(root) == NODE_INTERNAL) {
        return internal_find(table, table->root_page_num, key, 0u);
    }
    return NULL;
}

Cursor* tinydb_leaf_read_start(Table* table) {
    Cursor* cursor = tinydb_leaf_read_find(table, 0u);
    if (cursor == NULL) return NULL;
    if (cursor->end_of_table) return cursor;
    void* page = get_page(table->pager, cursor->page_num);
    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count)) {
        free(cursor);
        return NULL;
    }
    cursor->end_of_table = count == 0u;
    return cursor;
}

void* tinydb_leaf_read_value(Cursor* cursor) {
    if (cursor == NULL || cursor->table == NULL ||
        !valid_page_number(cursor->table, cursor->page_num)) {
        return NULL;
    }
    void* page = get_page(cursor->table->pager, cursor->page_num);
    if (get_node_type(page) != NODE_LEAF) return NULL;
    const void* value = NULL;
    uint32_t value_length = 0u;
    if (!tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cursor->cell_num,
                                   &value,
                                   &value_length)) {
        return NULL;
    }
    (void)value_length;
    return (void*)value;
}

bool tinydb_leaf_read_advance_checked(Cursor* cursor) {
    if (cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL) {
        return false;
    }
    if (cursor->end_of_table) return true;

    Table* table = cursor->table;
    if (!valid_page_number(table, cursor->page_num)) {
        cursor->end_of_table = true;
        return false;
    }

    void* page = get_page(table->pager, cursor->page_num);
    if (get_node_type(page) != NODE_LEAF) {
        cursor->end_of_table = true;
        return false;
    }
    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count) ||
        cursor->cell_num >= count) {
        cursor->end_of_table = true;
        return false;
    }

    if (cursor->cell_num + 1u < count) {
        cursor->cell_num++;
        return true;
    }

    uint32_t current_page = cursor->page_num;
    uint32_t current_count = count;
    uint32_t traversed = 0u;
    while (true) {
        uint32_t next_page = 0u;
        page = get_page(table->pager, current_page);
        if (!tinydb_leaf_page_next(page, PAGE_SIZE, &next_page)) {
            cursor->end_of_table = true;
            return false;
        }
        if (next_page == 0u) {
            cursor->end_of_table = true;
            return true;
        }
        if (!valid_page_number(table, next_page) ||
            next_page == current_page || traversed++ >= table->pager->num_pages) {
            cursor->end_of_table = true;
            return false;
        }

        void* next = get_page(table->pager, next_page);
        uint32_t next_count = 0u;
        if (get_node_type(next) != NODE_LEAF ||
            !tinydb_leaf_page_count(next, PAGE_SIZE, &next_count) ||
            !reciprocal_forward_transition(table, current_page, next) ||
            !ordered_forward_transition(table,
                                        current_page,
                                        current_count,
                                        next_page,
                                        next_count)) {
            cursor->end_of_table = true;
            return false;
        }

        cursor->page_num = next_page;
        if (next_count > 0u) {
            cursor->cell_num = 0u;
            return true;
        }
        current_page = next_page;
        current_count = next_count;
    }
}

void tinydb_leaf_read_advance(Cursor* cursor) {
    (void)tinydb_leaf_read_advance_checked(cursor);
}

Cursor* tinydb_leaf_read_end(Table* table) {
    if (table == NULL || table->pager == NULL ||
        !valid_page_number(table, table->root_page_num)) {
        return NULL;
    }

    uint32_t page_num = table->root_page_num;
    void* node = get_page(table->pager, page_num);
    uint32_t depth = 0u;
    while (get_node_type(node) == NODE_INTERNAL) {
        if (!internal_routing_valid(table, page_num, node)) return NULL;
        uint32_t child_page = *internal_node_right_child(node);
        if (!valid_page_number(table, child_page) || child_page == page_num ||
            depth++ >= table->pager->num_pages) {
            return NULL;
        }
        page_num = child_page;
        node = get_page(table->pager, page_num);
    }
    if (get_node_type(node) != NODE_LEAF) return NULL;

    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(node, PAGE_SIZE, &count)) return NULL;
    return make_cursor(table,
                       page_num,
                       count > 0u ? count - 1u : 0u,
                       count == 0u);
}

bool tinydb_leaf_read_retreat_checked(Cursor* cursor) {
    if (cursor == NULL || cursor->table == NULL ||
        cursor->table->pager == NULL) {
        return false;
    }
    if (cursor->end_of_table) return true;

    Table* table = cursor->table;
    if (!valid_page_number(table, cursor->page_num)) {
        cursor->end_of_table = true;
        return false;
    }
    void* page = get_page(table->pager, cursor->page_num);
    if (get_node_type(page) != NODE_LEAF) {
        cursor->end_of_table = true;
        return false;
    }
    uint32_t count = 0u;
    if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count) ||
        cursor->cell_num >= count) {
        cursor->end_of_table = true;
        return false;
    }
    if (cursor->cell_num > 0u) {
        cursor->cell_num--;
        return true;
    }

    uint32_t current_page = cursor->page_num;
    uint32_t current_count = count;
    uint32_t traversed = 0u;
    while (true) {
        uint32_t prev_page = 0u;
        page = get_page(table->pager, current_page);
        if (!tinydb_leaf_page_prev(page, PAGE_SIZE, &prev_page)) {
            cursor->end_of_table = true;
            return false;
        }
        if (prev_page == 0u) {
            cursor->end_of_table = true;
            return true;
        }
        if (!valid_page_number(table, prev_page) ||
            prev_page == current_page || traversed++ >= table->pager->num_pages) {
            cursor->end_of_table = true;
            return false;
        }

        void* prev = get_page(table->pager, prev_page);
        uint32_t prev_count = 0u;
        if (get_node_type(prev) != NODE_LEAF ||
            !tinydb_leaf_page_count(prev, PAGE_SIZE, &prev_count) ||
            !reciprocal_backward_transition(table, current_page, prev) ||
            !ordered_backward_transition(table,
                                         current_page,
                                         current_count,
                                         prev_page,
                                         prev_count)) {
            cursor->end_of_table = true;
            return false;
        }

        cursor->page_num = prev_page;
        if (prev_count > 0u) {
            cursor->cell_num = prev_count - 1u;
            return true;
        }
        current_page = prev_page;
        current_count = prev_count;
    }
}

void tinydb_leaf_read_retreat(Cursor* cursor) {
    (void)tinydb_leaf_read_retreat_checked(cursor);
}

/* Preserve the historical public cursor contract for all existing SQL,
 * mutation, split, recovery, diagnostics, and legacy-index code. Mixed-format
 * reads opt into tinydb_leaf_read_* explicitly instead of changing these
 * semantics underneath V1 production code. */
Cursor* table_find(Table* table, uint32_t key) {
    return table_find_v1_base(table, key);
}

Cursor* table_start(Table* table) {
    return table_start_v1_base(table);
}

Cursor* table_end(Table* table) {
    return table_end_v1_base(table);
}

void* cursor_value(Cursor* cursor) {
    return cursor_value_v1_base(cursor);
}

void cursor_advance(Cursor* cursor) {
    cursor_advance_v1_base(cursor);
}

void cursor_retreat(Cursor* cursor) {
    cursor_retreat_v1_base(cursor);
}
