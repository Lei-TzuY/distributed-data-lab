#include "diagnostics.h"
#include "leaf_page_access.h"
#include "pager_try_pin.h"

#include <stdarg.h>

static bool name_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

const TableSchema* tinydb_find_table_schema(const Table* table,
                                            const char* table_name) {
    if (table == NULL || table_name == NULL || table_name[0] == '\0') {
        return NULL;
    }
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (name_equal(table->catalog.schemas[i].name, table_name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

typedef struct {
    Table* table;
    uint32_t root_page_num;
    bool* visited;
    TinyDBTreeStats* stats;
    char* message;
    size_t message_size;
} TreeWalkContext;

static bool diagnostic_fail(TreeWalkContext* context, const char* format, ...) {
    if (context->message != NULL && context->message_size > 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(context->message, context->message_size, format, args);
        va_end(args);
    }
    return false;
}

static bool page_is_free(Pager* pager, uint32_t page_num) {
    for (uint32_t i = 0; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] == page_num) return true;
    }
    return false;
}

static bool release_tree_handle(TreeWalkContext* context,
                                PagerPageHandle* handle,
                                uint32_t page_num) {
    if (pager_release_page_handle(handle)) return true;
    return diagnostic_fail(context,
                           "unable to release diagnostic pin for page %u",
                           page_num);
}

static bool validate_leaf_neighbor(TreeWalkContext* context,
                                   uint32_t page_num,
                                   uint32_t sibling_page_num,
                                   bool previous_neighbor) {
    if (sibling_page_num == 0u) return true;

    Pager* pager = context->table->pager;
    if (sibling_page_num >= pager->num_pages) {
        return diagnostic_fail(context,
                               "leaf page %u has invalid %s pointer %u",
                               page_num,
                               previous_neighbor ? "prev" : "next",
                               sibling_page_num);
    }
    if (page_is_free(pager, sibling_page_num)) {
        return diagnostic_fail(context,
                               "leaf page %u links to free %s page %u",
                               page_num,
                               previous_neighbor ? "prev" : "next",
                               sibling_page_num);
    }

    PagerPageHandle sibling_handle;
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(
        pager, sibling_page_num, &sibling_handle);
    if (status != PAGER_TRY_PIN_OK) {
        return diagnostic_fail(context,
                               "leaf page %u could not acquire %s page %u: %s",
                               page_num,
                               previous_neighbor ? "prev" : "next",
                               sibling_page_num,
                               pager_try_pin_status_string(status));
    }

    void* sibling = sibling_handle.data;
    bool reciprocal_ok = get_node_type(sibling) == NODE_LEAF;
    uint32_t reciprocal_page = 0u;
    if (reciprocal_ok) {
        reciprocal_ok = previous_neighbor
            ? tinydb_leaf_page_next(sibling, PAGE_SIZE, &reciprocal_page)
            : tinydb_leaf_page_prev(sibling, PAGE_SIZE, &reciprocal_page);
        reciprocal_ok = reciprocal_ok && reciprocal_page == page_num;
    }

    if (!release_tree_handle(context, &sibling_handle, sibling_page_num)) {
        return false;
    }
    if (!reciprocal_ok) {
        return diagnostic_fail(context,
                               "leaf page %u has a broken %s link to %u",
                               page_num,
                               previous_neighbor ? "prev" : "next",
                               sibling_page_num);
    }
    return true;
}

static bool walk_tree(TreeWalkContext* context,
                      uint32_t page_num,
                      uint32_t expected_parent,
                      uint32_t depth) {
    Pager* pager = context->table->pager;
    if (page_num >= pager->num_pages) {
        return diagnostic_fail(context,
                               "page %u is outside the database page range",
                               page_num);
    }
    if (page_is_free(pager, page_num)) {
        return diagnostic_fail(context,
                               "tree references free page %u",
                               page_num);
    }
    if (context->visited[page_num]) {
        return diagnostic_fail(context,
                               "tree contains a cycle or duplicate child reference at page %u",
                               page_num);
    }

    PagerPageHandle node_handle;
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(
        pager, page_num, &node_handle);
    if (status != PAGER_TRY_PIN_OK) {
        return diagnostic_fail(context,
                               "page %u could not be acquired: %s",
                               page_num,
                               pager_try_pin_status_string(status));
    }
    context->visited[page_num] = true;

    void* node = node_handle.data;
    bool is_root_page = page_num == context->root_page_num;
    if (is_root_page) {
        if (!is_node_root(node)) {
            (void)pager_release_page_handle(&node_handle);
            return diagnostic_fail(context,
                                   "root page %u is not marked as root",
                                   page_num);
        }
    } else {
        if (is_node_root(node)) {
            (void)pager_release_page_handle(&node_handle);
            return diagnostic_fail(context,
                                   "non-root page %u is marked as root",
                                   page_num);
        }
        if (*node_parent(node) != expected_parent) {
            uint32_t actual_parent = *node_parent(node);
            (void)pager_release_page_handle(&node_handle);
            return diagnostic_fail(context,
                                   "page %u parent is %u, expected %u",
                                   page_num,
                                   actual_parent,
                                   expected_parent);
        }
    }

    if (depth + 1 > context->stats->height) {
        context->stats->height = depth + 1;
    }

    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        uint32_t num_cells = 0u;
        if (!tinydb_leaf_page_count(node, PAGE_SIZE, &num_cells)) {
            (void)pager_release_page_handle(&node_handle);
            return diagnostic_fail(context,
                                   "leaf page %u has an invalid leaf format",
                                   page_num);
        }

        uint32_t previous_key = 0u;
        for (uint32_t i = 0u; i < num_cells; i++) {
            uint32_t current_key = 0u;
            if (!tinydb_leaf_page_key_at(node, PAGE_SIZE, i, &current_key)) {
                (void)pager_release_page_handle(&node_handle);
                return diagnostic_fail(context,
                                       "leaf page %u has an invalid key at cell %u",
                                       page_num,
                                       i);
            }
            if (i > 0u && previous_key >= current_key) {
                (void)pager_release_page_handle(&node_handle);
                return diagnostic_fail(context,
                                       "leaf page %u keys are not strictly ordered (%u >= %u)",
                                       page_num,
                                       previous_key,
                                       current_key);
            }
            previous_key = current_key;
        }

        uint32_t previous_leaf = 0u;
        uint32_t next_leaf = 0u;
        if (!tinydb_leaf_page_prev(node, PAGE_SIZE, &previous_leaf) ||
            !tinydb_leaf_page_next(node, PAGE_SIZE, &next_leaf)) {
            (void)pager_release_page_handle(&node_handle);
            return diagnostic_fail(context,
                                   "leaf page %u has invalid sibling metadata",
                                   page_num);
        }

        context->stats->leaf_pages++;
        context->stats->total_rows += num_cells;
        if (!release_tree_handle(context, &node_handle, page_num)) return false;

        return validate_leaf_neighbor(context,
                                      page_num,
                                      previous_leaf,
                                      true) &&
               validate_leaf_neighbor(context,
                                      page_num,
                                      next_leaf,
                                      false);
    }

    if (type != NODE_INTERNAL) {
        (void)pager_release_page_handle(&node_handle);
        return diagnostic_fail(context,
                               "page %u has unknown node type %u",
                               page_num,
                               (uint32_t)type);
    }

    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys > INTERNAL_NODE_MAX_KEYS) {
        (void)pager_release_page_handle(&node_handle);
        return diagnostic_fail(context,
                               "internal page %u has %u keys, max is %u",
                               page_num,
                               num_keys,
                               (uint32_t)INTERNAL_NODE_MAX_KEYS);
    }
    for (uint32_t i = 1; i < num_keys; i++) {
        uint32_t previous = *internal_node_key(node, i - 1);
        uint32_t current = *internal_node_key(node, i);
        if (previous >= current) {
            (void)pager_release_page_handle(&node_handle);
            return diagnostic_fail(context,
                                   "internal page %u separator keys are not strictly ordered (%u >= %u)",
                                   page_num,
                                   previous,
                                   current);
        }
    }

    uint32_t child_count = num_keys + 1;
    uint32_t* child_pages = (uint32_t*)malloc(sizeof(uint32_t) * child_count);
    if (child_pages == NULL) {
        (void)pager_release_page_handle(&node_handle);
        return diagnostic_fail(context,
                               "unable to allocate child snapshot for page %u",
                               page_num);
    }
    for (uint32_t i = 0; i < child_count; i++) {
        child_pages[i] = *internal_node_child(node, i);
    }

    context->stats->internal_pages++;
    if (!release_tree_handle(context, &node_handle, page_num)) {
        free(child_pages);
        return false;
    }

    for (uint32_t i = 0; i < child_count; i++) {
        if (!walk_tree(context, child_pages[i], page_num, depth + 1)) {
            free(child_pages);
            return false;
        }
    }
    free(child_pages);
    return true;
}

static bool run_tree_walk(Table* table,
                          const char* table_name,
                          TinyDBTreeStats* stats,
                          char* message,
                          size_t message_size) {
    const TableSchema* schema = tinydb_find_table_schema(table, table_name);
    if (schema == NULL) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "table '%s' not found", table_name);
        }
        return false;
    }
    if (schema->root_page_num >= table->pager->num_pages) {
        if (message != NULL && message_size > 0) {
            snprintf(message,
                     message_size,
                     "table '%s' root page %u is outside the database",
                     schema->name,
                     schema->root_page_num);
        }
        return false;
    }

    memset(stats, 0, sizeof(*stats));
    stats->root_page_num = schema->root_page_num;
    if (message != NULL && message_size > 0) message[0] = '\0';

    bool* visited = (bool*)calloc(table->pager->num_pages, sizeof(bool));
    if (visited == NULL) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "unable to allocate diagnostic visited map");
        }
        return false;
    }

    TreeWalkContext context;
    context.table = table;
    context.root_page_num = schema->root_page_num;
    context.visited = visited;
    context.stats = stats;
    context.message = message;
    context.message_size = message_size;

    bool ok = walk_tree(&context, schema->root_page_num, schema->root_page_num, 0);
    free(visited);
    return ok;
}

bool tinydb_get_tree_stats_diagnostic(Table* table,
                                      const char* table_name,
                                      TinyDBTreeStats* stats,
                                      char* message,
                                      size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (table == NULL || table_name == NULL || stats == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "invalid tree-stat arguments");
        }
        return false;
    }
    return run_tree_walk(table, table_name, stats, message, message_size);
}

bool tinydb_get_tree_stats(Table* table,
                           const char* table_name,
                           TinyDBTreeStats* stats) {
    char ignored[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    return tinydb_get_tree_stats_diagnostic(table,
                                            table_name,
                                            stats,
                                            ignored,
                                            sizeof(ignored));
}

bool tinydb_check_table_tree(Table* table,
                             const char* table_name,
                             char* message,
                             size_t message_size) {
    if (table == NULL || table_name == NULL) return false;

    TinyDBTreeStats tree_stats;
    if (!run_tree_walk(table, table_name, &tree_stats, message, message_size)) {
        return false;
    }

    TinyDBPageOwnershipStats ownership_stats;
    char ownership_message_text[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_page_ownership(table,
                                     &ownership_stats,
                                     ownership_message_text,
                                     sizeof(ownership_message_text))) {
        if (message != NULL && message_size > 0) {
            snprintf(message,
                     message_size,
                     "page ownership: %s",
                     ownership_message_text);
        }
        return false;
    }

    if (message != NULL && message_size > 0) {
        snprintf(message,
                 message_size,
                 "ok: root=%u height=%u rows=%u leaf_pages=%u internal_pages=%u ownership_owned=%u free=%u",
                 tree_stats.root_page_num,
                 tree_stats.height,
                 tree_stats.total_rows,
                 tree_stats.leaf_pages,
                 tree_stats.internal_pages,
                 ownership_stats.owned_pages,
                 ownership_stats.free_pages);
    }
    return true;
}

typedef struct {
    Table* table;
    uint32_t* owners;
    bool* shared;
    TinyDBPageOwnershipStats* stats;
    char* message;
    size_t message_size;
} OwnershipContext;

static void ownership_message(OwnershipContext* context, const char* format, ...) {
    if (context->message == NULL || context->message_size == 0 ||
        context->message[0] != '\0') {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(context->message, context->message_size, format, args);
    va_end(args);
}

static bool release_ownership_handle(OwnershipContext* context,
                                     PagerPageHandle* handle,
                                     uint32_t page_num) {
    if (pager_release_page_handle(handle)) return true;
    ownership_message(context,
                      "unable to release ownership diagnostic pin for page %u",
                      page_num);
    return false;
}

static bool claim_tree_pages(OwnershipContext* context,
                             uint32_t page_num,
                             uint32_t table_index) {
    Pager* pager = context->table->pager;
    if (page_num >= pager->num_pages) {
        ownership_message(context,
                          "table '%s' references out-of-range page %u",
                          context->table->catalog.schemas[table_index].name,
                          page_num);
        return false;
    }
    if (page_is_free(pager, page_num)) {
        ownership_message(context,
                          "table '%s' references free page %u",
                          context->table->catalog.schemas[table_index].name,
                          page_num);
        return false;
    }

    if (context->owners[page_num] != UINT32_MAX) {
        if (!context->shared[page_num]) {
            context->shared[page_num] = true;
            context->stats->shared_pages++;
        }
        ownership_message(
            context,
            "page %u is referenced by both '%s' and '%s' (or more than once by one tree)",
            page_num,
            context->table->catalog.schemas[context->owners[page_num]].name,
            context->table->catalog.schemas[table_index].name);
        return false;
    }

    context->owners[page_num] = table_index;
    context->stats->owned_pages++;

    PagerPageHandle node_handle;
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(
        pager, page_num, &node_handle);
    if (status != PAGER_TRY_PIN_OK) {
        ownership_message(context,
                          "owned page %u could not be acquired: %s",
                          page_num,
                          pager_try_pin_status_string(status));
        return false;
    }

    void* node = node_handle.data;
    NodeType type = get_node_type(node);
    if (type == NODE_LEAF) {
        return release_ownership_handle(context, &node_handle, page_num);
    }
    if (type != NODE_INTERNAL) {
        (void)pager_release_page_handle(&node_handle);
        ownership_message(context,
                          "owned page %u has unknown node type %u",
                          page_num,
                          (uint32_t)type);
        return false;
    }

    uint32_t num_keys = *internal_node_num_keys(node);
    if (num_keys > INTERNAL_NODE_MAX_KEYS) {
        (void)pager_release_page_handle(&node_handle);
        ownership_message(context,
                          "internal page %u has invalid key count %u",
                          page_num,
                          num_keys);
        return false;
    }

    uint32_t child_count = num_keys + 1;
    uint32_t* children = (uint32_t*)malloc(sizeof(uint32_t) * child_count);
    if (children == NULL) {
        (void)pager_release_page_handle(&node_handle);
        ownership_message(context,
                          "unable to allocate ownership child snapshot for page %u",
                          page_num);
        return false;
    }
    for (uint32_t i = 0; i < child_count; i++) {
        children[i] = *internal_node_child(node, i);
    }

    if (!release_ownership_handle(context, &node_handle, page_num)) {
        free(children);
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0; i < child_count; i++) {
        if (!claim_tree_pages(context, children[i], table_index)) {
            ok = false;
        }
    }
    free(children);
    return ok;
}

static bool validate_free_page_list(OwnershipContext* context) {
    Pager* pager = context->table->pager;
    for (uint32_t i = 0; i < pager->free_page_count; i++) {
        uint32_t page_num = pager->free_pages[i];
        if (page_num >= pager->num_pages) {
            ownership_message(context,
                              "free-page list contains out-of-range page %u",
                              page_num);
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (pager->free_pages[j] == page_num) {
                ownership_message(context,
                                  "free-page list contains duplicate page %u",
                                  page_num);
                return false;
            }
        }
    }
    return true;
}

bool tinydb_check_page_ownership(Table* table,
                                 TinyDBPageOwnershipStats* stats,
                                 char* message,
                                 size_t message_size) {
    if (table == NULL || stats == NULL) return false;

    memset(stats, 0, sizeof(*stats));
    stats->total_pages = table->pager->num_pages;
    stats->free_pages = table->pager->free_page_count;
    if (message != NULL && message_size > 0) message[0] = '\0';

    if (table->pager->num_pages == 0) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "ok: no pages allocated");
        }
        return true;
    }

    uint32_t* owners = (uint32_t*)malloc(sizeof(uint32_t) * table->pager->num_pages);
    bool* shared = (bool*)calloc(table->pager->num_pages, sizeof(bool));
    if (owners == NULL || shared == NULL) {
        free(owners);
        free(shared);
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "unable to allocate page ownership map");
        }
        return false;
    }
    for (uint32_t i = 0; i < table->pager->num_pages; i++) {
        owners[i] = UINT32_MAX;
    }

    OwnershipContext context;
    context.table = table;
    context.owners = owners;
    context.shared = shared;
    context.stats = stats;
    context.message = message;
    context.message_size = message_size;

    bool ok = validate_free_page_list(&context);
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        uint32_t root = table->catalog.schemas[i].root_page_num;
        if (!claim_tree_pages(&context, root, i)) {
            ok = false;
        }
    }

    for (uint32_t page_num = 0; page_num < table->pager->num_pages; page_num++) {
        if (page_is_free(table->pager, page_num)) continue;
        if (owners[page_num] == UINT32_MAX) {
            stats->orphan_pages++;
            ownership_message(&context,
                              "page %u is allocated but unreachable from every catalog root",
                              page_num);
            ok = false;
        }
    }

    if (ok && message != NULL && message_size > 0) {
        snprintf(message,
                 message_size,
                 "ok: total=%u owned=%u free=%u orphan=%u shared=%u",
                 stats->total_pages,
                 stats->owned_pages,
                 stats->free_pages,
                 stats->orphan_pages,
                 stats->shared_pages);
    }

    free(owners);
    free(shared);
    return ok;
}
