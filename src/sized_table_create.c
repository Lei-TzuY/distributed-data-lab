#include "column_type.h"
#include "record_payload.h"
#include "slotted_leaf_v2.h"
#include "table.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool table_create_table_legacy_base(Table* table,
                                    const char* name,
                                    uint32_t num_cols,
                                    char col_names[][32],
                                    char col_types[][16],
                                    bool has_fk,
                                    const char* fk_col,
                                    const char* fk_parent_table,
                                    const char* fk_parent_col,
                                    bool fk_on_delete_cascade);

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

static bool legacy_column_names(uint32_t num_cols, char col_names[][32]) {
    return num_cols == 3u &&
           ci_equal(col_names[0], "id") &&
           ci_equal(col_names[1], "username") &&
           ci_equal(col_names[2], "email");
}

static bool legacy_fixed_row_shape(uint32_t num_cols,
                                   char col_names[][32],
                                   const TinyDBColumnTypeSpec* types,
                                   bool recognized_all) {
    if (!recognized_all || !legacy_column_names(num_cols, col_names) ||
        types[0].type != COL_TYPE_INT ||
        types[1].type != COL_TYPE_VARCHAR ||
        types[2].type != COL_TYPE_VARCHAR) {
        return false;
    }

    if (!types[1].explicitly_sized && !types[2].explicitly_sized) return true;
    return types[1].explicitly_sized && types[2].explicitly_sized &&
           types[1].storage_size == USERNAME_SIZE &&
           types[2].storage_size == EMAIL_SIZE;
}

static bool validate_sized_layout(uint32_t num_cols,
                                  char col_types[][16],
                                  TinyDBColumnTypeSpec* types,
                                  bool* recognized_all,
                                  bool* has_explicit_width) {
    *recognized_all = true;
    *has_explicit_width = false;

    for (uint32_t i = 0; i < num_cols; i++) {
        if (!tinydb_column_type_parse(col_types[i], &types[i])) {
            memset(&types[i], 0, sizeof(types[i]));
            *recognized_all = false;
            continue;
        }
        if (types[i].explicitly_sized) *has_explicit_width = true;
    }
    return true;
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

static void normalize_legacy_row_schema(TableSchema* schema) {
    if (schema == NULL || schema->num_columns != 3u) return;

    schema->columns[0].type = COL_TYPE_INT;
    schema->columns[0].offset = ID_OFFSET;
    schema->columns[0].size = ID_SIZE;

    schema->columns[1].type = COL_TYPE_VARCHAR;
    schema->columns[1].offset = USERNAME_OFFSET;
    schema->columns[1].size = USERNAME_SIZE;

    schema->columns[2].type = COL_TYPE_VARCHAR;
    schema->columns[2].offset = EMAIL_OFFSET;
    schema->columns[2].size = EMAIL_SIZE;

    schema->row_size = ROW_SIZE;
}

static bool initialize_wide_v2_root(Table* table, const TableSchema* schema) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char staged[PAGE_SIZE];
    memset(staged, 0, sizeof(staged));
    if (!tinydb_slotted_leaf_v2_init(staged, sizeof(staged))) return false;
    set_node_root(staged, true);
    *node_parent(staged) = 0u;

    void* root = get_page(table->pager, schema->root_page_num);
    memcpy(root, staged, PAGE_USABLE_SIZE);
    mark_page_dirty(table->pager, schema->root_page_num);
    return true;
}

bool table_create_table(Table* table,
                        const char* name,
                        uint32_t num_cols,
                        char col_names[][32],
                        char col_types[][16],
                        bool has_fk,
                        const char* fk_col,
                        const char* fk_parent_table,
                        const char* fk_parent_col,
                        bool fk_on_delete_cascade) {
    if (table == NULL || name == NULL || num_cols == 0u ||
        num_cols > MAX_COLUMNS_PER_TABLE) {
        return false;
    }

    TinyDBColumnTypeSpec types[MAX_COLUMNS_PER_TABLE];
    memset(types, 0, sizeof(types));
    bool recognized_all = false;
    bool has_explicit_width = false;
    if (!validate_sized_layout(num_cols,
                               col_types,
                               types,
                               &recognized_all,
                               &has_explicit_width)) {
        return false;
    }

    const bool legacy_row = legacy_fixed_row_shape(num_cols,
                                                    col_names,
                                                    types,
                                                    recognized_all);
    const bool executable_generic =
        recognized_all && ci_equal(col_names[0], "id") &&
        types[0].type == COL_TYPE_INT && !legacy_row;
    uint32_t validated_row_size = 0u;
    if (executable_generic) {
        for (uint32_t i = 0; i < num_cols; i++) {
            if (validated_row_size > TINYDB_RECORD_PAYLOAD_MAX ||
                types[i].storage_size >
                    TINYDB_RECORD_PAYLOAD_MAX - validated_row_size) {
                printf("Error: CREATE TABLE row layout exceeds the schema-sized payload limit.\n");
                return false;
            }
            validated_row_size += types[i].storage_size;
        }
    }

    if (!table_create_table_legacy_base(table,
                                        name,
                                        num_cols,
                                        col_names,
                                        col_types,
                                        has_fk,
                                        fk_col,
                                        fk_parent_table,
                                        fk_parent_col,
                                        fk_on_delete_cascade)) {
        return false;
    }

    TableSchema* schema = find_schema(table, name);
    if (schema == NULL) return false;

    if (legacy_row) {
        /*
         * The historical base creator records bare VARCHAR as 256 bytes for
         * every text column, while the actual Row serializer is 4/33/256.
         * Normalize the catalog immediately so routing and ALTER policy can
         * recognize the ABI from physical metadata instead of column names.
         */
        normalize_legacy_row_schema(schema);
    } else if (has_explicit_width) {
        uint32_t offset = 0;
        for (uint32_t i = 0; i < num_cols; i++) {
            if (recognized_all || types[i].storage_size != 0u) {
                schema->columns[i].type = types[i].type;
                schema->columns[i].size = types[i].storage_size;
            }
            schema->columns[i].offset = offset;
            offset += schema->columns[i].size;
        }
        schema->row_size = offset;
    }

    if (executable_generic && schema->row_size > ROW_SIZE) {
        if (validated_row_size != schema->row_size ||
            !initialize_wide_v2_root(table, schema)) {
            printf("Error: unable to initialize the schema-sized V2 table root.\n");
            return false;
        }
    }
    return true;
}
