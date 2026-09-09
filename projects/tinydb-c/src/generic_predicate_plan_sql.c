#include "generic_predicate.h"
#include "generic_sql.h"

#include <ctype.h>

typedef enum {
    PLAN_PROJECTION_STAR = 0,
    PLAN_PROJECTION_COUNT,
    PLAN_PROJECTION_COLUMN
} PlanProjectionKind;

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_predicate_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_predicate_base(
    const TinyDBGenericSelectPlan* plan);

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

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
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

static bool parse_projection_token(TinyDBGenericParser* parser,
                                   PlanProjectionKind* kind,
                                   char* column,
                                   size_t column_size) {
    column[0] = '\0';
    if (tinydb_generic_consume_char(parser, '*')) {
        *kind = PLAN_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        *kind = PLAN_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    if (!tinydb_generic_parse_identifier(parser, column, column_size)) return false;
    *kind = PLAN_PROJECTION_COLUMN;
    return true;
}

static bool append_text(char* output,
                        size_t output_size,
                        size_t* length,
                        const char* text) {
    while (*text != '\0') {
        if (*length + 1 >= output_size) return false;
        output[(*length)++] = *text++;
    }
    output[*length] = '\0';
    return true;
}

static bool append_predicate_text(
    char* output,
    size_t output_size,
    size_t* length,
    const TableSchema* schema,
    const TinyDBGenericPredicate* predicate) {
    if (!append_text(output,
                     output_size,
                     length,
                     schema->columns[predicate->column_index].name) ||
        !append_text(output, output_size, length, " ") ||
        !append_text(output,
                     output_size,
                     length,
                     tinydb_generic_compare_op_text(predicate->op)) ||
        !append_text(output, output_size, length, " ")) {
        return false;
    }

    if (predicate->value.type == COL_TYPE_INT) {
        char number[16];
        snprintf(number, sizeof(number), "%u", predicate->value.int_value);
        return append_text(output, output_size, length, number);
    }

    return append_text(output, output_size, length, "'") &&
           append_text(output, output_size, length, predicate->value.text) &&
           append_text(output, output_size, length, "'");
}

static bool contains_primary_key_equality(
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count) {
    for (uint32_t i = 0; i < predicate_count; i++) {
        if (predicates[i].column_index == 0 &&
            predicates[i].op == TINYDB_GENERIC_COMPARE_EQ) {
            return true;
        }
    }
    return false;
}

static TinyDBGenericSqlStatus try_build_compound_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    PlanProjectionKind projection_kind;
    char projection_column[MAX_NAME_SIZE];
    if (!parse_projection_token(&parser,
                                &projection_kind,
                                projection_column,
                                sizeof(projection_column))) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_consume_word(&parser, "from") ||
        !tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
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

    if (projection_kind == PLAN_PROJECTION_STAR) {
        snprintf(plan->projection, sizeof(plan->projection), "*");
    } else if (projection_kind == PLAN_PROJECTION_COUNT) {
        snprintf(plan->projection, sizeof(plan->projection), "COUNT(*)");
    } else {
        int projection_index =
            tinydb_generic_find_column_index(schema, projection_column);
        if (projection_index < 0) {
            return syntax_error(
                result,
                "generic SELECT projection references an unknown column");
        }
        snprintf(plan->projection,
                 sizeof(plan->projection),
                 "%s",
                 schema->columns[projection_index].name);
    }

    if (!tinydb_generic_consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t predicate_count = 0;
    if (!tinydb_generic_parse_predicate(&parser,
                                        schema,
                                        &predicates[predicate_count])) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    predicate_count++;

    bool saw_and = tinydb_generic_consume_word(&parser, "and");
    if (!saw_and && predicates[0].op != TINYDB_GENERIC_COMPARE_LIKE) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    if (saw_and) {
        do {
            if (predicate_count >= MAX_COLUMNS_PER_TABLE ||
                !tinydb_generic_parse_predicate(
                    &parser, schema, &predicates[predicate_count])) {
                return syntax_error(
                    result,
                    "generic SELECT WHERE requires typed predicates joined by AND");
            }
            predicate_count++;
        } while (tinydb_generic_consume_word(&parser, "and"));
    }

    if (tinydb_generic_consume_word(&parser, "limit")) {
        uint32_t ignored_limit = 0;
        if (!tinydb_generic_parse_uint32(&parser, &ignored_limit)) {
            return syntax_error(result, "LIMIT requires an integer");
        }
        if (tinydb_generic_consume_word(&parser, "offset")) {
            uint32_t ignored_offset = 0;
            if (!tinydb_generic_parse_uint32(&parser, &ignored_offset)) {
                return syntax_error(result, "OFFSET requires an integer");
            }
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        uint32_t ignored_offset = 0;
        if (!tinydb_generic_parse_uint32(&parser, &ignored_offset)) {
            return syntax_error(result, "OFFSET requires an integer");
        }
    }

    if (!tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            "generic SELECT contains an unsupported clause after its predicates");
    }

    plan->applicable = true;
    plan->kind = contains_primary_key_equality(predicates, predicate_count)
        ? TINYDB_GENERIC_PLAN_PRIMARY_KEY_LOOKUP
        : TINYDB_GENERIC_PLAN_FULL_SCAN;
    plan->root_page_num = schema->root_page_num;
    plan->has_filter = true;
    snprintf(plan->table_name, sizeof(plan->table_name), "%s", schema->name);

    const TinyDBGenericPredicate* first = &predicates[0];
    snprintf(plan->filter_column,
             sizeof(plan->filter_column),
             "%s",
             schema->columns[first->column_index].name);
    snprintf(plan->filter_operator,
             sizeof(plan->filter_operator),
             "%s",
             tinydb_generic_compare_op_text(first->op));
    if (first->value.type == COL_TYPE_INT) {
        snprintf(plan->filter_value,
                 sizeof(plan->filter_value),
                 "%u",
                 first->value.int_value);
    } else {
        size_t value_length = strlen(first->value.text);
        if (value_length + 3 > sizeof(plan->filter_value)) {
            return syntax_error(result, "generic filter value exceeds plan capacity");
        }
        plan->filter_value[0] = '\'';
        memcpy(plan->filter_value + 1, first->value.text, value_length);
        plan->filter_value[value_length + 1] = '\'';
        plan->filter_value[value_length + 2] = '\0';
    }

    size_t expression_length = 0;
    plan->filter_expression[0] = '\0';
    for (uint32_t i = 0; i < predicate_count; i++) {
        if (i > 0 &&
            !append_text(plan->filter_expression,
                         sizeof(plan->filter_expression),
                         &expression_length,
                         " AND ")) {
            return syntax_error(result, "compound predicate plan text exceeds capacity");
        }
        if (!append_predicate_text(plan->filter_expression,
                                   sizeof(plan->filter_expression),
                                   &expression_length,
                                   schema,
                                   &predicates[i])) {
            return syntax_error(result, "compound predicate plan text exceeds capacity");
        }
    }

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

    TinyDBGenericSqlStatus status =
        try_build_compound_plan(table, sql, plan, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    memset(plan, 0, sizeof(*plan));
    return tinydb_generic_sql_build_select_plan_predicate_base(
        table, sql, plan, output);
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable) return;
    if (plan->filter_expression[0] == '\0') {
        tinydb_generic_sql_print_plan_predicate_base(plan);
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
    printf("  FILTER: %s\n", plan->filter_expression);
}
