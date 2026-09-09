#include "generic_boolean.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    WIDE_GROUPED_PROJECTION_STAR = 0,
    WIDE_GROUPED_PROJECTION_COUNT,
    WIDE_GROUPED_PROJECTION_COLUMN
} WideGroupedProjectionKind;

typedef struct {
    TableSchema* schema;
    TinyDBGenericBooleanExpression expression;
    WideGroupedProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
    uint32_t matched;
    uint32_t emitted;
    bool decode_failed;
} WideGroupedSelectContext;

typedef struct {
    uint32_t column_index;
    TinyDBValue value;
} WideGroupedAssignment;

typedef struct {
    const TableSchema* schema;
    const TinyDBGenericBooleanExpression* expression;
    uint32_t* ids;
    uint32_t count;
    uint32_t capacity;
    bool allocation_failed;
    bool decode_failed;
} WideGroupedCollector;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_or_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static TinyDBGenericSqlStatus grouped_error(TinyDBGenericSqlResult* result,
                                            StatementType type,
                                            TinyDBGenericSqlStatus status,
                                            ExecuteResult execute_result,
                                            const char* message) {
    result->status = status;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = execute_result;
    result->executed = false;
    snprintf(result->message,
             sizeof(result->message),
             "%s",
             message != NULL ? message : "wide grouped SQL failed");
    return status;
}

static TinyDBGenericSqlStatus grouped_success(TinyDBGenericSqlResult* result,
                                              StatementType type) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    result->message[0] = '\0';
    return result->status;
}

static int grouped_ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool grouped_ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (grouped_ci_char(*left) != grouped_ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static TableSchema* grouped_find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (grouped_ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool grouped_legacy_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           grouped_ci_equal(schema->columns[0].name, "id") &&
           grouped_ci_equal(schema->columns[1].name, "username") &&
           grouped_ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool grouped_wide_schema(const TableSchema* schema) {
    return schema != NULL && schema->row_size > ROW_SIZE &&
           !grouped_legacy_shape(schema);
}

static bool parse_projection(TinyDBGenericParser* parser,
                             WideGroupedProjectionKind* kind,
                             char* column,
                             size_t column_size) {
    column[0] = '\0';
    if (tinydb_generic_consume_char(parser, '*')) {
        *kind = WIDE_GROUPED_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        *kind = WIDE_GROUPED_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    if (!tinydb_generic_parse_identifier(parser, column, column_size)) return false;
    *kind = WIDE_GROUPED_PROJECTION_COLUMN;
    return true;
}

static bool parse_limit_offset(TinyDBGenericParser* parser,
                               bool* has_limit,
                               uint32_t* limit,
                               uint32_t* offset) {
    *has_limit = false;
    *limit = 0u;
    *offset = 0u;
    if (tinydb_generic_consume_word(parser, "limit")) {
        if (!tinydb_generic_parse_uint32(parser, limit)) return false;
        *has_limit = true;
        if (tinydb_generic_consume_word(parser, "offset") &&
            !tinydb_generic_parse_uint32(parser, offset)) {
            return false;
        }
        return true;
    }
    if (tinydb_generic_consume_word(parser, "offset")) {
        return tinydb_generic_parse_uint32(parser, offset);
    }
    return true;
}

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static void print_row_values(const TinyDBValue* values, uint32_t value_count) {
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

static bool visit_grouped_select_payload(const TableSchema* schema,
                                         const TinyDBRecordPayload* payload,
                                         void* raw_context) {
    WideGroupedSelectContext* context =
        (WideGroupedSelectContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_decode_values(schema,
                                             payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &value_count,
                                             message,
                                             sizeof(message)) ||
        value_count != schema->num_columns) {
        context->decode_failed = true;
        return false;
    }
    if (!tinydb_generic_boolean_matches(&context->expression,
                                        values,
                                        value_count)) {
        return true;
    }

    context->matched++;
    if (context->projection_kind == WIDE_GROUPED_PROJECTION_COUNT ||
        context->matched <= context->offset ||
        (context->has_limit && context->emitted >= context->limit)) {
        return true;
    }

    if (context->projection_kind == WIDE_GROUPED_PROJECTION_COLUMN) {
        print_value(&values[context->projection_column_index]);
    } else {
        print_row_values(values, value_count);
    }
    context->emitted++;
    return true;
}

static TinyDBGenericSqlStatus try_grouped_select(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result,
    bool* applicable) {
    *applicable = false;
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    WideGroupedProjectionKind projection_kind;
    char projection_column[MAX_NAME_SIZE];
    if (!parse_projection(&parser,
                          &projection_kind,
                          projection_column,
                          sizeof(projection_column)) ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    TableSchema* schema = grouped_find_schema(table, table_name);
    if (!grouped_wide_schema(schema) ||
        !tinydb_generic_consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    WideGroupedSelectContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    context.projection_kind = projection_kind;
    if (projection_kind == WIDE_GROUPED_PROJECTION_COLUMN) {
        int projection_index = tinydb_generic_find_column_index(schema,
                                                                projection_column);
        if (projection_index < 0) {
            return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
        }
        context.projection_column_index = (uint32_t)projection_index;
    }

    if (!tinydb_generic_parse_boolean_expression(&parser,
                                                 schema,
                                                 &context.expression)) {
        if (!context.expression.saw_grouping) {
            return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
        }
        *applicable = true;
        return grouped_error(
            result,
            STATEMENT_SELECT,
            TINYDB_GENERIC_SQL_SYNTAX_ERROR,
            EXECUTE_SUCCESS,
            "wide SELECT contains an invalid parenthesized predicate");
    }
    if (!context.expression.saw_grouping) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    *applicable = true;

    if (!parse_limit_offset(&parser,
                            &context.has_limit,
                            &context.limit,
                            &context.offset) ||
        !tinydb_generic_consume_end(&parser)) {
        return grouped_error(
            result,
            STATEMENT_SELECT,
            TINYDB_GENERIC_SQL_SYNTAX_ERROR,
            EXECUTE_SUCCESS,
            "wide SELECT contains an unsupported clause after its grouped predicate");
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    if (!tinydb_record_payload_schema_supported(schema,
                                                message,
                                                sizeof(message))) {
        return grouped_error(result,
                             STATEMENT_SELECT,
                             TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                             EXECUTE_SUCCESS,
                             message);
    }

    bool scan_complete = false;
    (void)tinydb_record_payload_scan(table,
                                    schema,
                                    visit_grouped_select_payload,
                                    &context,
                                    &scan_complete,
                                    message,
                                    sizeof(message));
    if (!scan_complete || context.decode_failed) {
        return grouped_error(
            result,
            STATEMENT_SELECT,
            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
            EXECUTE_SUCCESS,
            message[0] != '\0'
                ? message
                : "unable to decode schema-sized payload during grouped SELECT");
    }

    if (projection_kind == WIDE_GROUPED_PROJECTION_COUNT) {
        uint32_t count = context.matched;
        if (context.offset > 0u) count = 0u;
        if (context.has_limit && context.limit == 0u) count = 0u;
        printf("%u\n", count);
    }
    return grouped_success(result, STATEMENT_SELECT);
}

static bool parse_assignments(TinyDBGenericParser* parser,
                              const TableSchema* schema,
                              WideGroupedAssignment* assignments,
                              uint32_t* assignment_count) {
    *assignment_count = 0u;
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
        WideGroupedAssignment* assignment = &assignments[*assignment_count];
        assignment->column_index = (uint32_t)column_index;
        if (!tinydb_generic_parse_value_for_column(
                parser,
                &schema->columns[column_index],
                &assignment->value)) {
            return false;
        }
        (*assignment_count)++;
        if (tinydb_generic_consume_word(parser, "where")) return true;
        if (!tinydb_generic_consume_char(parser, ',')) return false;
    }
    return false;
}

static bool append_id(WideGroupedCollector* collector, uint32_t id) {
    if (collector->count == collector->capacity) {
        uint32_t new_capacity = collector->capacity == 0u
            ? 16u
            : collector->capacity * 2u;
        if (new_capacity <= collector->capacity) {
            collector->allocation_failed = true;
            return false;
        }
#if SIZE_MAX < UINT32_MAX
        if ((size_t)new_capacity > SIZE_MAX / sizeof(uint32_t)) {
            collector->allocation_failed = true;
            return false;
        }
#endif
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

static bool visit_collect_payload(const TableSchema* schema,
                                  const TinyDBRecordPayload* payload,
                                  void* raw_context) {
    WideGroupedCollector* collector = (WideGroupedCollector*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0u;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_decode_values(schema,
                                             payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &value_count,
                                             message,
                                             sizeof(message)) ||
        value_count != schema->num_columns) {
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

static bool collect_ids(Table* table,
                        const TableSchema* schema,
                        const TinyDBGenericBooleanExpression* expression,
                        WideGroupedCollector* collector,
                        char* message,
                        size_t message_size) {
    memset(collector, 0, sizeof(*collector));
    collector->schema = schema;
    collector->expression = expression;
    bool scan_complete = false;
    (void)tinydb_record_payload_scan(table,
                                    schema,
                                    visit_collect_payload,
                                    collector,
                                    &scan_complete,
                                    message,
                                    message_size);
    return scan_complete && !collector->allocation_failed &&
           !collector->decode_failed;
}

static bool begin_statement_transaction(Table* table) {
    if (table->in_transaction) return false;
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    return true;
}

static void finish_statement_transaction(Table* table,
                                         bool started,
                                         bool success) {
    if (!started) return;
    if (success) {
        pager_commit(table->pager);
    } else {
        pager_rollback(table->pager);
    }
    table->in_transaction = false;
}

static TinyDBGenericSqlStatus apply_update(
    Table* table,
    TableSchema* schema,
    const WideGroupedAssignment* assignments,
    uint32_t assignment_count,
    const TinyDBGenericBooleanExpression* expression,
    TinyDBGenericSqlResult* result) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    WideGroupedCollector collector;
    if (!collect_ids(table,
                     schema,
                     expression,
                     &collector,
                     message,
                     sizeof(message))) {
        free(collector.ids);
        return grouped_error(
            result,
            STATEMENT_UPDATE,
            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
            EXECUTE_SUCCESS,
            message[0] != '\0'
                ? message
                : "unable to collect wide grouped UPDATE targets");
    }
    if (collector.count == 0u) {
        free(collector.ids);
        return grouped_success(result, STATEMENT_UPDATE);
    }

    bool started = begin_statement_transaction(table);
    bool success = true;
    message[0] = '\0';
    for (uint32_t i = 0u; i < collector.count; i++) {
        uint32_t id = collector.ids[i];
        TinyDBRecordPayload existing;
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t value_count = 0u;
        if (!tinydb_record_payload_find(table,
                                        schema,
                                        id,
                                        &existing,
                                        message,
                                        sizeof(message)) ||
            !tinydb_record_payload_decode_values(schema,
                                                 &existing,
                                                 values,
                                                 MAX_COLUMNS_PER_TABLE,
                                                 &value_count,
                                                 message,
                                                 sizeof(message)) ||
            value_count != schema->num_columns) {
            success = false;
            break;
        }
        for (uint32_t j = 0u; j < assignment_count; j++) {
            values[assignments[j].column_index] = assignments[j].value;
        }
        TinyDBRecordPayload replacement;
        if (!tinydb_record_payload_encode_values(schema,
                                                 values,
                                                 value_count,
                                                 &replacement,
                                                 message,
                                                 sizeof(message)) ||
            !tinydb_record_payload_update(table,
                                          schema,
                                          id,
                                          &replacement,
                                          message,
                                          sizeof(message))) {
            success = false;
            break;
        }
    }
    finish_statement_transaction(table, started, success);
    free(collector.ids);

    if (!success) {
        return grouped_error(
            result,
            STATEMENT_UPDATE,
            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
            EXECUTE_SUCCESS,
            message[0] != '\0'
                ? message
                : "wide grouped UPDATE failed");
    }
    return grouped_success(result, STATEMENT_UPDATE);
}

static TinyDBGenericSqlStatus try_grouped_update(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result,
    bool* applicable) {
    *applicable = false;
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
    TableSchema* schema = grouped_find_schema(table, table_name);
    if (!grouped_wide_schema(schema) ||
        !tinydb_generic_consume_word(&parser, "set")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    WideGroupedAssignment assignments[MAX_COLUMNS_PER_TABLE];
    uint32_t assignment_count = 0u;
    if (!parse_assignments(&parser,
                           schema,
                           assignments,
                           &assignment_count) ||
        assignment_count == 0u) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    TinyDBGenericBooleanExpression expression;
    memset(&expression, 0, sizeof(expression));
    if (!tinydb_generic_parse_boolean_expression(&parser, schema, &expression)) {
        if (!expression.saw_grouping) {
            return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
        }
        *applicable = true;
        return grouped_error(
            result,
            STATEMENT_UPDATE,
            TINYDB_GENERIC_SQL_SYNTAX_ERROR,
            EXECUTE_SUCCESS,
            "wide UPDATE contains an invalid parenthesized predicate");
    }
    if (!expression.saw_grouping) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    *applicable = true;
    if (!tinydb_generic_consume_end(&parser)) {
        return grouped_error(
            result,
            STATEMENT_UPDATE,
            TINYDB_GENERIC_SQL_SYNTAX_ERROR,
            EXECUTE_SUCCESS,
            "wide UPDATE contains an unsupported clause after its grouped predicate");
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(schema,
                                                message,
                                                sizeof(message))) {
        return grouped_error(result,
                             STATEMENT_UPDATE,
                             TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                             EXECUTE_SUCCESS,
                             message);
    }
    return apply_update(table,
                        schema,
                        assignments,
                        assignment_count,
                        &expression,
                        result);
}

static TinyDBGenericSqlStatus apply_delete(
    Table* table,
    TableSchema* schema,
    const TinyDBGenericBooleanExpression* expression,
    TinyDBGenericSqlResult* result) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    WideGroupedCollector collector;
    if (!collect_ids(table,
                     schema,
                     expression,
                     &collector,
                     message,
                     sizeof(message))) {
        free(collector.ids);
        return grouped_error(
            result,
            STATEMENT_DELETE,
            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
            EXECUTE_SUCCESS,
            message[0] != '\0'
                ? message
                : "unable to collect wide grouped DELETE targets");
    }
    if (collector.count == 0u) {
        free(collector.ids);
        return grouped_success(result, STATEMENT_DELETE);
    }

    bool started = begin_statement_transaction(table);
    bool success = true;
    message[0] = '\0';
    for (uint32_t i = 0u; i < collector.count; i++) {
        if (!tinydb_record_payload_delete(table,
                                          schema,
                                          collector.ids[i],
                                          message,
                                          sizeof(message))) {
            success = false;
            break;
        }
    }
    finish_statement_transaction(table, started, success);
    free(collector.ids);

    if (!success) {
        return grouped_error(
            result,
            STATEMENT_DELETE,
            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
            EXECUTE_SUCCESS,
            message[0] != '\0'
                ? message
                : "wide grouped DELETE failed");
    }
    return grouped_success(result, STATEMENT_DELETE);
}

static TinyDBGenericSqlStatus try_grouped_delete(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result,
    bool* applicable) {
    *applicable = false;
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
    TableSchema* schema = grouped_find_schema(table, table_name);
    if (!grouped_wide_schema(schema) ||
        !tinydb_generic_consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    TinyDBGenericBooleanExpression expression;
    memset(&expression, 0, sizeof(expression));
    if (!tinydb_generic_parse_boolean_expression(&parser, schema, &expression)) {
        if (!expression.saw_grouping) {
            return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
        }
        *applicable = true;
        return grouped_error(
            result,
            STATEMENT_DELETE,
            TINYDB_GENERIC_SQL_SYNTAX_ERROR,
            EXECUTE_SUCCESS,
            "wide DELETE contains an invalid parenthesized predicate");
    }
    if (!expression.saw_grouping) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    *applicable = true;
    if (!tinydb_generic_consume_end(&parser)) {
        return grouped_error(
            result,
            STATEMENT_DELETE,
            TINYDB_GENERIC_SQL_SYNTAX_ERROR,
            EXECUTE_SUCCESS,
            "wide DELETE contains an unsupported clause after its grouped predicate");
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(schema,
                                                message,
                                                sizeof(message))) {
        return grouped_error(result,
                             STATEMENT_DELETE,
                             TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                             EXECUTE_SUCCESS,
                             message);
    }
    return apply_delete(table, schema, &expression, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    if (table != NULL && sql != NULL) {
        bool select_applicable = false;
        TinyDBGenericSqlStatus select_status =
            try_grouped_select(table, sql, output, &select_applicable);
        if (select_applicable) return select_status;

        initialize_result(output);
        bool update_applicable = false;
        TinyDBGenericSqlStatus update_status =
            try_grouped_update(table, sql, output, &update_applicable);
        if (update_applicable) return update_status;

        initialize_result(output);
        bool delete_applicable = false;
        TinyDBGenericSqlStatus delete_status =
            try_grouped_delete(table, sql, output, &delete_applicable);
        if (delete_applicable) return delete_status;
    }

    return tinydb_generic_sql_try_execute_wide_or_base(table, sql, output);
}
