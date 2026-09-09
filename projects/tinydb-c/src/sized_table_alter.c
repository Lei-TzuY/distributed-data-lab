#include "column_type.h"
#include "leaf_format.h"
#include "record.h"
#include "record_payload.h"
#include "slotted_leaf_v2.h"
#include "table.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool table_add_column_legacy_base(Table* table,
                                  const char* table_name,
                                  const char* col_name,
                                  const char* col_type);

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (ci_char(*left) != ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool schema_table_is_empty(Table* table,
                                  const TableSchema* schema) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    bool scan_complete = false;
    uint32_t row_count = tinydb_record_payload_scan(table,
                                                    schema,
                                                    NULL,
                                                    NULL,
                                                    &scan_complete,
                                                    message,
                                                    sizeof(message));
    return scan_complete && row_count == 0u;
}

static bool schema_rows_accept_appended_column(
    Table* table,
    const TableSchema* schema,
    const TinyDBColumnTypeSpec* type) {
    if (table == NULL || schema == NULL || type == NULL ||
        schema->num_columns >= MAX_COLUMNS_PER_TABLE ||
        schema->row_size > TINYDB_RECORD_PAYLOAD_MAX ||
        type->storage_size > TINYDB_RECORD_PAYLOAD_MAX - schema->row_size) {
        return false;
    }

    TableSchema prospective = *schema;
    TableColumn* added = &prospective.columns[prospective.num_columns];
    memset(added, 0, sizeof(*added));
    added->type = type->type;
    added->offset = schema->row_size;
    added->size = type->storage_size;
    prospective.num_columns++;
    prospective.row_size += type->storage_size;

    char message[TINYDB_RECORD_MESSAGE_MAX];
    bool scan_complete = false;
    (void)tinydb_record_payload_scan(table,
                                     &prospective,
                                     NULL,
                                     NULL,
                                     &scan_complete,
                                     message,
                                     sizeof(message));
    return scan_complete;
}

static bool root_is_v2_tree(Table* table, const TableSchema* schema) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }
    void* root = get_page(table->pager, schema->root_page_num);
    if (!is_node_root(root) || *node_parent(root) != 0u) return false;
    if (get_node_type(root) == NODE_INTERNAL) return true;
    return get_node_type(root) == NODE_LEAF &&
           tinydb_leaf_format_detect_page(root, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(root, PAGE_SIZE);
}

static bool empty_root_can_become_v2(Table* table,
                                     const TableSchema* schema) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }

    void* root = get_page(table->pager, schema->root_page_num);
    if (get_node_type(root) != NODE_LEAF || !is_node_root(root) ||
        *node_parent(root) != 0u) {
        return false;
    }

    TinyDBLeafPageFormat format =
        tinydb_leaf_format_detect_page(root, PAGE_SIZE);
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return tinydb_slotted_leaf_v2_validate(root, PAGE_SIZE) &&
               tinydb_slotted_leaf_v2_count(root, PAGE_SIZE) == 0u;
    }
    if (format != TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) return false;

    return *leaf_node_num_cells(root) == 0u &&
           *leaf_node_next_leaf(root) == 0u &&
           *leaf_node_prev_leaf(root) == 0u;
}

static bool initialize_empty_v2_root(Table* table,
                                     const TableSchema* schema) {
    if (!empty_root_can_become_v2(table, schema)) return false;

    void* root = get_page(table->pager, schema->root_page_num);
    if (tinydb_leaf_format_detect_page(root, PAGE_SIZE) ==
        TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return true;
    }

    unsigned char staged[PAGE_SIZE];
    memset(staged, 0, sizeof(staged));
    if (!tinydb_slotted_leaf_v2_init(staged, sizeof(staged))) return false;
    set_node_root(staged, true);
    *node_parent(staged) = 0u;

    memcpy(root, staged, PAGE_USABLE_SIZE);
    mark_page_dirty(table->pager, schema->root_page_num);
    return true;
}

bool table_add_column(Table* table,
                      const char* table_name,
                      const char* col_name,
                      const char* col_type) {
    TinyDBColumnTypeSpec type;
    if (!tinydb_column_type_parse(col_type, &type) ||
        !((type.type == COL_TYPE_VARCHAR && type.explicitly_sized) ||
          type.type == COL_TYPE_INT)) {
        return table_add_column_legacy_base(table,
                                            table_name,
                                            col_name,
                                            col_type);
    }

    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL) {
        printf("Error: Table '%s' not found.\n", table_name);
        return false;
    }

    uint32_t old_row_size = schema->row_size;
    uint32_t old_num_columns = schema->num_columns;
    if (old_row_size > TINYDB_RECORD_PAYLOAD_MAX ||
        type.storage_size > TINYDB_RECORD_PAYLOAD_MAX - old_row_size) {
        printf("Error: ALTER TABLE ADD COLUMN would exceed the schema-sized payload limit.\n");
        return false;
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    bool executable_generic = tinydb_schema_supports_records(
        schema, schema_message, sizeof(schema_message));
    bool crosses_payload_boundary =
        executable_generic && old_row_size <= ROW_SIZE &&
        type.storage_size > ROW_SIZE - old_row_size;

    bool crossing_already_v2 = false;
    if (crosses_payload_boundary) {
        crossing_already_v2 = root_is_v2_tree(table, schema);
        if (crossing_already_v2) {
            if (!schema_rows_accept_appended_column(table, schema, &type)) {
                printf("Error: ALTER TABLE ADD COLUMN requires append-compatible compact V2 rows before crossing the fixed record boundary.\n");
                return false;
            }
        } else {
            if (!schema_table_is_empty(table, schema)) {
                printf("Error: ALTER TABLE ADD COLUMN would exceed the fixed generic record slot.\n");
                return false;
            }
            if (!empty_root_can_become_v2(table, schema)) {
                printf("Error: ALTER TABLE ADD COLUMN cannot safely migrate the empty table root to schema-sized storage.\n");
                return false;
            }
        }
    }
    if (old_row_size > ROW_SIZE &&
        !schema_rows_accept_appended_column(table, schema, &type)) {
        printf("Error: ALTER TABLE ADD COLUMN requires append-compatible compact V2 rows for a non-empty schema-sized payload table.\n");
        return false;
    }

    TableSchema previous_schema = *schema;

    /* The legacy catalog mutator owns duplicate/max-column/users checks. It
     * temporarily mutates the catalog entry; after success restore the
     * canonical compact physical layout from the shared type parser. Existing
     * compact V2 rows may retain an older append-only schema generation: the
     * row envelope validates their historical prefix fingerprint and decodes
     * missing trailing fields as zero/empty defaults. A crossing ALTER may
     * arrive with storage already migrated to a compact V2 leaf or multi-leaf
     * tree by the schema-aware publication wrapper; otherwise only a provably
     * empty fixed root is converted here. */
    if (!table_add_column_legacy_base(table,
                                      table_name,
                                      col_name,
                                      col_type)) {
        return false;
    }

    schema = find_schema(table, table_name);
    if (schema == NULL || schema->num_columns != old_num_columns + 1u) {
        return false;
    }

    TableColumn* added = &schema->columns[old_num_columns];
    added->type = type.type;
    added->offset = old_row_size;
    added->size = type.storage_size;
    schema->row_size = old_row_size + type.storage_size;

    if (crosses_payload_boundary && !crossing_already_v2 &&
        !initialize_empty_v2_root(table, schema)) {
        *schema = previous_schema;
        printf("Error: ALTER TABLE ADD COLUMN could not migrate the empty table root to schema-sized storage.\n");
        return false;
    }
    return true;
}
