#include "leaf_mutation_policy.h"

#include "leaf_page_access.h"

#include <stdio.h>
#include <stdlib.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static bool valid_page_num(const Table* table, uint32_t page_num) {
    return table != NULL && table->pager != NULL &&
           page_num != INVALID_PAGE_NUM && page_num < table->pager->num_pages;
}

static bool validate_leaf_for_mutation(Table* table,
                                       uint32_t page_num,
                                       char* message,
                                       size_t message_size) {
    void* page = get_page(table->pager, page_num);
    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        set_message(message,
                    message_size,
                    "slotted leaf V2 pages are read-only until V2 mutation and split recovery are enabled");
        return false;
    }
    if (format != TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        set_message(message,
                    message_size,
                    "unknown or corrupt leaf format blocks mutation");
        return false;
    }

    uint32_t next_page = 0u;
    uint32_t prev_page = 0u;
    if (!tinydb_leaf_page_next(page, PAGE_SIZE, &next_page) ||
        !tinydb_leaf_page_prev(page, PAGE_SIZE, &prev_page)) {
        set_message(message,
                    message_size,
                    "unable to validate leaf sibling metadata before mutation");
        return false;
    }
    if ((next_page != 0u &&
         (!valid_page_num(table, next_page) || next_page == page_num)) ||
        (prev_page != 0u &&
         (!valid_page_num(table, prev_page) || prev_page == page_num))) {
        set_message(message,
                    message_size,
                    "corrupt leaf sibling metadata blocks mutation");
        return false;
    }
    return true;
}

static bool page_descends_from_root(Table* table,
                                    uint32_t page_num,
                                    uint32_t root_page_num) {
    uint32_t traversed = 0u;
    while (valid_page_num(table, page_num) &&
           traversed++ <= table->pager->num_pages) {
        if (page_num == root_page_num) return true;
        void* page = get_page(table->pager, page_num);
        if (is_node_root(page)) return false;
        uint32_t parent = *node_parent(page);
        if (!valid_page_num(table, parent) || parent == page_num) return false;
        page_num = parent;
    }
    return false;
}

static bool reject_descendant_v2_pages(Table* table,
                                       uint32_t root_page_num,
                                       char* message,
                                       size_t message_size) {
    for (uint32_t page_num = 0u; page_num < table->pager->num_pages; page_num++) {
        void* page = get_page(table->pager, page_num);
        if (get_node_type(page) != NODE_LEAF) continue;
        if (tinydb_leaf_format_detect_page(page, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
            continue;
        }
        if (page_descends_from_root(table, page_num, root_page_num)) {
            set_message(message,
                        message_size,
                        "slotted leaf V2 pages are read-only until V2 mutation and split recovery are enabled");
            return false;
        }
    }
    return true;
}

static bool find_leftmost_leaf(Table* table,
                               uint32_t root_page_num,
                               uint32_t* leaf_page_num,
                               char* message,
                               size_t message_size) {
    uint32_t page_num = root_page_num;
    uint32_t depth = 0u;
    while (true) {
        if (!valid_page_num(table, page_num) || depth++ > table->pager->num_pages) {
            set_message(message,
                        message_size,
                        "corrupt B+ tree path blocks mutation");
            return false;
        }

        void* node = get_page(table->pager, page_num);
        NodeType type = get_node_type(node);
        if (type == NODE_LEAF) {
            *leaf_page_num = page_num;
            return true;
        }
        if (type != NODE_INTERNAL) {
            set_message(message,
                        message_size,
                        "unknown B+ tree node type blocks mutation");
            return false;
        }

        uint32_t key_count = *internal_node_num_keys(node);
        if (key_count > INTERNAL_NODE_MAX_KEYS) {
            set_message(message,
                        message_size,
                        "corrupt internal-node key count blocks mutation");
            return false;
        }

        uint32_t child = *internal_node_child(node, 0u);
        if (!valid_page_num(table, child) || child == page_num) {
            set_message(message,
                        message_size,
                        "corrupt internal child pointer blocks mutation");
            return false;
        }
        page_num = child;
    }
}

static bool validate_leaf_chain(Table* table,
                                uint32_t root_page_num,
                                char* message,
                                size_t message_size) {
    uint32_t page_count = table->pager->num_pages;
    unsigned char* visited = (unsigned char*)calloc(page_count, 1u);
    if (visited == NULL) {
        set_message(message,
                    message_size,
                    "unable to allocate leaf-chain validation state");
        return false;
    }

    uint32_t page_num = 0u;
    bool supported = find_leftmost_leaf(table,
                                        root_page_num,
                                        &page_num,
                                        message,
                                        message_size);
    uint32_t expected_prev = 0u;
    uint32_t previous_last_key = 0u;
    bool have_previous_key = false;

    while (supported) {
        if (!valid_page_num(table, page_num) || visited[page_num]) {
            set_message(message,
                        message_size,
                        "corrupt or cyclic leaf chain blocks mutation");
            supported = false;
            break;
        }
        visited[page_num] = 1u;

        void* page = get_page(table->pager, page_num);
        TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
        if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
            set_message(message,
                        message_size,
                        "slotted leaf V2 pages are read-only until V2 mutation and split recovery are enabled");
            supported = false;
            break;
        }
        if (format != TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
            set_message(message,
                        message_size,
                        "unknown or corrupt leaf format blocks mutation");
            supported = false;
            break;
        }

        uint32_t prev_page = 0u;
        uint32_t next_page = 0u;
        uint32_t count = 0u;
        if (!tinydb_leaf_page_prev(page, PAGE_SIZE, &prev_page) ||
            !tinydb_leaf_page_next(page, PAGE_SIZE, &next_page) ||
            !tinydb_leaf_page_count(page, PAGE_SIZE, &count) ||
            prev_page != expected_prev) {
            set_message(message,
                        message_size,
                        "inconsistent leaf sibling chain blocks mutation");
            supported = false;
            break;
        }

        if (count > 0u) {
            uint32_t first_key = 0u;
            uint32_t last_key = 0u;
            uint32_t previous_key = 0u;
            for (uint32_t i = 0u; i < count; i++) {
                uint32_t current_key = 0u;
                if (!tinydb_leaf_page_key_at(page,
                                             PAGE_SIZE,
                                             i,
                                             &current_key) ||
                    (i > 0u && previous_key >= current_key)) {
                    set_message(message,
                                message_size,
                                "non-monotonic leaf key order blocks mutation");
                    supported = false;
                    break;
                }
                if (i == 0u) first_key = current_key;
                last_key = current_key;
                previous_key = current_key;
            }
            if (!supported) break;
            if (have_previous_key && previous_last_key >= first_key) {
                set_message(message,
                            message_size,
                            "non-monotonic leaf key order blocks mutation");
                supported = false;
                break;
            }
            previous_last_key = last_key;
            have_previous_key = true;
        }

        if (next_page == 0u) break;
        if (!valid_page_num(table, next_page) || next_page == page_num) {
            set_message(message,
                        message_size,
                        "corrupt leaf sibling metadata blocks mutation");
            supported = false;
            break;
        }
        expected_prev = page_num;
        page_num = next_page;
    }

    free(visited);
    return supported;
}

static bool validate_tree_structure(Table* table,
                                    uint32_t root_page_num,
                                    char* message,
                                    size_t message_size) {
    uint32_t page_count = table->pager->num_pages;
    unsigned char* visited = (unsigned char*)calloc(page_count, 1u);
    uint32_t* stack = (uint32_t*)malloc(sizeof(uint32_t) * page_count);
    if (visited == NULL || stack == NULL) {
        free(visited);
        free(stack);
        set_message(message,
                    message_size,
                    "unable to allocate mutation-policy traversal state");
        return false;
    }

    uint32_t stack_count = 0u;
    stack[stack_count++] = root_page_num;
    bool supported = true;

    while (stack_count > 0u && supported) {
        uint32_t page_num = stack[--stack_count];
        if (!valid_page_num(table, page_num) || visited[page_num]) {
            set_message(message,
                        message_size,
                        "corrupt or cyclic B+ tree blocks mutation");
            supported = false;
            break;
        }
        visited[page_num] = 1u;

        void* node = get_page(table->pager, page_num);
        NodeType type = get_node_type(node);
        if (type == NODE_LEAF) {
            supported = validate_leaf_for_mutation(table,
                                                   page_num,
                                                   message,
                                                   message_size);
            continue;
        }
        if (type != NODE_INTERNAL) {
            set_message(message,
                        message_size,
                        "unknown B+ tree node type blocks mutation");
            supported = false;
            break;
        }

        uint32_t key_count = *internal_node_num_keys(node);
        if (key_count > INTERNAL_NODE_MAX_KEYS) {
            set_message(message,
                        message_size,
                        "corrupt internal-node key count blocks mutation");
            supported = false;
            break;
        }
        if (stack_count + key_count + 1u > page_count) {
            set_message(message,
                        message_size,
                        "B+ tree fanout exceeds mutation-policy traversal capacity");
            supported = false;
            break;
        }

        for (uint32_t i = 0u; i < key_count; i++) {
            uint32_t child = *internal_node_child(node, i);
            if (!valid_page_num(table, child) || child == page_num) {
                set_message(message,
                            message_size,
                            "corrupt internal child pointer blocks mutation");
                supported = false;
                break;
            }
            stack[stack_count++] = child;
        }
        if (!supported) break;

        uint32_t right_child = *internal_node_right_child(node);
        if (!valid_page_num(table, right_child) || right_child == page_num) {
            set_message(message,
                        message_size,
                        "corrupt internal right-child pointer blocks mutation");
            supported = false;
            break;
        }
        stack[stack_count++] = right_child;
    }

    free(stack);
    free(visited);
    return supported;
}

bool tinydb_leaf_tree_mutation_supported(Table* table,
                                         uint32_t root_page_num,
                                         char* message,
                                         size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || table->pager == NULL ||
        !valid_page_num(table, root_page_num)) {
        set_message(message,
                    message_size,
                    "table and a valid root are required before leaf mutation");
        return false;
    }

    if (!reject_descendant_v2_pages(table,
                                    root_page_num,
                                    message,
                                    message_size) ||
        !validate_tree_structure(table,
                                 root_page_num,
                                 message,
                                 message_size)) {
        return false;
    }
    return validate_leaf_chain(table,
                               root_page_num,
                               message,
                               message_size);
}
