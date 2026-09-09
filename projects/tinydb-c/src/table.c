/*
 * table.c — B+ Tree implementation, cursor, and table lifecycle
 *
 * This file contains the heart of the database engine:
 *  1. Node accessor helpers (read/write fields in raw page memory)
 *  2. Table open/close (calls the pager, initializes the root page)
 *  3. Cursor navigation (table_start, table_find, cursor_advance)
 *  4. Leaf node operations (insert, split)
 *  5. Internal node operations (find, insert key, split)
 *  6. Row serialization / deserialization
 *  7. Tree pretty-printer for the .btree command
 */

#include "table.h"

#ifdef _WIN32
#include <io.h>
#define db_strcasecmp _stricmp
#else
#include <strings.h>
#include <unistd.h>
#define db_strcasecmp strcasecmp
#endif

#define INDEX_CATALOG_MAGIC   0x58444954u /* TIDX */
#define INDEX_CATALOG_VERSION 2u
#define INDEX_CATALOG_WAL_COMMIT_MAGIC 0x49445843u /* IDXC */
#define USERNAME_INDEX_MAGIC 0x55494458u /* UIDX */
#define USERNAME_INDEX_VERSION 1u
#define USERNAME_INDEX_WAL_COMMIT_MAGIC 0x55494443u /* UIDC */
#define USERNAME_INDEX_MAX_ENTRIES (TABLE_MAX_PAGES * LEAF_NODE_MAX_CELLS)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t username_index_enabled;
    uint32_t username_index_entries_valid;
    uint32_t num_indexes;
    SecondaryIndexMeta indexes[MAX_INDEXES];
} IndexCatalog;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
} UsernameIndexFileHeader;

static void table_load_index_catalog(Table* table);
static void table_save_index_catalog_state(Table* table, bool entries_valid);

static void build_index_filename(char* output, size_t output_size,
                                 const char* database_filename,
                                 const char* index_name,
                                 const char* suffix) {
    size_t database_len = strlen(database_filename);
    size_t index_len = strlen(index_name);
    size_t suffix_len = strlen(suffix);

    if (database_len + 1u + index_len + suffix_len + 1u > output_size) {
        printf("Error: Index filename is too long.\n");
        exit(EXIT_FAILURE);
    }

    memcpy(output, database_filename, database_len);
    output[database_len] = '.';
    memcpy(output + database_len + 1u, index_name, index_len);
    memcpy(output + database_len + 1u + index_len, suffix, suffix_len + 1u);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 1 — Node accessor helpers
 *
 * INTERNALS: WHY RAW POINTER ARITHMETIC?
 * ────────────────────────────────────────
 * We store the entire page as a raw `void*` buffer.  Using a C struct
 * with __attribute__((packed)) would also work, but pointer arithmetic
 * is explicit about byte offsets and makes the on-disk format obvious.
 * Each accessor returns a POINTER into the buffer so callers can both
 * read and write through the same pointer.
 * ═══════════════════════════════════════════════════════════════ */

NodeType get_node_type(void* node) {
    uint8_t value = *((uint8_t*)((char*)node + NODE_TYPE_OFFSET));
    return (NodeType)value;
}

void set_node_type(void* node, NodeType type) {
    uint8_t value = (uint8_t)type;
    *((uint8_t*)((char*)node + NODE_TYPE_OFFSET)) = value;
}

bool is_node_root(void* node) {
    uint8_t value = *((uint8_t*)((char*)node + IS_ROOT_OFFSET));
    return (bool)value;
}

void set_node_root(void* node, bool is_root) {
    uint8_t value = (uint8_t)is_root;
    *((uint8_t*)((char*)node + IS_ROOT_OFFSET)) = value;
}

uint32_t* node_parent(void* node) {
    return (uint32_t*)((char*)node + PARENT_POINTER_OFFSET);
}

/* ─── Leaf node accessors ─────────────────────────────────────── */

uint32_t* leaf_node_num_cells(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}

/*
 * INTERNALS: SIBLING POINTER
 * next_leaf stores the page number of the right sibling leaf.
 * Value 0 is the sentinel meaning "no sibling" (page 0 is always root).
 * During a leaf split we update both the old node's next_leaf and the
 * new node's next_leaf to maintain the linked list.
 */
uint32_t* leaf_node_next_leaf(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NEXT_LEAF_OFFSET);
}

uint32_t* leaf_node_prev_leaf(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_PREV_LEAF_OFFSET);
}

static void* leaf_node_cell(void* node, uint32_t cell_num) {
    return (char*)node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

uint32_t* leaf_node_key(void* node, uint32_t cell_num) {
    return (uint32_t*)leaf_node_cell(node, cell_num);
}

static void* leaf_node_value(void* node, uint32_t cell_num) {
    return (char*)leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE;
}

/* ─── Internal node accessors ────────────────────────────────── */

uint32_t* internal_node_num_keys(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

uint32_t* internal_node_right_child(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_HEADER_SIZE
                       + cell_num * INTERNAL_NODE_CELL_SIZE);
}

uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        printf("ERROR: child_num %u > num_keys %u\n", child_num, num_keys);
        exit(EXIT_FAILURE);
    }
    return (child_num == num_keys)
        ? internal_node_right_child(node)
        : internal_node_cell(node, child_num);
}

uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return (uint32_t*)((char*)internal_node_cell(node, key_num)
                       + INTERNAL_NODE_CHILD_SIZE);
}

/*
 * INTERNALS: FINDING THE CHILD INDEX
 * Returns a pointer to the child page number that should contain `key`.
 * We binary-search the separator keys and pick the leftmost child whose
 * separator key >= key.  If key is larger than all separators, we
 * follow right_child_ptr.
 */
uint32_t* internal_node_find_child(void* node, uint32_t key) {
    uint32_t num_keys = *internal_node_num_keys(node);
    uint32_t min_index = 0;
    uint32_t max_index = num_keys;

    while (min_index != max_index) {
        uint32_t index = (min_index + max_index) / 2;
        uint32_t key_to_right = *internal_node_key(node, index);
        if (key_to_right >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return internal_node_child(node, min_index);
}

/*
 * get_node_max_key — the largest key stored anywhere under this node.
 * For a leaf: it's the last cell's key (cells are sorted).
 * For an internal node: it's the last separator key (right subtree may
 * have larger keys, but the max separator is sufficient for our split
 * logic which only needs to update the parent's view of the max).
 */
uint32_t get_node_max_key(void* node) {
    switch (get_node_type(node)) {
        case NODE_LEAF:
            return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
        case NODE_INTERNAL:
            return *internal_node_key(node, *internal_node_num_keys(node) - 1);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 2 — Initialization helpers
 * ═══════════════════════════════════════════════════════════════ */

static void initialize_leaf_node(void* node) {
    set_node_type(node, NODE_LEAF);
    set_node_root(node, false);
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0; /* sentinel: no right sibling */
    *leaf_node_prev_leaf(node) = 0; /* sentinel: no left sibling  */
}

static void initialize_internal_node(void* node) {
    set_node_type(node, NODE_INTERNAL);
    set_node_root(node, false);
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = 0;
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 3 — Table lifecycle
 *
 * INTERNALS: DATABASE FILE FORMAT
 * ─────────────────────────────────
 * The .db file is a flat sequence of PAGE_SIZE (4096) byte pages.
 * Page 0 is always the B+ tree root (even if the root changes type
 * over time — see create_new_root).  The pager tracks how many pages
 * exist via the file length.
 * ═══════════════════════════════════════════════════════════════ */

static void table_init_catalog(Table* table);

Table* db_open(const char* filename) {
    Pager* pager = pager_open(filename);

    Table* table = (Table*)calloc(1, sizeof(Table));
    table->pager          = pager;
    table->root_page_num  = 0;
    table->in_transaction = false;
    table->username_index_enabled = false;
    table->username_index_dirty = false;
    table->username_index_entries = NULL;
    table->username_index_count = 0;
    table->username_index_capacity = 0;
    int catalog_len = snprintf(table->index_catalog_filename,
                               sizeof(table->index_catalog_filename),
                               "%s.catalog",
                               filename);
    int catalog_wal_len = snprintf(table->index_catalog_wal_filename,
                                   sizeof(table->index_catalog_wal_filename),
                                   "%s.catalog.wal",
                                   filename);
    int username_index_len = snprintf(table->username_index_filename,
                                      sizeof(table->username_index_filename),
                                      "%s.username.idx",
                                      filename);
    int username_index_wal_len = snprintf(table->username_index_wal_filename,
                                          sizeof(table->username_index_wal_filename),
                                          "%s.username.idx.wal",
                                          filename);
    if (catalog_len < 0 ||
        (size_t)catalog_len >= sizeof(table->index_catalog_filename) ||
        catalog_wal_len < 0 ||
        (size_t)catalog_wal_len >= sizeof(table->index_catalog_wal_filename) ||
        username_index_len < 0 ||
        (size_t)username_index_len >= sizeof(table->username_index_filename) ||
        username_index_wal_len < 0 ||
        (size_t)username_index_wal_len >= sizeof(table->username_index_wal_filename)) {
        printf("Database filename too long.\n");
        exit(EXIT_FAILURE);
    }
    table_load_index_catalog(table);
    table_init_catalog(table);

    if (pager->num_pages == 0) {
        /* Brand new database: initialize page 0 as an empty leaf root. */
        void* root_node = get_page(pager, 0);
        initialize_leaf_node(root_node);
        set_node_root(root_node, true);
        mark_page_dirty(pager, 0);
    }

    return table;
}

static void table_init_catalog(Table* table) {
    if (table->catalog.num_tables > 0) return;

    TableSchema* s = &table->catalog.schemas[0];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, "users", MAX_NAME_SIZE - 1);
    s->root_page_num = 0;
    s->num_columns = 3;

    strncpy(s->columns[0].name, "id", MAX_NAME_SIZE - 1);
    s->columns[0].type = COL_TYPE_INT;
    s->columns[0].size = ID_SIZE;
    s->columns[0].offset = ID_OFFSET;

    strncpy(s->columns[1].name, "username", MAX_NAME_SIZE - 1);
    s->columns[1].type = COL_TYPE_VARCHAR;
    s->columns[1].size = USERNAME_SIZE;
    s->columns[1].offset = USERNAME_OFFSET;

    strncpy(s->columns[2].name, "email", MAX_NAME_SIZE - 1);
    s->columns[2].type = COL_TYPE_VARCHAR;
    s->columns[2].size = EMAIL_SIZE;
    s->columns[2].offset = EMAIL_OFFSET;

    s->row_size = ROW_SIZE;
    table->catalog.num_tables = 1;
}

TableSchema* table_get_schema(Table* table, const char* name) {
    if (name != NULL && strlen(name) > 0) {
        for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
            if (strcmp(table->catalog.schemas[i].name, name) == 0) {
                return &table->catalog.schemas[i];
            }
        }
    }
    return &table->catalog.schemas[0];
}

bool table_create_table(Table* table, const char* name, uint32_t num_cols, char col_names[][32], char col_types[][16], bool has_fk, const char* fk_col, const char* fk_parent_table, const char* fk_parent_col, bool fk_on_delete_cascade) {
    if (table->catalog.num_tables >= MAX_TABLES) {
        printf("Error: Catalog full (max %d tables).\n", MAX_TABLES);
        return false;
    }
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) {
            printf("Error: Table '%s' already exists.\n", name);
            return false;
        }
    }

    uint32_t new_root_page = get_unused_page_num(table->pager);
    void* new_root_node = get_page(table->pager, new_root_page);
    initialize_leaf_node(new_root_node);
    set_node_root(new_root_node, true);
    mark_page_dirty(table->pager, new_root_page);

    TableSchema* s = &table->catalog.schemas[table->catalog.num_tables++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, MAX_NAME_SIZE - 1);
    s->root_page_num = new_root_page;
    s->num_columns = num_cols;
    s->has_fk = has_fk;
    s->fk_on_delete_cascade = fk_on_delete_cascade;
    if (has_fk) {
        if (fk_col) strncpy(s->fk_col, fk_col, MAX_NAME_SIZE - 1);
        if (fk_parent_table) strncpy(s->fk_parent_table, fk_parent_table, MAX_NAME_SIZE - 1);
        if (fk_parent_col) strncpy(s->fk_parent_col, fk_parent_col, MAX_NAME_SIZE - 1);
    }

    uint32_t offset = 0;
    for (uint32_t i = 0; i < num_cols; i++) {
        strncpy(s->columns[i].name, col_names[i], MAX_NAME_SIZE - 1);
        if (db_strcasecmp(col_types[i], "INT") == 0 ||
            db_strcasecmp(col_types[i], "INTEGER") == 0) {
            s->columns[i].type = COL_TYPE_INT;
            s->columns[i].size = 4;
        } else {
            s->columns[i].type = COL_TYPE_VARCHAR;
            s->columns[i].size = 256;
        }
        s->columns[i].offset = offset;
        offset += s->columns[i].size;
    }
    s->row_size = offset;
    return true;
}

void table_print_tables(Table* table) {
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        printf("%s\n", table->catalog.schemas[i].name);
    }
}

bool table_rename_table(Table* table, const char* old_name, const char* new_name) {
    /* Check that new name doesn't already exist */
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, new_name) == 0) {
            printf("Error: Table '%s' already exists.\n", new_name);
            return false;
        }
    }
    /* Find and rename */
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, old_name) == 0) {
            memset(table->catalog.schemas[i].name, 0, MAX_NAME_SIZE);
            strncpy(table->catalog.schemas[i].name, new_name, MAX_NAME_SIZE - 1);
            return true;
        }
    }
    printf("Error: Table '%s' not found.\n", old_name);
    return false;
}

bool table_add_column(Table* table, const char* table_name, const char* col_name, const char* col_type) {
    /* Reject for built-in 'users' table (hardcoded Row layout) */
    if (strcmp(table_name, "users") == 0) {
        printf("Error: Cannot add columns to built-in 'users' table (fixed row layout).\n");
        return false;
    }

    TableSchema* schema = NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, table_name) == 0) {
            schema = &table->catalog.schemas[i];
            break;
        }
    }
    if (schema == NULL) {
        printf("Error: Table '%s' not found.\n", table_name);
        return false;
    }
    if (schema->num_columns >= MAX_COLUMNS_PER_TABLE) {
        printf("Error: Table '%s' already has maximum columns (%d).\n", table_name, MAX_COLUMNS_PER_TABLE);
        return false;
    }
    /* Check for duplicate column name */
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (strcmp(schema->columns[i].name, col_name) == 0) {
            printf("Error: Column '%s' already exists in table '%s'.\n", col_name, table_name);
            return false;
        }
    }

    TableColumn* col = &schema->columns[schema->num_columns];
    memset(col, 0, sizeof(*col));
    strncpy(col->name, col_name, MAX_NAME_SIZE - 1);
    col->offset = schema->row_size;

    if (db_strcasecmp(col_type, "INT") == 0 || db_strcasecmp(col_type, "INTEGER") == 0) {
        col->type = COL_TYPE_INT;
        col->size = 4;
    } else {
        col->type = COL_TYPE_VARCHAR;
        col->size = 256;
    }

    schema->row_size += col->size;
    schema->num_columns++;
    return true;
}

void db_close(Table* table) {
    Pager* pager = table->pager;
    if (table->in_transaction) {
        pager_rollback(pager);
        table->in_transaction = false;
    }
    pager_checkpoint(pager); /* flush all pages and delete the WAL */

    free(table->username_index_entries);
    pager_close(pager);
    free(table);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 4 — Cursor operations
 *
 * INTERNALS: WHY A CURSOR?
 * ──────────────────────────
 * A cursor represents a position inside the table (a specific cell in a
 * specific leaf page).  It separates the "where am I?" state from the
 * B+ tree structure.  Multiple cursors could point to different rows
 * simultaneously (important for joins / iterators in more advanced DBs).
 * ═══════════════════════════════════════════════════════════════ */

static Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void*    node      = get_page(table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    Cursor* cursor    = malloc(sizeof(Cursor));
    cursor->table     = table;
    cursor->page_num  = page_num;
    cursor->end_of_table = false;

    /* Binary search for insertion point of `key` */
    uint32_t min_index        = 0;
    uint32_t one_past_max_index = num_cells;
    while (one_past_max_index != min_index) {
        uint32_t index         = (min_index + one_past_max_index) / 2;
        uint32_t key_at_index  = *leaf_node_key(node, index);
        if (key == key_at_index) {
            cursor->cell_num = index;
            return cursor;
        }
        if (key < key_at_index) {
            one_past_max_index = index;
        } else {
            min_index = index + 1;
        }
    }

    cursor->cell_num = min_index;
    return cursor;
}

static Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void*    node      = get_page(table->pager, page_num);
    uint32_t child_num = *internal_node_find_child(node, key);
    void*    child     = get_page(table->pager, child_num);

    switch (get_node_type(child)) {
        case NODE_LEAF:
            return leaf_node_find(table, child_num, key);
        case NODE_INTERNAL:
            return internal_node_find(table, child_num, key);
    }
    return NULL;
}

Cursor* table_find(Table* table, uint32_t key) {
    uint32_t root_page_num = table->root_page_num;
    void*    root_node     = get_page(table->pager, root_page_num);

    if (get_node_type(root_node) == NODE_LEAF) {
        return leaf_node_find(table, root_page_num, key);
    } else {
        return internal_node_find(table, root_page_num, key);
    }
}

Cursor* table_start(Table* table) {
    /* Find the position of the smallest key (key = 0 → leftmost leaf). */
    Cursor*  cursor    = table_find(table, 0);
    void*    node      = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    cursor->end_of_table = (num_cells == 0);
    return cursor;
}

void* cursor_value(Cursor* cursor) {
    void* page = get_page(cursor->table->pager, cursor->page_num);
    return leaf_node_value(page, cursor->cell_num);
}

/*
 * INTERNALS: MULTI-LEAF TRAVERSAL
 * ─────────────────────────────────
 * When we advance past the last cell in a leaf, we read next_leaf.
 * If it's non-zero, we move the cursor to cell 0 of that sibling.
 * If it's zero (no sibling), we mark end_of_table.
 * This is what makes a full-table SELECT O(n) instead of O(n log n).
 */
void cursor_advance(Cursor* cursor) {
    void* node = get_page(cursor->table->pager, cursor->page_num);
    cursor->cell_num += 1;

    /* Follow next_leaf pointers, skipping any empty leaves, until we land
     * on a cell or exhaust the chain. */
    while (cursor->cell_num >= *leaf_node_num_cells(node)) {
        uint32_t next_page = *leaf_node_next_leaf(node);
        if (next_page == 0) {
            cursor->end_of_table = true;
            return;
        }
        cursor->page_num = next_page;
        cursor->cell_num = 0;
        node = get_page(cursor->table->pager, next_page);
    }
}

/* Position a cursor at the last cell of the rightmost leaf (for DESC scans). */
Cursor* table_end(Table* table) {
    uint32_t page_num = table->root_page_num;
    void*    node     = get_page(table->pager, page_num);

    while (get_node_type(node) == NODE_INTERNAL) {
        page_num = *internal_node_right_child(node);
        node     = get_page(table->pager, page_num);
    }

    uint32_t num_cells = *leaf_node_num_cells(node);
    Cursor* cursor     = malloc(sizeof(Cursor));
    cursor->table        = table;
    cursor->page_num     = page_num;
    cursor->end_of_table = (num_cells == 0);
    cursor->cell_num     = (num_cells > 0) ? num_cells - 1 : 0;
    return cursor;
}

/* Move the cursor one step backwards (prev_leaf chain, right-to-left). */
void cursor_retreat(Cursor* cursor) {
    if (cursor->cell_num > 0) {
        cursor->cell_num--;
        return;
    }
    /* At cell 0 — must cross to the previous leaf. */
    void*    node      = get_page(cursor->table->pager, cursor->page_num);
    uint32_t prev_page = *leaf_node_prev_leaf(node);
    if (prev_page == 0) {
        cursor->end_of_table = true;
        return;
    }
    cursor->page_num = prev_page;
    node = get_page(cursor->table->pager, prev_page);

    /* Skip any empty leaves (guard against degenerate states). */
    while (*leaf_node_num_cells(node) == 0) {
        prev_page = *leaf_node_prev_leaf(node);
        if (prev_page == 0) { cursor->end_of_table = true; return; }
        cursor->page_num = prev_page;
        node = get_page(cursor->table->pager, prev_page);
    }
    cursor->cell_num = *leaf_node_num_cells(node) - 1;
}

static int compare_username_index_entries(const void* left, const void* right) {
    const UsernameIndexEntry* a = (const UsernameIndexEntry*)left;
    const UsernameIndexEntry* b = (const UsernameIndexEntry*)right;
    int name_cmp = strcmp(a->username, b->username);
    if (name_cmp != 0) return name_cmp;
    if (a->id < b->id) return -1;
    if (a->id > b->id) return 1;
    return 0;
}

static void sync_index_file(FILE* file) {
    if (fflush(file) != 0) {
        printf("Unable to flush index file.\n");
        exit(EXIT_FAILURE);
    }

#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        printf("Unable to sync index file.\n");
        exit(EXIT_FAILURE);
    }
#else
    if (fsync(fileno(file)) != 0) {
        printf("Unable to sync index file.\n");
        exit(EXIT_FAILURE);
    }
#endif
}

static bool index_catalog_is_valid(IndexCatalog* catalog) {
    return catalog->magic == INDEX_CATALOG_MAGIC &&
           catalog->version == INDEX_CATALOG_VERSION;
}

static bool username_index_header_is_valid(UsernameIndexFileHeader* header) {
    return header->magic == USERNAME_INDEX_MAGIC &&
           header->version == USERNAME_INDEX_VERSION &&
           header->count <= USERNAME_INDEX_MAX_ENTRIES;
}

static bool read_username_index_record(FILE* file, UsernameIndexEntry* entry) {
    if (fread(entry->username, 1, sizeof(entry->username), file) != sizeof(entry->username)) {
        return false;
    }
    entry->username[sizeof(entry->username) - 1] = '\0';
    return fread(&entry->id, sizeof(entry->id), 1, file) == 1;
}

static void write_username_index_record(FILE* file, UsernameIndexEntry* entry) {
    if (fwrite(entry->username, 1, sizeof(entry->username), file) != sizeof(entry->username) ||
        fwrite(&entry->id, sizeof(entry->id), 1, file) != 1) {
        printf("Unable to write username index record.\n");
        exit(EXIT_FAILURE);
    }
}

static bool read_username_index_payload(FILE* file,
                                        UsernameIndexEntry** entries,
                                        uint32_t* count) {
    UsernameIndexFileHeader header;
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        !username_index_header_is_valid(&header)) {
        return false;
    }

    UsernameIndexEntry* loaded = NULL;
    if (header.count > 0) {
        loaded = (UsernameIndexEntry*)malloc(header.count * sizeof(UsernameIndexEntry));
        if (loaded == NULL) {
            printf("Unable to allocate username index.\n");
            exit(EXIT_FAILURE);
        }
    }

    for (uint32_t i = 0; i < header.count; i++) {
        if (!read_username_index_record(file, &loaded[i])) {
            free(loaded);
            return false;
        }
    }

    *entries = loaded;
    *count = header.count;
    return true;
}

static void write_username_index_payload(FILE* file,
                                         UsernameIndexEntry* entries,
                                         uint32_t count) {
    UsernameIndexFileHeader header;
    header.magic = USERNAME_INDEX_MAGIC;
    header.version = USERNAME_INDEX_VERSION;
    header.count = count;

    if (fwrite(&header, sizeof(header), 1, file) != 1) {
        printf("Unable to write username index header.\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < count; i++) {
        write_username_index_record(file, &entries[i]);
    }
}

static void table_write_username_index_file(Table* table,
                                            UsernameIndexEntry* entries,
                                            uint32_t count) {
    FILE* file = fopen(table->username_index_filename, "wb");
    if (file == NULL) {
        printf("Unable to write username index file.\n");
        exit(EXIT_FAILURE);
    }

    write_username_index_payload(file, entries, count);
    sync_index_file(file);

    if (fclose(file) != 0) {
        printf("Unable to close username index file.\n");
        exit(EXIT_FAILURE);
    }
}

static void table_write_username_index_wal(Table* table,
                                           UsernameIndexEntry* entries,
                                           uint32_t count) {
    FILE* file = fopen(table->username_index_wal_filename, "wb");
    if (file == NULL) {
        printf("Unable to write username index WAL.\n");
        exit(EXIT_FAILURE);
    }

    write_username_index_payload(file, entries, count);
    uint32_t commit_magic = USERNAME_INDEX_WAL_COMMIT_MAGIC;
    if (fwrite(&commit_magic, sizeof(commit_magic), 1, file) != 1) {
        fclose(file);
        printf("Unable to write username index WAL.\n");
        exit(EXIT_FAILURE);
    }

    sync_index_file(file);

    if (fclose(file) != 0) {
        printf("Unable to close username index WAL.\n");
        exit(EXIT_FAILURE);
    }
}

static void table_recover_username_index_file(Table* table) {
    FILE* file = fopen(table->username_index_wal_filename, "rb");
    if (file == NULL) {
        return;
    }

    printf("Username index WAL found. Recovering...\n");

    UsernameIndexEntry* entries = NULL;
    uint32_t count = 0;
    uint32_t commit_magic = 0;
    bool complete = read_username_index_payload(file, &entries, &count) &&
                    fread(&commit_magic, sizeof(commit_magic), 1, file) == 1;
    fclose(file);

    if (!complete || commit_magic != USERNAME_INDEX_WAL_COMMIT_MAGIC) {
        printf("Ignoring incomplete username index WAL.\n");
        free(entries);
        if (remove(table->username_index_wal_filename) != 0) {
            printf("Unable to remove incomplete username index WAL.\n");
            exit(EXIT_FAILURE);
        }
        return;
    }

    table_write_username_index_file(table, entries, count);
    free(entries);

    if (remove(table->username_index_wal_filename) != 0) {
        printf("Unable to remove recovered username index WAL.\n");
        exit(EXIT_FAILURE);
    }
    printf("Username index recovery complete.\n");
}

static bool table_load_username_index_file(Table* table) {
    FILE* file = fopen(table->username_index_filename, "rb");
    if (file == NULL) {
        return false;
    }

    UsernameIndexEntry* entries = NULL;
    uint32_t count = 0;
    bool ok = read_username_index_payload(file, &entries, &count);
    fclose(file);

    if (!ok) {
        free(entries);
        return false;
    }

    if (count > 1) {
        qsort(entries,
              count,
              sizeof(UsernameIndexEntry),
              compare_username_index_entries);
    }

    free(table->username_index_entries);
    table->username_index_entries = entries;
    table->username_index_count = count;
    table->username_index_capacity = count;
    table->username_index_dirty = false;
    return true;
}

static void table_persist_username_index_file(Table* table) {
    table_write_username_index_wal(table,
                                   table->username_index_entries,
                                   table->username_index_count);
    table_write_username_index_file(table,
                                    table->username_index_entries,
                                    table->username_index_count);

    if (remove(table->username_index_wal_filename) != 0) {
        printf("Unable to remove committed username index WAL.\n");
        exit(EXIT_FAILURE);
    }
}

static void remove_index_file_if_exists(const char* filename,
                                        const char* error_message) {
    if (remove(filename) != 0 && errno != ENOENT) {
        printf("%s\n", error_message);
        exit(EXIT_FAILURE);
    }
}

static bool table_read_index_catalog_file(const char* filename,
                                          IndexCatalog* catalog) {
    FILE* file = fopen(filename, "rb");
    if (file == NULL) {
        return false;
    }

    size_t read_count = fread(catalog, sizeof(IndexCatalog), 1, file);
    fclose(file);

    return read_count == 1 && index_catalog_is_valid(catalog);
}

static void table_write_index_catalog_file(Table* table,
                                           IndexCatalog* catalog) {
    FILE* file = fopen(table->index_catalog_filename, "wb");
    if (file == NULL) {
        printf("Unable to write index catalog.\n");
        exit(EXIT_FAILURE);
    }

    if (fwrite(catalog, sizeof(IndexCatalog), 1, file) != 1) {
        fclose(file);
        printf("Unable to write index catalog.\n");
        exit(EXIT_FAILURE);
    }

    sync_index_file(file);

    if (fclose(file) != 0) {
        printf("Unable to close index catalog.\n");
        exit(EXIT_FAILURE);
    }
}

static void table_write_index_catalog_wal(Table* table,
                                          IndexCatalog* catalog) {
    FILE* file = fopen(table->index_catalog_wal_filename, "wb");
    if (file == NULL) {
        printf("Unable to write index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }

    uint32_t commit_magic = INDEX_CATALOG_WAL_COMMIT_MAGIC;
    if (fwrite(catalog, sizeof(IndexCatalog), 1, file) != 1 ||
        fwrite(&commit_magic, sizeof(commit_magic), 1, file) != 1) {
        fclose(file);
        printf("Unable to write index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }

    sync_index_file(file);

    if (fclose(file) != 0) {
        printf("Unable to close index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }
}

static void table_recover_index_catalog(Table* table) {
    FILE* file = fopen(table->index_catalog_wal_filename, "rb");
    if (file == NULL) {
        return;
    }

    printf("Index catalog WAL found. Recovering...\n");

    IndexCatalog catalog;
    uint32_t commit_magic = 0;
    bool complete = fread(&catalog, sizeof(IndexCatalog), 1, file) == 1 &&
                    fread(&commit_magic, sizeof(commit_magic), 1, file) == 1;
    fclose(file);

    if (!complete ||
        commit_magic != INDEX_CATALOG_WAL_COMMIT_MAGIC ||
        !index_catalog_is_valid(&catalog)) {
        printf("Ignoring incomplete index catalog WAL.\n");
        if (remove(table->index_catalog_wal_filename) != 0) {
            printf("Unable to remove incomplete index catalog WAL.\n");
            exit(EXIT_FAILURE);
        }
        return;
    }

    table_write_index_catalog_file(table, &catalog);
    if (remove(table->index_catalog_wal_filename) != 0) {
        printf("Unable to remove recovered index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }
    printf("Index catalog recovery complete.\n");
}

static void table_load_index_catalog(Table* table) {
    table_recover_username_index_file(table);
    table_recover_index_catalog(table);

    IndexCatalog catalog;
    if (!table_read_index_catalog_file(table->index_catalog_filename,
                                       &catalog)) {
        return;
    }

    if (catalog.username_index_enabled != 0) {
        table->username_index_enabled = true;
        if (catalog.username_index_entries_valid != 0 &&
            table_load_username_index_file(table)) {
            table->username_index_dirty = false;
        } else {
            table->username_index_dirty = true;
        }
    }

    table->num_sec_indexes = 0;
    for (uint32_t i = 0; i < catalog.num_indexes && i < MAX_INDEXES; i++) {
        if (catalog.indexes[i].enabled != 0) {
            GenericSecondaryIndex* idx = &table->sec_indexes[table->num_sec_indexes++];
            memset(idx, 0, sizeof(*idx));
            strncpy(idx->name, catalog.indexes[i].name, MAX_NAME_SIZE - 1);
            strncpy(idx->table_name, catalog.indexes[i].table_name, MAX_NAME_SIZE - 1);
            strncpy(idx->column_name, catalog.indexes[i].column_name, MAX_NAME_SIZE - 1);
            strncpy(idx->column_name2, catalog.indexes[i].column_name2, MAX_NAME_SIZE - 1);
            idx->num_columns = catalog.indexes[i].num_columns;
            idx->enabled = true;
            idx->dirty = true;
            build_index_filename(idx->index_filename, sizeof(idx->index_filename),
                                 table->pager->filename, idx->name, ".idx");
            build_index_filename(idx->index_wal_filename,
                                 sizeof(idx->index_wal_filename),
                                 table->pager->filename, idx->name, ".idx.wal");
        }
    }
}

static void table_save_index_catalog_state(Table* table, bool entries_valid) {
    IndexCatalog catalog;
    memset(&catalog, 0, sizeof(catalog));
    catalog.magic = INDEX_CATALOG_MAGIC;
    catalog.version = INDEX_CATALOG_VERSION;
    catalog.username_index_enabled = table->username_index_enabled ? 1u : 0u;
    catalog.username_index_entries_valid = entries_valid ? 1u : 0u;

    catalog.num_indexes = table->num_sec_indexes;
    for (uint32_t i = 0; i < table->num_sec_indexes && i < MAX_INDEXES; i++) {
        if (table->sec_indexes[i].enabled) {
            strncpy(catalog.indexes[i].name, table->sec_indexes[i].name, MAX_NAME_SIZE - 1);
            strncpy(catalog.indexes[i].table_name, table->sec_indexes[i].table_name, MAX_NAME_SIZE - 1);
            strncpy(catalog.indexes[i].column_name, table->sec_indexes[i].column_name, MAX_NAME_SIZE - 1);
            strncpy(catalog.indexes[i].column_name2, table->sec_indexes[i].column_name2, MAX_NAME_SIZE - 1);
            catalog.indexes[i].num_columns = table->sec_indexes[i].num_columns;
            catalog.indexes[i].enabled = 1u;
        }
    }

    table_write_index_catalog_wal(table, &catalog);
    table_write_index_catalog_file(table, &catalog);

    if (remove(table->index_catalog_wal_filename) != 0) {
        printf("Unable to remove committed index catalog WAL.\n");
        exit(EXIT_FAILURE);
    }
}

static void username_index_append(Table* table, Row* row) {
    if (table->username_index_count == table->username_index_capacity) {
        uint32_t new_capacity = table->username_index_capacity == 0
            ? 16
            : table->username_index_capacity * 2;
        UsernameIndexEntry* new_entries = realloc(
            table->username_index_entries,
            new_capacity * sizeof(UsernameIndexEntry)
        );
        if (new_entries == NULL) {
            printf("Unable to allocate username index.\n");
            exit(EXIT_FAILURE);
        }
        table->username_index_entries = new_entries;
        table->username_index_capacity = new_capacity;
    }

    UsernameIndexEntry* entry = &table->username_index_entries[table->username_index_count++];
    strncpy(entry->username, row->username, sizeof(entry->username) - 1);
    entry->username[sizeof(entry->username) - 1] = '\0';
    entry->id = row->id;
}

void table_mark_username_index_dirty(Table* table) {
    if (table->username_index_enabled) {
        table->username_index_dirty = true;
    }
}

void table_ensure_username_index(Table* table) {
    if (!table->username_index_enabled || !table->username_index_dirty) {
        return;
    }

    table->username_index_count = 0;

    Cursor* cursor = table_start(table);
    Row row;
    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row);
        username_index_append(table, &row);
        cursor_advance(cursor);
    }
    free(cursor);

    if (table->username_index_count > 1) {
        qsort(table->username_index_entries,
              table->username_index_count,
              sizeof(UsernameIndexEntry),
              compare_username_index_entries);
    }

    if (table->in_transaction) {
        return;
    }

    table_persist_username_index_file(table);
    table_save_index_catalog_state(table, true);
    table->username_index_dirty = false;
}

void table_create_username_index(Table* table) {
    table->username_index_enabled = true;
    table->username_index_dirty = true;
    table_ensure_username_index(table);
}

void table_drop_username_index(Table* table) {
    table->username_index_enabled = false;
    table->username_index_dirty = false;
    table->username_index_count = 0;
    table->username_index_capacity = 0;
    free(table->username_index_entries);
    table->username_index_entries = NULL;

    remove_index_file_if_exists(table->username_index_filename,
                                "Unable to remove username index file.");
    remove_index_file_if_exists(table->username_index_wal_filename,
                                "Unable to remove username index WAL.");
    table_save_index_catalog_state(table, false);
}

void table_prepare_username_index_commit(Table* table) {
    if (table->username_index_enabled && table->username_index_dirty) {
        table_save_index_catalog_state(table, false);
    }
}

void table_finalize_username_index_commit(Table* table) {
    if (table->username_index_enabled && table->username_index_dirty) {
        table_ensure_username_index(table);
    }
}

static void extract_column_value(Row* row, TableSchema* schema, const char* col_name, char* dest, size_t dest_size) {
    dest[0] = '\0';
    if (schema != NULL) {
        for (uint32_t i = 0; i < schema->num_columns; i++) {
            if (strcmp(schema->columns[i].name, col_name) == 0) {
                if (strcmp(col_name, "id") == 0) {
                    snprintf(dest, dest_size, "%u", row->id);
                } else if (strcmp(col_name, "username") == 0) {
                    strncpy(dest, row->username, dest_size - 1);
                    dest[dest_size - 1] = '\0';
                } else if (strcmp(col_name, "email") == 0) {
                    strncpy(dest, row->email, dest_size - 1);
                    dest[dest_size - 1] = '\0';
                }
                return;
            }
        }
    }
    if (strcmp(col_name, "username") == 0) {
        strncpy(dest, row->username, dest_size - 1);
        dest[dest_size - 1] = '\0';
    } else if (strcmp(col_name, "email") == 0) {
        strncpy(dest, row->email, dest_size - 1);
        dest[dest_size - 1] = '\0';
    } else if (strcmp(col_name, "id") == 0) {
        snprintf(dest, dest_size, "%u", row->id);
    }
}

static int compare_generic_index_entries(const void* a, const void* b) {
    const GenericIndexEntry* entry_a = (const GenericIndexEntry*)a;
    const GenericIndexEntry* entry_b = (const GenericIndexEntry*)b;
    int cmp = strcmp(entry_a->key_val, entry_b->key_val);
    if (cmp != 0) return cmp;
    if (entry_a->primary_key < entry_b->primary_key) return -1;
    if (entry_a->primary_key > entry_b->primary_key) return 1;
    return 0;
}

GenericSecondaryIndex* table_find_index_by_name(Table* table, const char* index_name) {
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        if (table->sec_indexes[i].enabled && strcmp(table->sec_indexes[i].name, index_name) == 0) {
            return &table->sec_indexes[i];
        }
    }
    return NULL;
}

GenericSecondaryIndex* table_find_index_by_column(Table* table, const char* table_name, const char* column_name) {
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        if (table->sec_indexes[i].enabled &&
            (table_name == NULL || strcmp(table->sec_indexes[i].table_name, table_name) == 0) &&
            strcmp(table->sec_indexes[i].column_name, column_name) == 0) {
            return &table->sec_indexes[i];
        }
    }
    return NULL;
}

GenericSecondaryIndex* table_find_composite_index(Table* table, const char* table_name, const char* col1, const char* col2) {
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* idx = &table->sec_indexes[i];
        if (idx->enabled && idx->num_columns == 2 &&
            (table_name == NULL || strcmp(idx->table_name, table_name) == 0) &&
            strcmp(idx->column_name, col1) == 0 &&
            strcmp(idx->column_name2, col2) == 0) {
            return idx;
        }
    }
    return NULL;
}

bool table_create_index(Table* table, const char* index_name, const char* table_name, const char* column_name, const char* column_name2) {
    if (table->num_sec_indexes >= MAX_INDEXES) {
        printf("Error: Maximum number of indexes (%d) reached.\n", MAX_INDEXES);
        return false;
    }

    if (table_find_index_by_name(table, index_name) != NULL) {
        printf("Error: Index %s already exists.\n", index_name);
        return false;
    }

    GenericSecondaryIndex* idx = &table->sec_indexes[table->num_sec_indexes++];
    memset(idx, 0, sizeof(*idx));
    strncpy(idx->name, index_name, sizeof(idx->name) - 1);
    strncpy(idx->table_name, table_name, sizeof(idx->table_name) - 1);
    strncpy(idx->column_name, column_name, sizeof(idx->column_name) - 1);
    if (column_name2 != NULL && column_name2[0] != '\0') {
        strncpy(idx->column_name2, column_name2, sizeof(idx->column_name2) - 1);
        idx->num_columns = 2;
    } else {
        idx->num_columns = 1;
    }
    idx->enabled = true;
    idx->dirty = true;

    build_index_filename(idx->index_filename, sizeof(idx->index_filename),
                         table->pager->filename, index_name, ".idx");
    build_index_filename(idx->index_wal_filename,
                         sizeof(idx->index_wal_filename),
                         table->pager->filename, index_name, ".idx.wal");

    if (strcmp(index_name, "idx_users_username") == 0) {
        table_create_username_index(table);
    }

    table_ensure_all_indexes(table);
    table_save_index_catalog_state(table, true);
    return true;
}

bool table_drop_index(Table* table, const char* index_name) {
    GenericSecondaryIndex* idx = table_find_index_by_name(table, index_name);
    if (idx == NULL) {
        if (strcmp(index_name, "idx_users_username") == 0 && table->username_index_enabled) {
            table_drop_username_index(table);
            return true;
        }
        printf("Error: Index %s does not exist.\n", index_name);
        return false;
    }

    idx->enabled = false;
    idx->dirty = false;
    idx->count = 0;
    idx->capacity = 0;
    if (idx->entries != NULL) {
        free(idx->entries);
        idx->entries = NULL;
    }

    remove_index_file_if_exists(idx->index_filename, "Unable to remove index file.");
    remove_index_file_if_exists(idx->index_wal_filename, "Unable to remove index WAL.");

    if (strcmp(index_name, "idx_users_username") == 0) {
        table_drop_username_index(table);
    }

    table_save_index_catalog_state(table, false);
    return true;
}

void table_ensure_all_indexes(Table* table) {
    table_ensure_username_index(table);

    TableSchema* schema = table_get_schema(table, "users");
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* idx = &table->sec_indexes[i];
        if (!idx->enabled || !idx->dirty) continue;

        idx->count = 0;
        Cursor* cursor = table_start(table);
        Row row;
        while (!cursor->end_of_table) {
            deserialize_row(cursor_value(cursor), &row);

            if (idx->count == idx->capacity) {
                uint32_t new_cap = idx->capacity == 0 ? 16 : idx->capacity * 2;
                GenericIndexEntry* new_entries = realloc(idx->entries, new_cap * sizeof(GenericIndexEntry));
                if (new_entries == NULL) {
                    printf("Unable to allocate memory for secondary index.\n");
                    exit(EXIT_FAILURE);
                }
                idx->entries = new_entries;
                idx->capacity = new_cap;
            }

            GenericIndexEntry* entry = &idx->entries[idx->count++];
            if (idx->num_columns == 2) {
                char val1[256] = "";
                char val2[256] = "";
                extract_column_value(&row, schema, idx->column_name, val1, sizeof(val1));
                extract_column_value(&row, schema, idx->column_name2, val2, sizeof(val2));
                snprintf(entry->key_val, sizeof(entry->key_val), "%s|%s", val1, val2);
            } else {
                extract_column_value(&row, schema, idx->column_name, entry->key_val, sizeof(entry->key_val));
            }
            entry->primary_key = row.id;

            cursor_advance(cursor);
        }
        free(cursor);

        if (idx->count > 1) {
            qsort(idx->entries, idx->count, sizeof(GenericIndexEntry), compare_generic_index_entries);
        }

        idx->dirty = false;
    }
}

void table_mark_indexes_dirty(Table* table) {
    table_mark_username_index_dirty(table);
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        if (table->sec_indexes[i].enabled) {
            table->sec_indexes[i].dirty = true;
        }
    }
}

void table_prepare_all_indexes_commit(Table* table) {
    table_prepare_username_index_commit(table);
}

void table_finalize_all_indexes_commit(Table* table) {
    table_finalize_username_index_commit(table);
    table_ensure_all_indexes(table);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 5 — Internal node: update parent separator key
 *
 * After inserting into a child, the parent's separator key for that
 * child may need to be updated if the child's max key changed (e.g.
 * because we inserted the largest key into a right sibling that was
 * just created).
 * ═══════════════════════════════════════════════════════════════ */

static void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
    uint32_t old_child_index = *internal_node_find_child(node, old_key);
    /* old_child_index is actually the VALUE of the child pointer; walk
     * through keys to find which separator to update. */
    uint32_t num_keys = *internal_node_num_keys(node);
    for (uint32_t i = 0; i < num_keys; i++) {
        if (*internal_node_key(node, i) == old_key) {
            *internal_node_key(node, i) = new_key;
            return;
        }
    }
    (void)old_child_index;
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 6 — Internal node split
 *
 * INTERNALS: INTERNAL NODE SPLIT
 * ────────────────────────────────
 * When an internal node is full (has INTERNAL_NODE_MAX_KEYS keys),
 * inserting another key requires splitting it.
 *
 * Split strategy:
 *  1. Create a new right internal node.
 *  2. The middle key is pushed UP to the parent (unlike leaf splits
 *     where the key is COPIED up).  This is the key difference between
 *     leaf splits and internal splits in a B+ tree.
 *  3. Left node keeps the left half of keys/children.
 *  4. Right node gets the right half.
 *  5. Recurse up if the parent is also full.
 * ═══════════════════════════════════════════════════════════════ */

/* Forward declaration */
static void internal_node_insert(Table* table, uint32_t parent_page_num,
                                  uint32_t child_page_num);

static void internal_node_split_and_insert(Table* table, uint32_t page_num,
                                            uint32_t child_page_num) {
    void*    old_node      = get_page(table->pager, page_num);
    uint32_t old_max       = get_node_max_key(old_node);
    mark_page_dirty(table->pager, page_num);

    void*    child         = get_page(table->pager, child_page_num);
    uint32_t child_max_key = get_node_max_key(child);

    uint32_t new_page_num  = get_unused_page_num(table->pager);
    void*    new_node      = get_page(table->pager, new_page_num);
    initialize_internal_node(new_node);
    mark_page_dirty(table->pager, new_page_num);

    bool splitting_root = is_node_root(old_node);

    /* Temporary scratch space: copy all current child ptrs + keys,
     * then rebuild left/right.  Max children = MAX_KEYS + 1, plus one
     * new child = MAX_KEYS + 2 total.  We'll use a local array. */
    uint32_t total_children = *internal_node_num_keys(old_node) + 2; /* +1 right child, +1 new */
    uint32_t* tmp_children  = malloc(total_children * sizeof(uint32_t));
    uint32_t* tmp_keys       = malloc((total_children - 1) * sizeof(uint32_t));

    /* Read existing children and keys from old_node */
    uint32_t num_keys = *internal_node_num_keys(old_node);
    uint32_t insert_idx = 0;

    /* Find insertion position for new child */
    for (insert_idx = 0; insert_idx <= num_keys; insert_idx++) {
        uint32_t existing_child = *internal_node_child(old_node, insert_idx);
        void* ec = get_page(table->pager, existing_child);
        uint32_t ec_max = get_node_max_key(ec);
        if (child_max_key < ec_max) break;
    }

    /* Build the merged, sorted arrays */
    uint32_t ci = 0; /* index into child_page_num */
    bool inserted = false;
    uint32_t out = 0;
    for (uint32_t i = 0; i <= num_keys; i++) {
        if (!inserted && out == insert_idx) {
            tmp_children[out] = child_page_num;
            inserted = true;
            if (i <= num_keys && i > 0) {
                /* separator key between previous child and new child */
                tmp_keys[out - 1] = child_max_key;
            }
            /* Don't increment i — re-process the current existing child */
            tmp_children[out + 1] = *internal_node_child(old_node, i);
            if (i < num_keys) {
                tmp_keys[out] = *internal_node_key(old_node, i);
            }
            out += 2;
            ci++;
        } else {
            tmp_children[out] = *internal_node_child(old_node, i);
            if (i < num_keys) tmp_keys[out - 1 + (out == 0 ? 1 : 0)] = *internal_node_key(old_node, i);
            out++;
        }
    }
    if (!inserted) {
        /* new child goes at the end */
        tmp_children[out] = child_page_num;
        tmp_keys[out - 1] = child_max_key;
        out++;
    }
    (void)ci;

    /* The above scratch-space approach is complex; use a simpler
     * in-place method instead: just call internal_node_insert which
     * inserts into old_node first, then split. */

    /* ── Simpler approach: insert into old first, then split ── */
    free(tmp_children);
    free(tmp_keys);

    /* Insert the new child into old_node (it's temporarily over-full by 1) */
    uint32_t cur_num_keys = *internal_node_num_keys(old_node);
    /* Find where the new child's separator key goes */
    uint32_t new_separator = child_max_key;
    /* Shift keys and children right to make room */
    uint32_t idx;
    for (idx = cur_num_keys; idx > 0; idx--) {
        if (*internal_node_key(old_node, idx - 1) > new_separator) {
            *internal_node_key(old_node, idx)   = *internal_node_key(old_node, idx - 1);
            *internal_node_child(old_node, idx + 1) = *internal_node_child(old_node, idx);
        } else {
            break;
        }
    }
    *internal_node_key(old_node, idx)       = new_separator;
    *internal_node_child(old_node, idx + 1) = *internal_node_right_child(old_node);
    *internal_node_right_child(old_node)    = child_page_num;
    *internal_node_num_keys(old_node)       = cur_num_keys + 1;

    /* Now split: left keeps LEFT_SPLIT_COUNT keys, middle key is pushed up,
     * right gets the remaining keys. */
    uint32_t left_count  = INTERNAL_NODE_LEFT_SPLIT_COUNT;
    uint32_t right_count = *internal_node_num_keys(old_node) - left_count - 1;
    uint32_t mid_key     = *internal_node_key(old_node, left_count);

    /* Copy right half into new_node */
    *internal_node_num_keys(new_node)      = right_count;
    *internal_node_right_child(new_node)   = *internal_node_right_child(old_node);

    for (uint32_t i = 0; i < right_count; i++) {
        *internal_node_child(new_node, i) =
            *internal_node_child(old_node, left_count + 1 + i);
        *internal_node_key(new_node, i) =
            *internal_node_key(old_node, left_count + 1 + i);
    }

    /* Update right_child of left (old) node */
    *internal_node_right_child(old_node) =
        *internal_node_child(old_node, left_count);
    *internal_node_num_keys(old_node)    = left_count;

    /* Update parent pointers for all children we moved to new_node */
    for (uint32_t i = 0; i <= right_count; i++) {
        uint32_t moved_child_page = *internal_node_child(new_node, i);
        void* moved_child = get_page(table->pager, moved_child_page);
        *node_parent(moved_child) = new_page_num;
        mark_page_dirty(table->pager, moved_child_page);
    }

    if (splitting_root) {
        /* Create a brand-new root that points to old_node (left) and new_node (right). */
        uint32_t left_page_num = get_unused_page_num(table->pager);
        void* left_node = get_page(table->pager, left_page_num);
        memcpy(left_node, old_node, PAGE_SIZE);
        set_node_root(left_node, false);
        mark_page_dirty(table->pager, left_page_num);

        initialize_internal_node(old_node);
        set_node_root(old_node, true);
        *internal_node_num_keys(old_node)    = 1;
        *internal_node_child(old_node, 0)    = left_page_num;
        *internal_node_key(old_node, 0)      = mid_key;
        *internal_node_right_child(old_node) = new_page_num;
        *node_parent(left_node) = table->root_page_num;
        *node_parent(new_node)  = table->root_page_num;
    } else {
        uint32_t parent_page_num = *node_parent(old_node);
        /* Update the old_node's separator in the parent */
        uint32_t new_max = get_node_max_key(old_node);
        void* parent = get_page(table->pager, parent_page_num);
        update_internal_node_key(parent, old_max, new_max);
        *node_parent(new_node) = parent_page_num;
        mark_page_dirty(table->pager, parent_page_num);
        internal_node_insert(table, parent_page_num, new_page_num);
    }
}

/*
 * internal_node_insert — insert a new child page number into the given
 * internal node, splitting if necessary.
 *
 * INTERNALS: INSERT INTO INTERNAL NODE
 * ──────────────────────────────────────
 * The new child's maximum key serves as the separator key.  We find the
 * correct slot (binary search), shift existing cells right, and write
 * the new (child_ptr, separator_key) pair.
 * If the node is already full we delegate to internal_node_split_and_insert.
 */
static void internal_node_insert(Table* table, uint32_t parent_page_num,
                                  uint32_t child_page_num) {
    void*    parent   = get_page(table->pager, parent_page_num);
    void*    child    = get_page(table->pager, child_page_num);
    uint32_t child_max_key = get_node_max_key(child);
    uint32_t num_keys = *internal_node_num_keys(parent);

    if (num_keys >= INTERNAL_NODE_MAX_KEYS) {
        internal_node_split_and_insert(table, parent_page_num, child_page_num);
        return;
    }

    /* Find insertion index: first key slot whose key >= child_max_key */
    uint32_t right_child_page_num = *internal_node_right_child(parent);
    void*    right_child          = get_page(table->pager, right_child_page_num);

    if (child_max_key > get_node_max_key(right_child)) {
        /* New child becomes the new right child; old right_child becomes a
         * regular interior child with key = its max key. */
        *internal_node_cell(parent, num_keys)  = right_child_page_num;
        *internal_node_key(parent, num_keys)   = get_node_max_key(right_child);
        *internal_node_right_child(parent)      = child_page_num;
    } else {
        uint32_t index = 0;
        while (index < num_keys &&
               *internal_node_key(parent, index) < child_max_key) {
            index++;
        }

        /* Shift cells right to make room for the new separator */
        for (uint32_t i = num_keys; i > index; i--) {
            void* dest = internal_node_cell(parent, i);
            void* src  = internal_node_cell(parent, i - 1);
            memcpy(dest, src, INTERNAL_NODE_CELL_SIZE);
        }
        *internal_node_cell(parent, index)  = child_page_num;
        *internal_node_key(parent, index)   = child_max_key;
    }

    *internal_node_num_keys(parent) = num_keys + 1;
    *node_parent(child) = parent_page_num;
    mark_page_dirty(table->pager, child_page_num);
    mark_page_dirty(table->pager, parent_page_num);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 7 — Root creation
 *
 * INTERNALS: HOW THE TREE GROWS TALLER
 * ──────────────────────────────────────
 * When the root (which starts as a leaf) overflows:
 *  1. We allocate a new left_child page and copy the old root there.
 *  2. We re-initialize the old root page as an internal node.
 *  3. The internal root now points to left_child (left) and the newly
 *     split right_child (right).
 * By always keeping the root at page 0, we avoid having to update any
 * pointer that stores "root_page_num" — it's always 0.
 * ═══════════════════════════════════════════════════════════════ */

static void create_new_root(Table* table, uint32_t right_child_page_num) {
    void*    root        = get_page(table->pager, table->root_page_num);
    void*    right_child = get_page(table->pager, right_child_page_num);
    mark_page_dirty(table->pager, table->root_page_num);

    uint32_t left_child_page_num = get_unused_page_num(table->pager);
    void*    left_child          = get_page(table->pager, left_child_page_num);
    mark_page_dirty(table->pager, left_child_page_num);

    memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);
    *node_parent(left_child) = table->root_page_num;

    /* After the copy, right_child->prev_leaf still points to old page 0
     * (now an internal root).  Fix it to point to the real left leaf. */
    if (get_node_type(left_child) == NODE_LEAF) {
        *leaf_node_prev_leaf(right_child) = left_child_page_num;
        mark_page_dirty(table->pager, right_child_page_num);
    }

    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root)      = 1;
    *internal_node_child(root, 0)      = left_child_page_num;
    *internal_node_key(root, 0)        = get_node_max_key(left_child);
    *internal_node_right_child(root)   = right_child_page_num;
    *node_parent(right_child)          = table->root_page_num;
    mark_page_dirty(table->pager, right_child_page_num);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 8 — Leaf node insert and split
 *
 * INTERNALS: LEAF SPLIT
 * ──────────────────────
 * When a leaf is full and we need to insert another cell:
 *  1. Allocate a new leaf page.
 *  2. Redistribute cells: left keeps LEFT_SPLIT_COUNT, right gets the rest.
 *  3. Wire the sibling pointers: old->next_leaf = new; new->next_leaf = old's old next.
 *  4. Push the right leaf's min key up to the parent.
 *     - If the old leaf was the root, call create_new_root.
 *     - Otherwise, call internal_node_insert(parent, new_page).
 * ═══════════════════════════════════════════════════════════════ */

void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value) {
    void*    old_node     = get_page(cursor->table->pager, cursor->page_num);
    uint32_t old_max      = get_node_max_key(old_node);
    mark_page_dirty(cursor->table->pager, cursor->page_num);

    uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
    void*    new_node     = get_page(cursor->table->pager, new_page_num);
    initialize_leaf_node(new_node);
    mark_page_dirty(cursor->table->pager, new_page_num);

    /* Maintain the doubly-linked sibling list.
     *  old_node <--> new_node <--> old_right (if any)            */
    uint32_t old_right = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(new_node) = old_right;
    *leaf_node_prev_leaf(new_node) = cursor->page_num; /* old_node's page */
    *leaf_node_next_leaf(old_node) = new_page_num;
    if (old_right != 0) {
        void* old_right_node = get_page(cursor->table->pager, old_right);
        *leaf_node_prev_leaf(old_right_node) = new_page_num;
        mark_page_dirty(cursor->table->pager, old_right);
    }

    /* Set parent pointer on new node */
    *node_parent(new_node) = *node_parent(old_node);

    /*
     * Redistribute: iterate over all existing cells + 1 new cell (total
     * LEAF_NODE_MAX_CELLS + 1) from right to left, placing each into
     * either the old (left) or new (right) node.
     */
    for (int32_t i = LEAF_NODE_MAX_CELLS; i >= 0; i--) {
        void*    destination_node;
        if (i >= (int32_t)LEAF_NODE_LEFT_SPLIT_COUNT) {
            destination_node = new_node;
        } else {
            destination_node = old_node;
        }
        uint32_t index_within_node = (uint32_t)i % LEAF_NODE_LEFT_SPLIT_COUNT;
        void*    destination       = leaf_node_cell(destination_node, index_within_node);

        if (i == (int32_t)cursor->cell_num) {
            /* This is the newly inserted cell */
            serialize_row(value, leaf_node_value(destination_node, index_within_node));
            *leaf_node_key(destination_node, index_within_node) = key;
        } else if (i > (int32_t)cursor->cell_num) {
            memcpy(destination, leaf_node_cell(old_node, (uint32_t)i - 1), LEAF_NODE_CELL_SIZE);
        } else {
            memcpy(destination, leaf_node_cell(old_node, (uint32_t)i), LEAF_NODE_CELL_SIZE);
        }
    }

    *(leaf_node_num_cells(old_node)) = LEAF_NODE_LEFT_SPLIT_COUNT;
    *(leaf_node_num_cells(new_node)) = LEAF_NODE_RIGHT_SPLIT_COUNT;

    if (is_node_root(old_node)) {
        create_new_root(cursor->table, new_page_num);
    } else {
        uint32_t parent_page_num = *node_parent(old_node);
        void*    parent          = get_page(cursor->table->pager, parent_page_num);
        uint32_t new_max         = get_node_max_key(old_node);

        update_internal_node_key(parent, old_max, new_max);
        *node_parent(new_node) = parent_page_num;
        internal_node_insert(cursor->table, parent_page_num, new_page_num);
    }
}

void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
    void*    node      = get_page(cursor->table->pager, cursor->page_num);
    mark_page_dirty(cursor->table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (num_cells >= LEAF_NODE_MAX_CELLS) {
        leaf_node_split_and_insert(cursor, key, value);
        return;
    }

    /* Shift cells right to open a slot for the new cell */
    if (cursor->cell_num < num_cells) {
        for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
            memcpy(leaf_node_cell(node, i),
                   leaf_node_cell(node, i - 1),
                   LEAF_NODE_CELL_SIZE);
        }
    }

    *(leaf_node_num_cells(node)) += 1;
    *(leaf_node_key(node, cursor->cell_num)) = key;
    serialize_row(value, leaf_node_value(node, cursor->cell_num));
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 8b — Empty-leaf removal helpers
 *
 * When a leaf loses its last cell it must be excised from the tree so
 * that internal-node routing and the leaf linked-list stay consistent.
 *
 * Strategy (simplified for educational purposes):
 *  1. Unlink the empty leaf from the next_leaf chain.
 *  2. Remove its slot from its parent internal node.
 *  3. If the parent is now a 0-key root, collapse it: its sole remaining
 *     child becomes the new root content (the tree shrinks by one level).
 *  4. Non-root internal nodes that become 0-key are left in place; their
 *     routing still works (internal_node_find_child always returns
 *     right_child when num_keys == 0), at the cost of one wasted level.
 * ═══════════════════════════════════════════════════════════════ */

/* Remove empty_page from the doubly-linked leaf chain in O(1). */
static void unlink_from_leaf_chain(Table* table, uint32_t empty_page) {
    void*    empty_node  = get_page(table->pager, empty_page);
    uint32_t predecessor = *leaf_node_prev_leaf(empty_node);
    uint32_t successor   = *leaf_node_next_leaf(empty_node);

    if (predecessor != 0) {
        void* pred = get_page(table->pager, predecessor);
        *leaf_node_next_leaf(pred) = successor;
        mark_page_dirty(table->pager, predecessor);
    }
    if (successor != 0) {
        void* succ = get_page(table->pager, successor);
        *leaf_node_prev_leaf(succ) = predecessor;
        mark_page_dirty(table->pager, successor);
    }
}

/* Remove child_page from parent's children array.
 *
 * Layout reminder:
 *   cells[0..(num_keys-1)] each hold (child_ptr, separator_key).
 *   right_child is stored separately in the header.
 *
 * Case A – removing a regular cell (child_idx < num_keys):
 *   Shift cells [child_idx+1 .. num_keys-1] left by one; decrement num_keys.
 *   right_child is unchanged.
 *
 * Case B – removing the right child (child_idx == num_keys):
 *   The previous last cell's child pointer becomes the new right_child;
 *   decrement num_keys (dropping that cell's separator key implicitly).
 *
 * Root collapse: if the parent is the root and ends up with 0 keys,
 *   copy its sole remaining child into page 0 so the root is always page 0.
 */
static void remove_child_from_parent(Table* table, uint32_t parent_page,
                                     uint32_t child_page) {
    void*    parent   = get_page(table->pager, parent_page);
    uint32_t num_keys = *internal_node_num_keys(parent);

    /* Locate child_page among the parent's children. */
    uint32_t child_idx = num_keys; /* assume right_child until found */
    for (uint32_t i = 0; i <= num_keys; i++) {
        if (*internal_node_child(parent, i) == child_page) {
            child_idx = i;
            break;
        }
    }

    if (child_idx == num_keys) {
        /* Case B: removing the right_child. */
        if (num_keys == 0) return; /* degenerate; shouldn't happen */
        *internal_node_right_child(parent) =
            *internal_node_child(parent, num_keys - 1);
    } else {
        /* Case A: shift cells left to fill the gap. */
        for (uint32_t i = child_idx; i < num_keys - 1; i++) {
            void* dst = internal_node_cell(parent, i);
            void* src = internal_node_cell(parent, i + 1);
            memcpy(dst, src, INTERNAL_NODE_CELL_SIZE);
        }
    }
    *internal_node_num_keys(parent) = num_keys - 1;
    mark_page_dirty(table->pager, parent_page);

    /* Root collapse: the tree shrinks by one level. */
    if (*internal_node_num_keys(parent) == 0 && is_node_root(parent)) {
        uint32_t sole_child_page = *internal_node_right_child(parent);
        void*    sole_child      = get_page(table->pager, sole_child_page);
        memcpy(parent, sole_child, PAGE_SIZE);
        set_node_root(parent, true);
        mark_page_dirty(table->pager, parent_page);

        /* sole_child_page is now orphaned — its content lives in page 0. */
        pager_free_page(table->pager, sole_child_page);

        /* If the new root is an internal node, update its children's
         * parent pointers to point back to page 0. */
        if (get_node_type(parent) == NODE_INTERNAL) {
            uint32_t nk = *internal_node_num_keys(parent);
            for (uint32_t i = 0; i <= nk; i++) {
                uint32_t child_p = *internal_node_child(parent, i);
                void*    child   = get_page(table->pager, child_p);
                *node_parent(child) = table->root_page_num;
                mark_page_dirty(table->pager, child_p);
            }
        }
    }
}

/* Called after a leaf loses its last cell.
 * If it is the root (single-leaf tree), leave it as an empty root —
 * the table is simply empty.  Otherwise excise it from the tree and
 * add the page to the free list for reuse. */
static void remove_empty_leaf(Table* table, uint32_t leaf_page) {
    void* leaf = get_page(table->pager, leaf_page);
    if (is_node_root(leaf)) return; /* empty table: root stays as empty leaf */

    unlink_from_leaf_chain(table, leaf_page);

    uint32_t parent_page = *node_parent(leaf);
    remove_child_from_parent(table, parent_page, leaf_page);

    /* leaf_page is now unreachable from the tree — reclaim it. */
    pager_free_page(table->pager, leaf_page);
}

void leaf_node_delete(Cursor* cursor) {
    uint32_t page_num  = cursor->page_num;
    void*    node      = get_page(cursor->table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    for (uint32_t i = cursor->cell_num; i < num_cells - 1; i++) {
        memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i + 1), LEAF_NODE_CELL_SIZE);
    }

    *leaf_node_num_cells(node) = num_cells - 1;
    mark_page_dirty(cursor->table->pager, page_num);

    if (num_cells - 1 == 0) {
        remove_empty_leaf(cursor->table, page_num);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 9 — Row serialization
 *
 * INTERNALS: ROW SERIALIZATION
 * ─────────────────────────────
 * We copy each field individually using memcpy to avoid struct padding
 * issues.  The on-disk layout is always:
 *   [id: 4 bytes][username: 33 bytes][email: 256 bytes]
 * regardless of how the compiler lays out the Row struct in memory.
 * ═══════════════════════════════════════════════════════════════ */

void serialize_row(Row* source, void* destination) {
    memcpy((char*)destination + ID_OFFSET,       &(source->id),       ID_SIZE);
    memcpy((char*)destination + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
    memcpy((char*)destination + EMAIL_OFFSET,    &(source->email),    EMAIL_SIZE);
}

void deserialize_row(void* source, Row* destination) {
    memcpy(&(destination->id),       (char*)source + ID_OFFSET,       ID_SIZE);
    memcpy(&(destination->username), (char*)source + USERNAME_OFFSET, USERNAME_SIZE);
    memcpy(&(destination->email),    (char*)source + EMAIL_OFFSET,    EMAIL_SIZE);
}

void print_row(Row* row) {
    printf("(%u, %s, %s)\n", row->id, row->username, row->email);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 11 — Table truncate
 *
 * Discards every page except the root and re-initialises page 0 as an
 * empty leaf.  Orphaned pages remain in the file but are unreachable
 * from the tree; subsequent inserts allocate fresh pages beyond them.
 * ═══════════════════════════════════════════════════════════════ */

void table_truncate(Table* table) {
    Pager* pager = table->pager;

    /* Drop all pages except the root, clear the free list, and truncate
     * the file on disk so reopening gives the correct page count. */
    pager_shrink(pager, 1);

    void* root = get_page(pager, 0);
    memset(root, 0, PAGE_SIZE);
    set_node_type(root, NODE_LEAF);
    set_node_root(root, true);
    /* num_cells and next_leaf are already 0 from memset */
    mark_page_dirty(pager, 0);
    table_mark_username_index_dirty(table);
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 10 — Tree printer
 *
 * INTERNALS: READING THE .btree OUTPUT
 * ──────────────────────────────────────
 * Prints the B+ tree structure for debugging / learning.  Example output
 * for a tree with 15 rows (after one root split):
 *
 *   - internal (size 1)
 *     - leaf (size 7)       ← left child, keys 1..7
 *       - 1
 *       - 2 ...
 *     - key 7               ← separator key
 *     - leaf (size 8)       ← right child, keys 8..15
 *       - 8 ...
 * ═══════════════════════════════════════════════════════════════ */

static void indent(uint32_t level) {
    for (uint32_t i = 0; i < level; i++) printf("  ");
}

void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level) {
    void*    node     = get_page(pager, page_num);
    uint32_t num_keys, child;

    switch (get_node_type(node)) {
        case NODE_LEAF:
            num_keys = *leaf_node_num_cells(node);
            indent(indentation_level);
            printf("- leaf (size %u)\n", num_keys);
            for (uint32_t i = 0; i < num_keys; i++) {
                indent(indentation_level + 1);
                printf("- %u\n", *leaf_node_key(node, i));
            }
            break;
        case NODE_INTERNAL:
            num_keys = *internal_node_num_keys(node);
            indent(indentation_level);
            printf("- internal (size %u)\n", num_keys);
            for (uint32_t i = 0; i < num_keys; i++) {
                child = *internal_node_child(node, i);
                print_tree(pager, child, indentation_level + 1);
                indent(indentation_level + 1);
                printf("- key %u\n", *internal_node_key(node, i));
            }
            child = *internal_node_right_child(node);
            print_tree(pager, child, indentation_level + 1);
            break;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * SECTION 12 — Stats, Page Inspection, and VACUUM
 * ═══════════════════════════════════════════════════════════════ */

void db_get_stats(Table* table, TableStats* stats) {
    memset(stats, 0, sizeof(*stats));
    stats->total_pages = table->pager->num_pages;
    stats->free_pages  = table->pager->free_page_count;

    Cursor* cursor = table_start(table);
    uint32_t current_page = 0xFFFFFFFF;
    while (!cursor->end_of_table) {
        stats->total_rows++;
        if (cursor->page_num != current_page) {
            stats->leaf_pages++;
            current_page = cursor->page_num;
        }
        cursor_advance(cursor);
    }
    if (stats->leaf_pages == 0 && stats->total_pages > 0) {
        stats->leaf_pages = 1;
    }

    for (uint32_t i = 0; i < table->pager->num_pages; i++) {
        bool is_free = false;
        for (uint32_t f = 0; f < table->pager->free_page_count; f++) {
            if (table->pager->free_pages[f] == i) { is_free = true; break; }
        }
        if (is_free) continue;
        void* page = get_page(table->pager, i);
        if (get_node_type(page) == NODE_INTERNAL) {
            stats->internal_pages++;
        }
    }
    free(cursor);
}

void print_page(Table* table, uint32_t page_num) {
    if (page_num >= table->pager->num_pages) {
        printf("Error: Page number %u out of bounds (total pages: %u).\n",
               page_num, table->pager->num_pages);
        return;
    }
    void* page = get_page(table->pager, page_num);
    NodeType type = get_node_type(page);
    bool is_root = is_node_root(page);
    uint32_t parent = *node_parent(page);

    printf("--- Page %u Details ---\n", page_num);
    printf("Type: %s\n", type == NODE_LEAF ? "LEAF" : "INTERNAL");
    printf("Is Root: %s\n", is_root ? "Yes" : "No");
    printf("Parent Page: %u\n", parent);

    if (type == NODE_LEAF) {
        uint32_t num_cells = *leaf_node_num_cells(page);
        uint32_t next_leaf = *leaf_node_next_leaf(page);
        uint32_t prev_leaf = *leaf_node_prev_leaf(page);
        printf("Num Cells: %u\n", num_cells);
        printf("Prev Leaf Page: %u\n", prev_leaf);
        printf("Next Leaf Page: %u\n", next_leaf);
        printf("Keys: ");
        for (uint32_t i = 0; i < num_cells; i++) {
            printf("%u%s", *leaf_node_key(page, i), (i + 1 < num_cells) ? ", " : "");
        }
        printf("\n");
    } else {
        uint32_t num_keys = *internal_node_num_keys(page);
        uint32_t right_child = *internal_node_right_child(page);
        printf("Num Keys: %u\n", num_keys);
        printf("Right Child Page: %u\n", right_child);
        printf("Keys & Children:\n");
        for (uint32_t i = 0; i < num_keys; i++) {
            printf("  Child[%u] -> Page %u | Key[%u] = %u\n",
                   i, *internal_node_child(page, i), i, *internal_node_key(page, i));
        }
        printf("  Child[%u] (Rightmost) -> Page %u\n", num_keys, right_child);
    }
}

void db_vacuum(Table* table) {
    uint32_t capacity = 16;
    uint32_t count = 0;
    Row* rows = malloc(sizeof(Row) * capacity);
    uint32_t version = db_get_user_version(table);

    Cursor* cursor = table_start(table);
    while (!cursor->end_of_table) {
        if (count >= capacity) {
            capacity *= 2;
            rows = realloc(rows, sizeof(Row) * capacity);
        }
        deserialize_row(cursor_value(cursor), &rows[count++]);
        cursor_advance(cursor);
    }
    free(cursor);

    table_truncate(table);

    for (uint32_t i = 0; i < count; i++) {
        Cursor* ins_cursor = table_find(table, rows[i].id);
        leaf_node_insert(ins_cursor, rows[i].id, &rows[i]);
        free(ins_cursor);
    }
    free(rows);

    db_set_user_version(table, version);

    pager_commit(table->pager);
    pager_checkpoint(table->pager);
}

void db_vacuum_into(Table* table, const char* dest_filename) {
    db_checkpoint(table);
    FILE* src = fopen(table->pager->filename, "rb");
    if (!src) return;
    FILE* dst = fopen(dest_filename, "wb");
    if (!dst) {
        fclose(src);
        return;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    fclose(src);
    fclose(dst);
    printf("Database backed up to '%s'.\n", dest_filename);
}

uint32_t db_get_user_version(Table* table) {
    if (table->pager->num_pages == 0) return 0;
    void* root = get_page(table->pager, table->root_page_num);
    return *node_parent(root);
}

void db_set_user_version(Table* table, uint32_t version) {
    void* root = get_page(table->pager, table->root_page_num);
    *node_parent(root) = version;
    mark_page_dirty(table->pager, table->root_page_num);
}

bool db_integrity_check(Table* table) {
    bool ok = true;
    Pager* pager = table->pager;

    if (pager->num_pages == 0) {
        printf("ok\n");
        return true;
    }

    void* root = get_page(pager, table->root_page_num);
    if (!is_node_root(root)) {
        printf("Error: Page %u is not marked as root.\n", table->root_page_num);
        ok = false;
    }

    for (uint32_t i = 0; i < pager->num_pages; i++) {
        bool is_free = false;
        for (uint32_t f = 0; f < pager->free_page_count; f++) {
            if (pager->free_pages[f] == i) { is_free = true; break; }
        }
        if (is_free) continue;

        void* page = get_page(pager, i);
        NodeType type = get_node_type(page);

        if (type == NODE_LEAF) {
            uint32_t num_cells = *leaf_node_num_cells(page);
            if (num_cells > LEAF_NODE_MAX_CELLS) {
                printf("Error: Leaf page %u has %u cells > max %u.\n", i, num_cells, (uint32_t)LEAF_NODE_MAX_CELLS);
                ok = false;
            }
            for (uint32_t c = 1; c < num_cells; c++) {
                if (*leaf_node_key(page, c - 1) >= *leaf_node_key(page, c)) {
                    printf("Error: Leaf page %u keys not sorted (%u >= %u).\n",
                           i, *leaf_node_key(page, c - 1), *leaf_node_key(page, c));
                    ok = false;
                }
            }
            uint32_t next = *leaf_node_next_leaf(page);
            if (next > 0 && next < pager->num_pages) {
                void* next_p = get_page(pager, next);
                if (*leaf_node_prev_leaf(next_p) != i) {
                    printf("Error: Leaf page %u next_leaf %u has broken prev_leaf link.\n", i, next);
                    ok = false;
                }
            }
        } else if (type == NODE_INTERNAL) {
            uint32_t num_keys = *internal_node_num_keys(page);
            if (num_keys > INTERNAL_NODE_MAX_KEYS) {
                printf("Error: Internal page %u has %u keys > max %u.\n", i, num_keys, (uint32_t)INTERNAL_NODE_MAX_KEYS);
                ok = false;
            }
        } else {
            printf("Error: Page %u has unknown node type %u.\n", i, (uint32_t)type);
            ok = false;
        }
    }

    if (ok) {
        printf("ok\n");
    }
    return ok;
}

void db_checkpoint(Table* table) {
    pager_checkpoint(table->pager);
    if (table->username_index_enabled) {
        table_prepare_username_index_commit(table);
        table_finalize_username_index_commit(table);
    }
    printf("Checkpoint complete.\n");
}

ViewSchema* table_find_view(Table* table, const char* name) {
    for (uint32_t i = 0; i < table->catalog.num_views; i++) {
        if (strcmp(table->catalog.views[i].name, name) == 0) {
            return &table->catalog.views[i];
        }
    }
    return NULL;
}

bool table_create_view(Table* table, const char* name, const char* select_sql) {
    if (table->catalog.num_views >= MAX_VIEWS) {
        printf("Error: View catalog full (max %d views).\n", MAX_VIEWS);
        return false;
    }
    if (table_find_view(table, name) != NULL) {
        printf("Error: View '%s' already exists.\n", name);
        return false;
    }
    ViewSchema* v = &table->catalog.views[table->catalog.num_views++];
    memset(v, 0, sizeof(*v));
    strncpy(v->name, name, MAX_NAME_SIZE - 1);
    strncpy(v->select_sql, select_sql, sizeof(v->select_sql) - 1);
    return true;
}

bool table_drop_view(Table* table, const char* name) {
    for (uint32_t i = 0; i < table->catalog.num_views; i++) {
        if (strcmp(table->catalog.views[i].name, name) == 0) {
            for (uint32_t j = i; j < table->catalog.num_views - 1; j++) {
                table->catalog.views[j] = table->catalog.views[j + 1];
            }
            table->catalog.num_views--;
            return true;
        }
    }
    printf("Error: View '%s' does not exist.\n", name);
    return false;
}

static void fts_add_token(FTSInvertedIndex* fts, const char* token, uint32_t doc_id) {
    if (token == NULL || token[0] == '\0') return;
    char lower_term[64];
    size_t i = 0;
    for (; token[i] && i < sizeof(lower_term) - 1; i++) {
        lower_term[i] = (char)tolower((unsigned char)token[i]);
    }
    lower_term[i] = '\0';

    for (uint32_t t = 0; t < fts->term_count; t++) {
        if (strcmp(fts->terms[t].term, lower_term) == 0) {
            for (uint32_t d = 0; d < fts->terms[t].doc_count; d++) {
                if (fts->terms[t].doc_ids[d] == doc_id) return;
            }
            if (fts->terms[t].doc_count < FTS_MAX_DOCS_PER_TERM) {
                fts->terms[t].doc_ids[fts->terms[t].doc_count++] = doc_id;
            }
            return;
        }
    }

    if (fts->term_count < FTS_MAX_TERMS) {
        FTSTermEntry* entry = &fts->terms[fts->term_count++];
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->term, lower_term, sizeof(entry->term) - 1);
        entry->doc_ids[0] = doc_id;
        entry->doc_count = 1;
    }
}

static void fts_tokenize_text(FTSInvertedIndex* fts, uint32_t doc_id, const char* text) {
    if (text == NULL) return;
    char buf[512];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* token = strtok(buf, " \t\n\r@.,;:-_");
    while (token != NULL) {
        fts_add_token(fts, token, doc_id);
        token = strtok(NULL, " \t\n\r@.,;:-_");
    }
}

void fts_build_index(Table* table) {
    memset(&table->fts_index, 0, sizeof(table->fts_index));
    Cursor* cursor = table_start(table);
    Row row;
    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row);
        fts_tokenize_text(&table->fts_index, row.id, row.username);
        fts_tokenize_text(&table->fts_index, row.id, row.email);
        cursor_advance(cursor);
    }
    free(cursor);
    table->fts_index.built = true;
}

uint32_t fts_search(Table* table, const char* keyword, uint32_t* out_doc_ids, uint32_t max_out) {
    fts_build_index(table);
    char lower_kw[64];
    size_t i = 0;
    for (; keyword[i] && i < sizeof(lower_kw) - 1; i++) {
        lower_kw[i] = (char)tolower((unsigned char)keyword[i]);
    }
    lower_kw[i] = '\0';

    uint32_t found = 0;
    for (uint32_t t = 0; t < table->fts_index.term_count; t++) {
        if (strstr(table->fts_index.terms[t].term, lower_kw) != NULL) {
            for (uint32_t d = 0; d < table->fts_index.terms[t].doc_count; d++) {
                uint32_t id = table->fts_index.terms[t].doc_ids[d];
                bool exists = false;
                for (uint32_t k = 0; k < found; k++) {
                    if (out_doc_ids[k] == id) { exists = true; break; }
                }
                if (!exists && found < max_out) {
                    out_doc_ids[found++] = id;
                }
            }
        }
    }
    return found;
}
