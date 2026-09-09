#include "generic_sql.h"

#include <ctype.h>
#include <errno.h>

typedef struct {
    const char* current;
} RangePlanParser;

typedef struct {
    uint32_t column_index;
    char op[TINYDB_GENERIC_PLAN_OPERATOR_MAX];
    TinyDBValue value;
} RangePlanPredicate;

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_base(const TinyDBGenericSelectPlan* plan);

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static TinyDBGenericSqlStatus syntax_error(TinyDBGenericSqlResult* result,
                                            const char* message) {
    result->status = TINYDB_GENERIC_SQL_SYNTAX_ERROR;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    snprintf(result->message, sizeof(result->message), "%s", message);
    return result->status;
}

static TinyDBGenericSqlStatus execute_error(TinyDBGenericSqlResult* result,
                                             const char* message) {
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_KEY_NOT_FOUND;
    snprintf(result->message, sizeof(result->message), "%s", message);
    return result->status;
}

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

static void skip_spaces(RangePlanParser* parser) {
    while (isspace((unsigned char)*parser->current)) parser->current++;
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool consume_word(RangePlanParser* parser, const char* word) {
    RangePlanParser backup = *parser;
    const char* expected = word;
    skip_spaces(parser);
    while (*expected != '\0' &&
           ci_char(*parser->current) == ci_char(*expected)) {
        parser->current++;
        expected++;
    }
    if (*expected != '\0' || is_identifier_char(*parser->current)) {
        *parser = backup;
        return false;
    }
    return true;
}

static bool consume_char(RangePlanParser* parser, char expected) {
    skip_spaces(parser);
    if (*parser->current != expected) return false;
    parser->current++;
    return true;
}

static bool parse_identifier(RangePlanParser* parser,
                             char* output,
                             size_t output_size) {
    skip_spaces(parser);
    if (!isalpha((unsigned char)*parser->current) && *parser->current != '_') {
        return false;
    }
    const char* start = parser->current;
    while (is_identifier_char(*parser->current)) parser->current++;
    size_t length = (size_t)(parser->current - start);
    if (length == 0 || length >= output_size) return false;
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool parse_uint32(RangePlanParser* parser, uint32_t* value) {
    skip_spaces(parser);
    if (!isdigit((unsigned char)*parser->current)) return false;
    char* end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(parser->current, &end, 10);
    if (errno == ERANGE || parsed > UINT32_MAX || end == parser->current) {
        return false;
    }
    parser->current = end;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_string(RangePlanParser* parser,
                         char* output,
                         size_t output_size) {
    skip_spaces(parser);
    if (*parser->current != '\'') return false;
    parser->current++;
    size_t length = 0;

    while (*parser->current != '\0') {
        char value = *parser->current++;
        if (value == '\'') {
            if (*parser->current == '\'') {
                parser->current++;
                value = '\'';
            } else {
                if (length >= output_size) return false;
                output[length] = '\0';
                return true;
            }
        }
        if (length + 1 >= output_size) return false;
        output[length++] = value;
    }
    return false;
}

static bool consume_end(RangePlanParser* parser) {
    skip_spaces(parser);
    if (*parser->current == ';') parser->current++;
    skip_spaces(parser);
    return *parser->current == '\0';
}

static TableSchema* find_schema(Table* table, const char* name) {
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static int find_column_index(const TableSchema* schema, const char* name) {
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (ci_equal(schema->columns[i].name, name)) return (int)i;
    }
    return -1;
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

static bool parse_projection(RangePlanParser* parser,
                             const TableSchema* schema,
                             char* output,
                             size_t output_size) {
    if (consume_char(parser, '*')) {
        snprintf(output, output_size, "*");
        return true;
    }

    RangePlanParser backup = *parser;
    if (consume_word(parser, "count") && consume_char(parser, '(') &&
        consume_char(parser, '*') && consume_char(parser, ')')) {
        snprintf(output, output_size, "COUNT(*)");
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = find_column_index(schema, column);
    if (column_index < 0) return false;
    snprintf(output, output_size, "%s", schema->columns[column_index].name);
    return true;
}

static bool parse_operator(RangePlanParser* parser,
                           char* output,
                           size_t output_size) {
    skip_spaces(parser);
    if (*parser->current != '<' && *parser->current != '>') return false;
    char first = *parser->current++;
    if (*parser->current == '=') {
        parser->current++;
        snprintf(output, output_size, "%c=", first);
    } else {
        snprintf(output, output_size, "%c", first);
    }
    return true;
}

static bool parse_value_for_column(RangePlanParser* parser,
                                   const TableColumn* column,
                                   TinyDBValue* value) {
    memset(value, 0, sizeof(*value));
    value->type = column->type;
    if (column->type == COL_TYPE_INT) {
        return parse_uint32(parser, &value->int_value);
    }
    return parse_string(parser, value->text, sizeof(value->text));
}

static bool parse_range_predicate(RangePlanParser* parser,
                                  const TableSchema* schema,
                                  RangePlanPredicate* predicate) {
    char column[MAX_NAME_SIZE];
    if (!parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = find_column_index(schema, column);
    if (column_index < 0 ||
        !parse_operator(parser, predicate->op, sizeof(predicate->op))) {
        return false;
    }
    predicate->column_index = (uint32_t)column_index;
    return parse_value_for_column(parser,
                                  &schema->columns[predicate->column_index],
                                  &predicate->value);
}

static void format_value(const TinyDBValue* value,
                         char* output,
                         size_t output_size) {
    if (value->type == COL_TYPE_INT) {
        snprintf(output, output_size, "%u", value->int_value);
    } else {
        snprintf(output, output_size, "'%s'", value->text);
    }
}

static TinyDBGenericSqlStatus try_build_range_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    RangePlanParser parser;
    parser.current = sql;
    if (!consume_word(&parser, "select")) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;

    /* Resolve the target table first without assuming the projection is valid. */
    RangePlanParser routing = parser;
    if (consume_char(&routing, '*')) {
        /* already positioned after projection */
    } else {
        RangePlanParser backup = routing;
        if (!(consume_word(&routing, "count") && consume_char(&routing, '(') &&
              consume_char(&routing, '*') && consume_char(&routing, ')'))) {
            routing = backup;
            char ignored_projection[MAX_NAME_SIZE];
            if (!parse_identifier(&routing,
                                  ignored_projection,
                                  sizeof(ignored_projection))) {
                return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
            }
        }
    }

    char table_name[MAX_NAME_SIZE];
    if (!consume_word(&routing, "from") ||
        !parse_identifier(&routing, table_name, sizeof(table_name))) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL || is_legacy_fixed_row_schema(schema)) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return execute_error(result, schema_message);
    }

    if (!parse_projection(&parser,
                          schema,
                          plan->projection,
                          sizeof(plan->projection))) {
        return syntax_error(result,
                            "generic SELECT projection references an unknown column");
    }
    if (!consume_word(&parser, "from") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name)) {
        return syntax_error(result,
                            "generic SELECT requires a catalog-backed table target");
    }
    if (!consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    RangePlanPredicate predicate;
    memset(&predicate, 0, sizeof(predicate));
    if (!parse_range_predicate(&parser, schema, &predicate)) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    if (consume_word(&parser, "limit")) {
        uint32_t ignored_limit = 0;
        if (!parse_uint32(&parser, &ignored_limit)) {
            return syntax_error(result, "LIMIT requires an integer");
        }
        if (consume_word(&parser, "offset")) {
            uint32_t ignored_offset = 0;
            if (!parse_uint32(&parser, &ignored_offset)) {
                return syntax_error(result, "OFFSET requires an integer");
            }
        }
    } else if (consume_word(&parser, "offset")) {
        uint32_t ignored_offset = 0;
        if (!parse_uint32(&parser, &ignored_offset)) {
            return syntax_error(result, "OFFSET requires an integer");
        }
    }

    if (!consume_end(&parser)) {
        return syntax_error(result,
                            "generic SELECT contains an unsupported clause");
    }

    plan->applicable = true;
    plan->kind = TINYDB_GENERIC_PLAN_FULL_SCAN;
    plan->root_page_num = schema->root_page_num;
    plan->has_filter = true;
    snprintf(plan->table_name, sizeof(plan->table_name), "%s", schema->name);
    snprintf(plan->filter_column,
             sizeof(plan->filter_column),
             "%s",
             schema->columns[predicate.column_index].name);
    snprintf(plan->filter_operator,
             sizeof(plan->filter_operator),
             "%s",
             predicate.op);
    format_value(&predicate.value,
                 plan->filter_value,
                 sizeof(plan->filter_value));

    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    return result->status;
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (plan != NULL) memset(plan, 0, sizeof(*plan));
    if (table == NULL || sql == NULL || plan == NULL) return output->status;

    TinyDBGenericSqlStatus range_status =
        try_build_range_plan(table, sql, plan, output);
    if (range_status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) {
        return range_status;
    }

    initialize_result(output);
    memset(plan, 0, sizeof(*plan));
    return tinydb_generic_sql_build_select_plan_base(table, sql, plan, output);
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable) return;

    if (plan->filter_operator[0] == '\0') {
        tinydb_generic_sql_print_plan_base(plan);
        return;
    }

    if (plan->kind == TINYDB_GENERIC_PLAN_PRIMARY_KEY_LOOKUP) {
        printf("PLAN: GENERIC PRIMARY KEY LOOKUP\n");
    } else {
        printf("PLAN: GENERIC SCHEMA-AWARE TABLE SCAN\n");
    }
    printf("  TABLE: %s (root page %u)\n",
           plan->table_name,
           plan->root_page_num);
    printf("  PROJECTION: %s\n", plan->projection);
    if (plan->has_filter) {
        printf("  FILTER: %s %s %s\n",
               plan->filter_column,
               plan->filter_operator,
               plan->filter_value);
    } else {
        printf("  FILTER: none\n");
    }
}
