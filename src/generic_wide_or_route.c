#include "generic_predicate.h"
#include "generic_sql.h"

#include <ctype.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_range_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_or_scan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_parenthesized_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

static int wide_or_ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool wide_or_ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (wide_or_ci_char(*left) != wide_or_ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static TableSchema* wide_or_find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (wide_or_ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool wide_or_legacy_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           wide_or_ci_equal(schema->columns[0].name, "id") &&
           wide_or_ci_equal(schema->columns[1].name, "username") &&
           wide_or_ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool wide_or_schema_owned(const TableSchema* schema) {
    return schema != NULL && schema->row_size > ROW_SIZE &&
           !wide_or_legacy_shape(schema);
}

static bool wide_or_parse_projection(TinyDBGenericParser* parser) {
    if (tinydb_generic_consume_char(parser, '*')) return true;

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    return tinydb_generic_parse_identifier(parser, column, sizeof(column));
}

static bool wide_or_expression_has_or(TinyDBGenericParser* parser,
                                      const TableSchema* schema) {
    for (;;) {
        TinyDBGenericPredicate predicate;
        memset(&predicate, 0, sizeof(predicate));
        if (!tinydb_generic_parse_predicate(parser, schema, &predicate)) {
            return false;
        }
        if (tinydb_generic_consume_word(parser, "or")) return true;
        if (tinydb_generic_consume_word(parser, "and")) continue;
        return false;
    }
}

/*
 * The schema-sized equality/range layer sits above the historical generic OR
 * executor. Detect only SELECTs that target a wide schema and actually contain
 * an OR token, then delegate them to the mature OR parser/executor. This keeps
 * AND-only statements on the payload range route while allowing expressions
 * such as "a >= x AND a <= y OR b = z" to reach OR-of-AND evaluation instead
 * of being rejected by the narrower compound-AND front end.
 */
static bool is_wide_or_select(Table* table, const char* sql) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select") ||
        !wide_or_parse_projection(&parser) ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return false;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }
    TableSchema* schema = wide_or_find_schema(table, table_name);
    return wide_or_schema_owned(schema) &&
           tinydb_generic_consume_word(&parser, "where") &&
           wide_or_expression_has_or(&parser, schema);
}

static bool wide_or_skip_update_assignments(TinyDBGenericParser* parser,
                                            const TableSchema* schema) {
    for (;;) {
        char column_name[MAX_NAME_SIZE];
        if (!tinydb_generic_parse_identifier(parser,
                                             column_name,
                                             sizeof(column_name))) {
            return false;
        }
        int column_index = tinydb_generic_find_column_index(schema, column_name);
        if (column_index <= 0 || !tinydb_generic_consume_char(parser, '=')) {
            return false;
        }
        TinyDBValue value;
        memset(&value, 0, sizeof(value));
        if (!tinydb_generic_parse_value_for_column(
                parser, &schema->columns[column_index], &value)) {
            return false;
        }
        if (tinydb_generic_consume_word(parser, "where")) return true;
        if (!tinydb_generic_consume_char(parser, ',')) return false;
    }
}

static bool is_wide_or_update(Table* table, const char* sql) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "update")) return false;

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }
    TableSchema* schema = wide_or_find_schema(table, table_name);
    return wide_or_schema_owned(schema) &&
           tinydb_generic_consume_word(&parser, "set") &&
           wide_or_skip_update_assignments(&parser, schema) &&
           wide_or_expression_has_or(&parser, schema);
}

static bool is_wide_or_delete(Table* table, const char* sql) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "delete") ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return false;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }
    TableSchema* schema = wide_or_find_schema(table, table_name);
    return wide_or_schema_owned(schema) &&
           tinydb_generic_consume_word(&parser, "where") &&
           wide_or_expression_has_or(&parser, schema);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    if (table != NULL && sql != NULL) {
        if (is_wide_or_select(table, sql)) {
            TinyDBGenericSqlStatus status =
                tinydb_generic_sql_try_execute_or_scan_base(table, sql, result);
            if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;
        }
        if (is_wide_or_update(table, sql) || is_wide_or_delete(table, sql)) {
            TinyDBGenericSqlStatus status =
                tinydb_generic_sql_try_execute_parenthesized_base(table,
                                                                  sql,
                                                                  result);
            if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;
        }
    }
    return tinydb_generic_sql_try_execute_wide_range_base(table, sql, result);
}
