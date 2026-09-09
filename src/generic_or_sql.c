#include "generic_predicate.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>

#define GENERIC_OR_MAX_GROUPS MAX_COLUMNS_PER_TABLE
#define GENERIC_OR_MAX_TERMS MAX_COLUMNS_PER_TABLE

typedef struct {
    TinyDBGenericPredicate terms[GENERIC_OR_MAX_TERMS];
    uint32_t count;
} GenericOrGroup;

typedef struct {
    GenericOrGroup groups[GENERIC_OR_MAX_GROUPS];
    uint32_t count;
} GenericOrExpression;

typedef enum {
    OR_PROJECTION_STAR = 0,
    OR_PROJECTION_COUNT,
    OR_PROJECTION_COLUMN
} OrProjectionKind;

typedef struct {
    TableSchema* schema;
    GenericOrExpression expression;
    OrProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
    bool payload_native;
} GenericOrSelect;

typedef struct {
    const GenericOrSelect* select;
    uint32_t matched;
    uint32_t emitted;
    bool decode_failed;
} GenericOrSelectContext;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_or_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_or_base(
    const TinyDBGenericSelectPlan* plan);

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

static TinyDBGenericSqlStatus success(TinyDBGenericSqlResult* result) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
}

static bool parse_projection(TinyDBGenericParser* parser,
                             OrProjectionKind* kind,
                             char* column,
                             size_t column_size) {
    column[0] = '\0';
    if (tinydb_generic_consume_char(parser, '*')) {
        *kind = OR_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        *kind = OR_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    if (!tinydb_generic_parse_identifier(parser, column, column_size)) return false;
    *kind = OR_PROJECTION_COLUMN;
    return true;
}

static bool parse_or_expression(TinyDBGenericParser* parser,
                                const TableSchema* schema,
                                GenericOrExpression* expression,
                                bool* saw_or) {
    memset(expression, 0, sizeof(*expression));
    *saw_or = false;
    expression->count = 1;

    for (;;) {
        GenericOrGroup* group = &expression->groups[expression->count - 1u];
        if (group->count >= GENERIC_OR_MAX_TERMS ||
            !tinydb_generic_parse_predicate(parser,
                                            schema,
                                            &group->terms[group->count])) {
            return false;
        }
        group->count++;

        if (tinydb_generic_consume_word(parser, "and")) {
            continue;
        }
        if (tinydb_generic_consume_word(parser, "or")) {
            *saw_or = true;
            if (expression->count >= GENERIC_OR_MAX_GROUPS) return false;
            expression->count++;
            continue;
        }
        return true;
    }
}

static TinyDBGenericSqlStatus parse_or_select(Table* table,
                                               const char* sql,
                                               GenericOrSelect* select,
                                               TinyDBGenericSqlResult* result) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    memset(select, 0, sizeof(*select));

    if (!tinydb_generic_consume_word(&parser, "select")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char projection_column[MAX_NAME_SIZE];
    if (!parse_projection(&parser,
                          &select->projection_kind,
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

    select->schema = find_schema(table, table_name);
    if (select->schema == NULL || is_legacy_fixed_row_schema(select->schema)) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    select->payload_native = select->schema->row_size > ROW_SIZE;

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (select->payload_native) {
        if (!tinydb_record_payload_schema_supported(select->schema,
                                                    schema_message,
                                                    sizeof(schema_message))) {
            return execute_error(result, schema_message);
        }
    } else if (!tinydb_schema_supports_records(select->schema,
                                               schema_message,
                                               sizeof(schema_message))) {
        return execute_error(result, schema_message);
    }

    if (select->projection_kind == OR_PROJECTION_COLUMN) {
        int projection_index = tinydb_generic_find_column_index(
            select->schema, projection_column);
        if (projection_index < 0) {
            return syntax_error(
                result,
                "generic SELECT projection references an unknown column");
        }
        select->projection_column_index = (uint32_t)projection_index;
    }

    if (!tinydb_generic_consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    bool saw_or = false;
    if (!parse_or_expression(&parser,
                             select->schema,
                             &select->expression,
                             &saw_or)) {
        return syntax_error(
            result,
            "generic SELECT WHERE requires typed predicates joined by AND/OR");
    }
    if (!saw_or) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;

    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->limit)) {
            return syntax_error(result, "LIMIT requires an integer");
        }
        select->has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset")) {
            if (!tinydb_generic_parse_uint32(&parser, &select->offset)) {
                return syntax_error(result, "OFFSET requires an integer");
            }
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->offset)) {
            return syntax_error(result, "OFFSET requires an integer");
        }
    }

    if (!tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            "generic SELECT contains an unsupported clause after its OR predicate");
    }
    return TINYDB_GENERIC_SQL_SUCCESS;
}

static bool expression_matches(const GenericOrExpression* expression,
                               const TinyDBValue* values) {
    for (uint32_t i = 0; i < expression->count; i++) {
        const GenericOrGroup* group = &expression->groups[i];
        bool group_matches = true;
        for (uint32_t j = 0; j < group->count; j++) {
            const TinyDBGenericPredicate* predicate = &group->terms[j];
            if (!tinydb_generic_predicate_matches(
                    predicate, &values[predicate->column_index])) {
                group_matches = false;
                break;
            }
        }
        if (group_matches) return true;
    }
    return false;
}

static bool decode_values(const TableSchema* schema,
                          const TinyDBRecord* record,
                          TinyDBValue* values) {
    uint32_t value_count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_decode(schema,
                                record,
                                values,
                                MAX_COLUMNS_PER_TABLE,
                                &value_count,
                                message,
                                sizeof(message)) &&
           value_count == schema->num_columns;
}

static bool decode_payload_values(const TableSchema* schema,
                                  const TinyDBRecordPayload* payload,
                                  TinyDBValue* values) {
    uint32_t value_count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_payload_decode_values(schema,
                                               payload,
                                               values,
                                               MAX_COLUMNS_PER_TABLE,
                                               &value_count,
                                               message,
                                               sizeof(message)) &&
           value_count == schema->num_columns;
}

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static void print_payload_row(const TinyDBValue* values,
                              uint32_t value_count) {
    printf("(");
    for (uint32_t i = 0u; i < value_count; i++) {
        if (i > 0u) printf(", ");
        if (values[i].type == COL_TYPE_INT) {
            printf("%u", values[i].int_value);
        } else {
            printf("%s", values[i].text);
        }
    }
    printf(")\n");
}

static bool visit_or_record(const TableSchema* schema,
                            const TinyDBRecord* record,
                            void* raw_context) {
    GenericOrSelectContext* context = (GenericOrSelectContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    if (!decode_values(schema, record, values)) {
        context->decode_failed = true;
        return false;
    }
    if (!expression_matches(&context->select->expression, values)) return true;

    context->matched++;
    if (context->select->projection_kind == OR_PROJECTION_COUNT) return true;
    if (context->matched <= context->select->offset) return true;
    if (context->select->has_limit &&
        context->emitted >= context->select->limit) {
        return false;
    }

    if (context->select->projection_kind == OR_PROJECTION_COLUMN) {
        print_value(&values[context->select->projection_column_index]);
    } else {
        tinydb_record_print(schema, record);
    }
    context->emitted++;

    if (context->select->has_limit &&
        context->emitted >= context->select->limit) {
        return false;
    }
    return true;
}

static bool visit_or_payload(const TableSchema* schema,
                             const TinyDBRecordPayload* payload,
                             void* raw_context) {
    GenericOrSelectContext* context = (GenericOrSelectContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    if (!decode_payload_values(schema, payload, values)) {
        context->decode_failed = true;
        return false;
    }
    if (!expression_matches(&context->select->expression, values)) return true;

    context->matched++;
    if (context->select->projection_kind == OR_PROJECTION_COUNT ||
        context->matched <= context->select->offset ||
        (context->select->has_limit &&
         context->emitted >= context->select->limit)) {
        return true;
    }

    if (context->select->projection_kind == OR_PROJECTION_COLUMN) {
        print_value(&values[context->select->projection_column_index]);
    } else {
        print_payload_row(values, schema->num_columns);
    }
    context->emitted++;
    return true;
}

static TinyDBGenericSqlStatus execute_or_select(Table* table,
                                                 const char* sql,
                                                 TinyDBGenericSqlResult* result) {
    GenericOrSelect select;
    TinyDBGenericSqlStatus parsed = parse_or_select(table, sql, &select, result);
    if (parsed != TINYDB_GENERIC_SQL_SUCCESS) return parsed;

    GenericOrSelectContext context;
    memset(&context, 0, sizeof(context));
    context.select = &select;
    if (select.payload_native) {
        bool scan_complete = false;
        char message[TINYDB_RECORD_MESSAGE_MAX];
        message[0] = '\0';
        (void)tinydb_record_payload_scan(table,
                                        select.schema,
                                        visit_or_payload,
                                        &context,
                                        &scan_complete,
                                        message,
                                        sizeof(message));
        if (!scan_complete || context.decode_failed) {
            return execute_error(
                result,
                message[0] != '\0'
                    ? message
                    : "unable to decode schema-sized payload during OR predicate SELECT");
        }
    } else {
        (void)tinydb_record_scan(table, select.schema, visit_or_record, &context);
        if (context.decode_failed) {
            return execute_error(
                result,
                "unable to decode generic record during OR predicate SELECT");
        }
    }

    if (select.projection_kind == OR_PROJECTION_COUNT) {
        uint32_t count = context.matched;
        if (select.offset > 0) count = 0;
        if (select.has_limit && select.limit == 0) count = 0;
        printf("%u\n", count);
    }
    return success(result);
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

static bool append_predicate(char* output,
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

static bool build_expression_text(const GenericOrSelect* select,
                                  char* output,
                                  size_t output_size) {
    size_t length = 0;
    output[0] = '\0';
    for (uint32_t i = 0; i < select->expression.count; i++) {
        if (i > 0 && !append_text(output, output_size, &length, " OR ")) {
            return false;
        }
        const GenericOrGroup* group = &select->expression.groups[i];
        for (uint32_t j = 0; j < group->count; j++) {
            if (j > 0 && !append_text(output, output_size, &length, " AND ")) {
                return false;
            }
            if (!append_predicate(output,
                                  output_size,
                                  &length,
                                  select->schema,
                                  &group->terms[j])) {
                return false;
            }
        }
    }
    return true;
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    TinyDBGenericSqlStatus status = execute_or_select(table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    return tinydb_generic_sql_try_execute_index_base(table, sql, output);
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

    GenericOrSelect select;
    TinyDBGenericSqlStatus parsed = parse_or_select(table, sql, &select, output);
    if (parsed == TINYDB_GENERIC_SQL_SUCCESS) {
        plan->applicable = true;
        plan->kind = TINYDB_GENERIC_PLAN_FULL_SCAN;
        plan->root_page_num = select.schema->root_page_num;
        plan->has_filter = true;
        snprintf(plan->table_name, sizeof(plan->table_name), "%s", select.schema->name);
        if (select.projection_kind == OR_PROJECTION_STAR) {
            snprintf(plan->projection, sizeof(plan->projection), "*");
        } else if (select.projection_kind == OR_PROJECTION_COUNT) {
            snprintf(plan->projection, sizeof(plan->projection), "COUNT(*)");
        } else {
            snprintf(plan->projection,
                     sizeof(plan->projection),
                     "%s",
                     select.schema->columns[select.projection_column_index].name);
        }
        if (!build_expression_text(&select,
                                   plan->filter_expression,
                                   sizeof(plan->filter_expression))) {
            return syntax_error(output, "OR predicate plan text exceeds capacity");
        }
        output->status = TINYDB_GENERIC_SQL_SUCCESS;
        output->statement_type = STATEMENT_SELECT;
        output->statement_type_valid = true;
        output->execute_result = EXECUTE_SUCCESS;
        return output->status;
    }
    if (parsed != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return parsed;

    initialize_result(output);
    memset(plan, 0, sizeof(*plan));
    return tinydb_generic_sql_build_select_plan_or_base(
        table, sql, plan, output);
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_or_base(plan);
}
