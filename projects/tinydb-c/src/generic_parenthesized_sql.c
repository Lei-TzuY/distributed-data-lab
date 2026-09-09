#include "generic_boolean.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    GROUPED_PROJECTION_STAR = 0,
    GROUPED_PROJECTION_COUNT,
    GROUPED_PROJECTION_COLUMN
} GroupedProjectionKind;

typedef struct {
    TableSchema* schema;
    TinyDBGenericBooleanExpression expression;
    GroupedProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} GroupedSelect;

typedef struct {
    uint32_t column_index;
    TinyDBValue value;
} GroupedAssignment;

typedef struct {
    const TinyDBGenericBooleanExpression* expression;
    uint32_t* ids;
    uint32_t count;
    uint32_t capacity;
    bool allocation_failed;
    bool decode_failed;
} GroupedCollector;

typedef struct {
    const GroupedSelect* select;
    uint32_t matched;
    uint32_t emitted;
    bool decode_failed;
} GroupedSelectContext;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_parenthesized_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_parenthesized_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_parenthesized_base(
    const TinyDBGenericSelectPlan* plan);

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static void set_message(TinyDBGenericSqlResult* result, const char* message) {
    snprintf(result->message, sizeof(result->message), "%s", message);
}

static TinyDBGenericSqlStatus syntax_error(TinyDBGenericSqlResult* result,
                                            StatementType type,
                                            const char* message) {
    result->status = TINYDB_GENERIC_SQL_SYNTAX_ERROR;
    result->statement_type = type;
    result->statement_type_valid = true;
    set_message(result, message);
    return result->status;
}

static TinyDBGenericSqlStatus execute_error(TinyDBGenericSqlResult* result,
                                             StatementType type,
                                             const char* message) {
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_KEY_NOT_FOUND;
    set_message(result, message);
    return result->status;
}

static TinyDBGenericSqlStatus success(TinyDBGenericSqlResult* result,
                                      StatementType type) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
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

static bool parse_projection(TinyDBGenericParser* parser,
                             GroupedProjectionKind* kind,
                             char* column,
                             size_t column_size) {
    column[0] = '\0';
    if (tinydb_generic_consume_char(parser, '*')) {
        *kind = GROUPED_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        *kind = GROUPED_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    if (!tinydb_generic_parse_identifier(parser, column, column_size)) return false;
    *kind = GROUPED_PROJECTION_COLUMN;
    return true;
}

static TinyDBGenericSqlStatus parse_grouped_select(
    Table* table,
    const char* sql,
    GroupedSelect* select,
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

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(select->schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return execute_error(result, STATEMENT_SELECT, schema_message);
    }

    if (select->projection_kind == GROUPED_PROJECTION_COLUMN) {
        int projection_index = tinydb_generic_find_column_index(
            select->schema, projection_column);
        if (projection_index < 0) {
            return syntax_error(
                result,
                STATEMENT_SELECT,
                "generic SELECT projection references an unknown column");
        }
        select->projection_column_index = (uint32_t)projection_index;
    }

    if (!tinydb_generic_consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    if (!tinydb_generic_parse_boolean_expression(&parser,
                                                 select->schema,
                                                 &select->expression)) {
        if (select->expression.saw_grouping) {
            return syntax_error(
                result,
                STATEMENT_SELECT,
                "generic SELECT contains an invalid parenthesized predicate");
        }
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    if (!select->expression.saw_grouping) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->limit)) {
            return syntax_error(result, STATEMENT_SELECT, "LIMIT requires an integer");
        }
        select->has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset")) {
            if (!tinydb_generic_parse_uint32(&parser, &select->offset)) {
                return syntax_error(result,
                                    STATEMENT_SELECT,
                                    "OFFSET requires an integer");
            }
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->offset)) {
            return syntax_error(result,
                                STATEMENT_SELECT,
                                "OFFSET requires an integer");
        }
    }

    if (!tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_SELECT,
            "generic SELECT contains an unsupported clause after its grouped predicate");
    }
    return TINYDB_GENERIC_SQL_SUCCESS;
}

static bool decode_values(const TableSchema* schema,
                          const TinyDBRecord* record,
                          TinyDBValue* values,
                          uint32_t* value_count) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    *value_count = 0;
    return tinydb_record_decode(schema,
                                record,
                                values,
                                MAX_COLUMNS_PER_TABLE,
                                value_count,
                                message,
                                sizeof(message)) &&
           *value_count == schema->num_columns;
}

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static bool visit_grouped_select_record(const TableSchema* schema,
                                        const TinyDBRecord* record,
                                        void* raw_context) {
    GroupedSelectContext* context = (GroupedSelectContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0;
    if (!decode_values(schema, record, values, &value_count)) {
        context->decode_failed = true;
        return false;
    }
    if (!tinydb_generic_boolean_matches(&context->select->expression,
                                        values,
                                        value_count)) {
        return true;
    }

    context->matched++;
    if (context->select->projection_kind == GROUPED_PROJECTION_COUNT) {
        return true;
    }
    if (context->matched <= context->select->offset) return true;
    if (context->select->has_limit &&
        context->emitted >= context->select->limit) {
        return false;
    }

    if (context->select->projection_kind == GROUPED_PROJECTION_COLUMN) {
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

static TinyDBGenericSqlStatus execute_grouped_select(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    GroupedSelect select;
    TinyDBGenericSqlStatus parsed = parse_grouped_select(table, sql, &select, result);
    if (parsed != TINYDB_GENERIC_SQL_SUCCESS) return parsed;

    GroupedSelectContext context;
    memset(&context, 0, sizeof(context));
    context.select = &select;
    (void)tinydb_record_scan(table,
                             select.schema,
                             visit_grouped_select_record,
                             &context);
    if (context.decode_failed) {
        return execute_error(
            result,
            STATEMENT_SELECT,
            "unable to decode generic record during grouped predicate SELECT");
    }

    if (select.projection_kind == GROUPED_PROJECTION_COUNT) {
        uint32_t count = context.matched;
        if (select.offset > 0) count = 0;
        if (select.has_limit && select.limit == 0) count = 0;
        printf("%u\n", count);
    }
    return success(result, STATEMENT_SELECT);
}

static bool parse_assignments(TinyDBGenericParser* parser,
                              const TableSchema* schema,
                              GroupedAssignment* assignments,
                              uint32_t* assignment_count) {
    *assignment_count = 0;
    while (*assignment_count < MAX_COLUMNS_PER_TABLE) {
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

        GroupedAssignment* assignment = &assignments[(*assignment_count)++];
        assignment->column_index = (uint32_t)column_index;
        if (!tinydb_generic_parse_value_for_column(
                parser,
                &schema->columns[column_index],
                &assignment->value)) {
            return false;
        }

        if (tinydb_generic_consume_word(parser, "where")) return true;
        if (!tinydb_generic_consume_char(parser, ',')) return false;
    }
    return false;
}

static bool append_id(GroupedCollector* collector, uint32_t id) {
    if (collector->count == collector->capacity) {
        uint32_t new_capacity = collector->capacity == 0
            ? 16u
            : collector->capacity * 2u;
        if (new_capacity < collector->capacity) {
            collector->allocation_failed = true;
            return false;
        }
        uint32_t* grown = (uint32_t*)realloc(
            collector->ids, (size_t)new_capacity * sizeof(uint32_t));
        if (grown == NULL) {
            collector->allocation_failed = true;
            return false;
        }
        collector->ids = grown;
        collector->capacity = new_capacity;
    }
    collector->ids[collector->count++] = id;
    return true;
}

static bool visit_collect_record(const TableSchema* schema,
                                 const TinyDBRecord* record,
                                 void* raw_context) {
    GroupedCollector* collector = (GroupedCollector*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0;
    if (!decode_values(schema, record, values, &value_count)) {
        collector->decode_failed = true;
        return false;
    }
    if (!tinydb_generic_boolean_matches(collector->expression,
                                        values,
                                        value_count)) {
        return true;
    }
    return append_id(collector, values[0].int_value);
}

static bool collect_matching_ids(
    Table* table,
    const TableSchema* schema,
    const TinyDBGenericBooleanExpression* expression,
    GroupedCollector* collector) {
    memset(collector, 0, sizeof(*collector));
    collector->expression = expression;
    (void)tinydb_record_scan(table, schema, visit_collect_record, collector);
    return !collector->allocation_failed && !collector->decode_failed;
}

static bool begin_internal_transaction(Table* table) {
    if (table->in_transaction) return false;
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    return true;
}

static void finish_internal_transaction(Table* table,
                                        bool started,
                                        bool mutation_succeeded) {
    if (!started) return;
    if (mutation_succeeded) {
        pager_commit(table->pager);
    } else {
        pager_rollback(table->pager);
    }
    table->in_transaction = false;
}

static TinyDBGenericSqlStatus apply_grouped_update(
    Table* table,
    TableSchema* schema,
    const GroupedAssignment* assignments,
    uint32_t assignment_count,
    const TinyDBGenericBooleanExpression* expression,
    TinyDBGenericSqlResult* result) {
    GroupedCollector collector;
    if (!collect_matching_ids(table, schema, expression, &collector)) {
        free(collector.ids);
        return execute_error(
            result,
            STATEMENT_UPDATE,
            "unable to collect grouped generic UPDATE targets");
    }
    if (collector.count == 0) {
        free(collector.ids);
        return success(result, STATEMENT_UPDATE);
    }

    bool started_transaction = begin_internal_transaction(table);
    bool mutation_succeeded = true;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';

    for (uint32_t i = 0; i < collector.count; i++) {
        uint32_t id = collector.ids[i];
        TinyDBRecord record;
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t value_count = 0;
        if (!tinydb_record_find(table, schema, id, &record) ||
            !tinydb_record_decode(schema,
                                  &record,
                                  values,
                                  MAX_COLUMNS_PER_TABLE,
                                  &value_count,
                                  message,
                                  sizeof(message)) ||
            value_count != schema->num_columns) {
            mutation_succeeded = false;
            break;
        }

        for (uint32_t assignment_index = 0;
             assignment_index < assignment_count;
             assignment_index++) {
            values[assignments[assignment_index].column_index] =
                assignments[assignment_index].value;
        }

        if (!tinydb_record_update(table,
                                  schema,
                                  id,
                                  values,
                                  value_count,
                                  message,
                                  sizeof(message))) {
            mutation_succeeded = false;
            break;
        }
    }

    finish_internal_transaction(table, started_transaction, mutation_succeeded);
    free(collector.ids);
    if (!mutation_succeeded) {
        return execute_error(
            result,
            STATEMENT_UPDATE,
            message[0] != '\0' ? message : "grouped generic UPDATE failed");
    }
    return success(result, STATEMENT_UPDATE);
}

static TinyDBGenericSqlStatus try_grouped_update(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "update")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
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
        return execute_error(result, STATEMENT_UPDATE, schema_message);
    }
    if (!tinydb_generic_consume_word(&parser, "set")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    GroupedAssignment assignments[MAX_COLUMNS_PER_TABLE];
    uint32_t assignment_count = 0;
    if (!parse_assignments(&parser,
                           schema,
                           assignments,
                           &assignment_count) ||
        assignment_count == 0) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    TinyDBGenericBooleanExpression expression;
    if (!tinydb_generic_parse_boolean_expression(&parser, schema, &expression)) {
        if (expression.saw_grouping) {
            return syntax_error(
                result,
                STATEMENT_UPDATE,
                "generic UPDATE contains an invalid parenthesized predicate");
        }
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    if (!expression.saw_grouping) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    if (!tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_UPDATE,
            "generic UPDATE contains an unsupported clause after its grouped predicate");
    }

    return apply_grouped_update(table,
                                schema,
                                assignments,
                                assignment_count,
                                &expression,
                                result);
}

static TinyDBGenericSqlStatus apply_grouped_delete(
    Table* table,
    TableSchema* schema,
    const TinyDBGenericBooleanExpression* expression,
    TinyDBGenericSqlResult* result) {
    GroupedCollector collector;
    if (!collect_matching_ids(table, schema, expression, &collector)) {
        free(collector.ids);
        return execute_error(
            result,
            STATEMENT_DELETE,
            "unable to collect grouped generic DELETE targets");
    }
    if (collector.count == 0) {
        free(collector.ids);
        return success(result, STATEMENT_DELETE);
    }

    bool started_transaction = begin_internal_transaction(table);
    bool mutation_succeeded = true;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';

    for (uint32_t i = 0; i < collector.count; i++) {
        if (!tinydb_record_delete(table,
                                  schema,
                                  collector.ids[i],
                                  message,
                                  sizeof(message))) {
            mutation_succeeded = false;
            break;
        }
    }

    finish_internal_transaction(table, started_transaction, mutation_succeeded);
    free(collector.ids);
    if (!mutation_succeeded) {
        return execute_error(
            result,
            STATEMENT_DELETE,
            message[0] != '\0' ? message : "grouped generic DELETE failed");
    }
    return success(result, STATEMENT_DELETE);
}

static TinyDBGenericSqlStatus try_grouped_delete(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "delete") ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
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
        return execute_error(result, STATEMENT_DELETE, schema_message);
    }
    if (!tinydb_generic_consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    TinyDBGenericBooleanExpression expression;
    if (!tinydb_generic_parse_boolean_expression(&parser, schema, &expression)) {
        if (expression.saw_grouping) {
            return syntax_error(
                result,
                STATEMENT_DELETE,
                "generic DELETE contains an invalid parenthesized predicate");
        }
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    if (!expression.saw_grouping) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    if (!tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_DELETE,
            "generic DELETE contains an unsupported clause after its grouped predicate");
    }

    return apply_grouped_delete(table, schema, &expression, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    TinyDBGenericSqlStatus status = execute_grouped_select(table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    status = try_grouped_update(table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    status = try_grouped_delete(table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    return tinydb_generic_sql_try_execute_parenthesized_base(table, sql, output);
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

    GroupedSelect select;
    TinyDBGenericSqlStatus parsed = parse_grouped_select(table,
                                                        sql,
                                                        &select,
                                                        output);
    if (parsed == TINYDB_GENERIC_SQL_SUCCESS) {
        plan->applicable = true;
        plan->kind = TINYDB_GENERIC_PLAN_FULL_SCAN;
        plan->root_page_num = select.schema->root_page_num;
        plan->has_filter = true;
        snprintf(plan->table_name,
                 sizeof(plan->table_name),
                 "%s",
                 select.schema->name);
        if (select.projection_kind == GROUPED_PROJECTION_STAR) {
            snprintf(plan->projection, sizeof(plan->projection), "*");
        } else if (select.projection_kind == GROUPED_PROJECTION_COUNT) {
            snprintf(plan->projection, sizeof(plan->projection), "COUNT(*)");
        } else {
            snprintf(plan->projection,
                     sizeof(plan->projection),
                     "%s",
                     select.schema->columns[select.projection_column_index].name);
        }
        if (!tinydb_generic_boolean_format(&select.expression,
                                           select.schema,
                                           plan->filter_expression,
                                           sizeof(plan->filter_expression))) {
            return syntax_error(
                output,
                STATEMENT_SELECT,
                "parenthesized predicate plan text exceeds capacity");
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
    return tinydb_generic_sql_build_select_plan_parenthesized_base(
        table, sql, plan, output);
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_parenthesized_base(plan);
}
