#include "multitable.h"
#include "record_payload.h"

#include <ctype.h>

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

static bool is_legacy_fixed_row_schema(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3 &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

bool multitable_index_target_supported(Table* table, const char* table_name) {
    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL) return false;

    /* Preserve the historical users/root-0 path exactly. */
    if (schema->root_page_num == 0) return true;

    /* Non-zero roots are allowed only for schema-aware generic records.
     * Fixed-Row lookalikes remain blocked because their legacy index
     * maintenance still assumes the primary users root. Payload-sized V2 rows
     * are valid index targets now that candidate rebuild and fetch paths stay
     * on the schema-sized carrier. */
    if (is_legacy_fixed_row_schema(schema)) return false;

    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_schema_supported(schema,
                                                  message,
                                                  sizeof(message));
}
