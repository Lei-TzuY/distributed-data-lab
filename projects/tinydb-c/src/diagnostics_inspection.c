#include "table.h"
#include "pager_try_pin.h"
#include "leaf_page_access.h"

static void tinydb_inspect_indent(uint32_t level) {
    for (uint32_t i = 0u; i < level; i++) printf("  ");
}

static bool tinydb_print_tree_nonfatal_impl(Pager* pager,
                                            uint32_t page_num,
                                            uint32_t indentation_level) {
    PagerPageHandle handle;
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(
        pager, page_num, &handle);
    if (status != PAGER_TRY_PIN_OK) {
        printf("Error: Unable to inspect B+ tree page %u: %s.\n",
               page_num,
               pager_try_pin_status_string(status));
        return false;
    }

    void* node = handle.data;
    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        uint32_t num_keys = 0u;
        if (!tinydb_leaf_page_count(node, PAGE_SIZE, &num_keys)) {
            (void)pager_release_page_handle(&handle);
            printf("Error: Unable to inspect B+ tree page %u: invalid leaf format.\n",
                   page_num);
            return false;
        }

        tinydb_inspect_indent(indentation_level);
        printf("- leaf (size %u)\n", num_keys);
        for (uint32_t i = 0u; i < num_keys; i++) {
            uint32_t key = 0u;
            if (!tinydb_leaf_page_key_at(node, PAGE_SIZE, i, &key)) {
                (void)pager_release_page_handle(&handle);
                printf("Error: Unable to inspect B+ tree page %u: invalid leaf key at cell %u.\n",
                       page_num,
                       i);
                return false;
            }
            tinydb_inspect_indent(indentation_level + 1u);
            printf("- %u\n", key);
        }
        if (!pager_release_page_handle(&handle)) {
            printf("Error: Unable to release B+ tree inspection page %u.\n",
                   page_num);
            return false;
        }
        return true;
    }

    if (type != NODE_INTERNAL) {
        (void)pager_release_page_handle(&handle);
        printf("Error: Unable to inspect B+ tree page %u: unknown node type %u.\n",
               page_num,
               (uint32_t)type);
        return false;
    }

    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys > INTERNAL_NODE_MAX_KEYS) {
        (void)pager_release_page_handle(&handle);
        printf("Error: Unable to inspect B+ tree page %u: invalid internal key count %u.\n",
               page_num,
               num_keys);
        return false;
    }

    uint32_t child_count = num_keys + 1u;
    uint32_t* children = (uint32_t*)malloc(sizeof(uint32_t) * child_count);
    uint32_t* keys = num_keys == 0u
        ? NULL
        : (uint32_t*)malloc(sizeof(uint32_t) * num_keys);
    if (children == NULL || (num_keys != 0u && keys == NULL)) {
        free(children);
        free(keys);
        (void)pager_release_page_handle(&handle);
        printf("Error: Unable to allocate B+ tree inspection snapshot for page %u.\n",
               page_num);
        return false;
    }

    for (uint32_t i = 0u; i < num_keys; i++) {
        children[i] = *internal_node_child(node, i);
        keys[i] = *internal_node_key(node, i);
    }
    children[num_keys] = *internal_node_right_child(node);

    tinydb_inspect_indent(indentation_level);
    printf("- internal (size %u)\n", num_keys);
    if (!pager_release_page_handle(&handle)) {
        free(children);
        free(keys);
        printf("Error: Unable to release B+ tree inspection page %u.\n",
               page_num);
        return false;
    }

    for (uint32_t i = 0u; i < num_keys; i++) {
        if (!tinydb_print_tree_nonfatal_impl(pager,
                                             children[i],
                                             indentation_level + 1u)) {
            free(children);
            free(keys);
            return false;
        }
        tinydb_inspect_indent(indentation_level + 1u);
        printf("- key %u\n", keys[i]);
    }
    bool ok = tinydb_print_tree_nonfatal_impl(pager,
                                               children[num_keys],
                                               indentation_level + 1u);
    free(children);
    free(keys);
    return ok;
}

bool tinydb_print_tree_nonfatal(Pager* pager,
                                uint32_t page_num,
                                uint32_t indentation_level) {
    if (pager == NULL || page_num >= pager->num_pages) {
        printf("Error: Unable to inspect B+ tree page %u: invalid page.\n",
               page_num);
        return false;
    }
    return tinydb_print_tree_nonfatal_impl(pager,
                                           page_num,
                                           indentation_level);
}

bool tinydb_print_page_nonfatal(Table* table,
                                uint32_t page_num) {
    if (table == NULL || table->pager == NULL ||
        page_num >= table->pager->num_pages) {
        printf("Error: Page number %u out of bounds (total pages: %u).\n",
               page_num,
               table != NULL && table->pager != NULL
                   ? table->pager->num_pages
                   : 0u);
        return false;
    }

    PagerPageHandle handle;
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(
        table->pager, page_num, &handle);
    if (status != PAGER_TRY_PIN_OK) {
        printf("Error: Unable to inspect page %u: %s.\n",
               page_num,
               pager_try_pin_status_string(status));
        return false;
    }

    void* page = handle.data;
    NodeType type = get_node_type(page);
    bool is_root_page = is_node_root(page);
    uint32_t parent = *node_parent(page);

    printf("--- Page %u Details ---\n", page_num);
    printf("Type: %s\n",
           type == NODE_LEAF ? "LEAF" :
           type == NODE_INTERNAL ? "INTERNAL" : "UNKNOWN");
    printf("Is Root: %s\n", is_root_page ? "Yes" : "No");
    printf("Parent Page: %u\n", parent);

    bool valid = true;
    if (type == NODE_LEAF) {
        uint32_t num_cells = 0u;
        uint32_t next_leaf = 0u;
        uint32_t prev_leaf = 0u;
        if (!tinydb_leaf_page_count(page, PAGE_SIZE, &num_cells) ||
            !tinydb_leaf_page_prev(page, PAGE_SIZE, &prev_leaf) ||
            !tinydb_leaf_page_next(page, PAGE_SIZE, &next_leaf)) {
            printf("Error: Page %u has invalid leaf format.\n", page_num);
            valid = false;
        } else {
            printf("Leaf Format: %s\n",
                   tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE)
                       ? "FIXED_V1"
                       : "SLOTTED_V2");
            printf("Num Cells: %u\n", num_cells);
            printf("Prev Leaf Page: %u\n", prev_leaf);
            printf("Next Leaf Page: %u\n", next_leaf);
            printf("Keys: ");
            for (uint32_t i = 0u; i < num_cells; i++) {
                uint32_t key = 0u;
                if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, i, &key)) {
                    printf("<invalid cell %u>", i);
                    valid = false;
                    break;
                }
                printf("%u%s",
                       key,
                       (i + 1u < num_cells) ? ", " : "");
            }
            printf("\n");
        }
    } else if (type == NODE_INTERNAL) {
        uint32_t num_keys = *internal_node_num_keys(page);
        if (num_keys > INTERNAL_NODE_MAX_KEYS) {
            printf("Error: Page %u has invalid internal key count %u.\n",
                   page_num,
                   num_keys);
            valid = false;
        } else {
            uint32_t right_child = *internal_node_right_child(page);
            printf("Num Keys: %u\n", num_keys);
            printf("Right Child Page: %u\n", right_child);
            printf("Keys & Children:\n");
            for (uint32_t i = 0u; i < num_keys; i++) {
                printf("  Child[%u] -> Page %u | Key[%u] = %u\n",
                       i,
                       *internal_node_child(page, i),
                       i,
                       *internal_node_key(page, i));
            }
            printf("  Child[%u] (Rightmost) -> Page %u\n",
                   num_keys,
                   right_child);
        }
    } else {
        printf("Error: Page %u has unknown node type %u.\n",
               page_num,
               (uint32_t)type);
        valid = false;
    }

    if (!pager_release_page_handle(&handle)) {
        printf("Error: Unable to release page inspection pin for page %u.\n",
               page_num);
        return false;
    }
    return valid;
}
