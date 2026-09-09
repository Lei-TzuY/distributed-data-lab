#include "record_payload.h"
#include "table.h"

#include <ctype.h>

bool table_create_index_legacy(Table* table,
                               const char* index_name,
                               const char* table_name,
                               const char* column_name,
                               const char* column_name2);

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
    if (table == NULL || name == NULL || name[0] == '\0') return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static int find_column_index(const TableSchema* schema, const char* name) {
    if (schema == NULL || name == NULL || name[0] == '\0') return -1;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (ci_equal(schema->columns[i].name, name)) return (int)i;
    }
    return -1;
}

bool table_create_index(Table* table,
                        const char* index_name,
                        const char* table_name,
                        const char* column_name,
                        const char* column_name2) {
    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL || schema->root_page_num == 0) {
        return table_create_index_legacy(table,
                                         index_name,
                                         table_name,
                                         column_name,
                                         column_name2);
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(schema,
                                                schema_message,
                                                sizeof(schema_message))) {
        printf("Error: generic index target '%s' is not a supported schema-sized row layout.\n",
               table_name);
        return false;
    }

    if (column_name2 != NULL && column_name2[0] != '\0') {
        printf("Error: generic table secondary indexes currently support one column only.\n");
        return false;
    }

    int column_index = find_column_index(schema, column_name);
    if (column_index < 0) {
        printf("Error: Column %s does not exist on table %s.\n",
               column_name,
               table_name);
        return false;
    }
    if (column_index == 0) {
        printf("Error: generic table id is already backed by the primary B+ tree index.\n");
        return false;
    }

    return table_create_index_legacy(table,
                                     index_name,
                                     table_name,
                                     column_name,
                                     NULL);
}
