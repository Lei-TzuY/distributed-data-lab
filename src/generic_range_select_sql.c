#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <errno.h>

typedef struct {
    const char* current;
} SelectRangeParser;

typedef enum {
    SELECT_RANGE_EQ = 0,
    SELECT_RANGE_GT,
    SELECT_RANGE_GTE,
    SELECT_RANGE_LT,
    SELECT_RANGE_LTE
} SelectRangeCompareOp;

typedef struct {
    uint32_t column_index;
    SelectRangeCompareOp op;
    TinyDBValue value;
} SelectRangePredicate;

typedef struct {
    const TableSchema* schema;
    SelectRangePredicate predicate;
    bool count_only;
    bool project_column;
    uint32_t projection_column_index;
    uint32_t matched;
    uint32_t emitted;
    uint32_t offset;
    uint32_t limit;
    bool has_limit;
    bool decode_failed;
} SelectRangeContext;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_range_mutations(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

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

static void skip_spaces(SelectRangeParser* parser) {
    while (isspace((unsigned char)*parser->current)) parser->current++;
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool consume_word(SelectRangeParser* parser, const char* word) {
    SelectRangeParser backup = *parser;
    const char* expected = word;
    skip_spaces(parser);
    while (*expected != '\0' && ci_char(*parser->current) == ci_char(*expected)) {
        parser->current++;
        expected++;
    }
    if (*expected != '\0' || is_identifier_char(*parser->current)) {
        *parser = backup;
        return false;
    }
    return true;
}

static bool consume_char(SelectRangeParser* parser, char expected) {
    skip_spaces(parser);
    if (*parser->current != expected) return false;
    parser->current++;
    return true;
}

static bool parse_identifier(SelectRangeParser* parser,
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

static bool parse_uint32(SelectRangeParser* parser, uint32_t* value) {
    skip_spaces(parser);
    if (!isdigit((unsigned char)*parser->current)) return false;
    char* end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(parser->current, &end, 10);
    if (errno == ERANGE || parsed > UINT32_MAX || end == parser->current) return false;
    parser->current = end;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_string(SelectRangeParser* parser,
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

static bool consume_end(SelectRangeParser* parser) {
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

static bool parse_projection(SelectRangeParser* parser,
                             const TableSchema* schema,
                             bool* count_only,
                             bool* project_column,
                             uint32_t* projection_column_index) {
    if (consume_char(parser, '*')) {
        *count_only = false;
        *project_column = false;
        *projection_column_index = 0;
        return true;
    }

    SelectRangeParser backup = *parser;
    if (consume_word(parser, "count") && consume_char(parser, '(') &&
        consume_char(parser, '*') && consume_char(parser, ')')) {
        *count_only = true;
        *project_column = false;
        *projection_column_index = 0;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = find_column_index(schema, column);
    if (column_index < 0) return false;
    *count_only = false;
    *project_column = true;
    *projection_column_index = (uint32_t)column_index;
    return true;
}

static bool parse_compare_op(SelectRangeParser* parser,
                             SelectRangeCompareOp* op) {
    skip_spaces(parser);
    if (*parser->current == '=') {
        parser->current++;
        *op = SELECT_RANGE_EQ;
        return true;
    }
    if (*parser->current == '>') {
        parser->current++;
        if (*parser->current == '=') {
            parser->current++;
            *op = SELECT_RANGE_GTE;
        } else {
            *op = SELECT_RANGE_GT;
        }
        return true;
    }
    if (*parser->current == '<') {
        parser->current++;
        if (*parser->current == '=') {
            parser->current++;
            *op = SELECT_RANGE_LTE;
        } else {
            *op = SELECT_RANGE_LT;
        }
        return true;
    }
    return false;
}

static bool parse_value_for_column(SelectRangeParser* parser,
                                   const TableColumn* column,
                                   TinyDBValue* value) {
    memset(value, 0, sizeof(*value));
    value->type = column->type;
    if (column->type == COL_TYPE_INT) {
        return parse_uint32(parser, &value->int_value);
    }
    return parse_string(parser, value->text, sizeof(value->text));
}

static bool parse_predicate(SelectRangeParser* parser,
                            const TableSchema* schema,
                            SelectRangePredicate* predicate) {
    char column[MAX_NAME_SIZE];
    if (!parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = find_column_index(schema, column);
    if (column_index < 0 || !parse_compare_op(parser, &predicate->op)) return false;
    predicate->column_index = (uint32_t)column_index;
    return parse_value_for_column(parser,
                                  &schema->columns[predicate->column_index],
                                  &predicate->value);
}

static int compare_values(const TinyDBValue* left, const TinyDBValue* right) {
    if (left->type == COL_TYPE_INT) {
        if (left->int_value < right->int_value) return -1;
        if (left->int_value > right->int_value) return 1;
        return 0;
    }
    int compared = strcmp(left->text, right->text);
    if (compared < 0) return -1;
    if (compared > 0) return 1;
    return 0;
}

static bool predicate_matches(const SelectRangePredicate* predicate,
                              const TinyDBValue* value) {
    if (value->type != predicate->value.type) return false;
    int compared = compare_values(value, &predicate->value);
    switch (predicate->op) {
        case SELECT_RANGE_EQ: return compared == 0;
        case SELECT_RANGE_GT: return compared > 0;
        case SELECT_RANGE_GTE: return compared >= 0;
        case SELECT_RANGE_LT: return compared < 0;
        case SELECT_RANGE_LTE: return compared <= 0;
    }
    return false;
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

static bool visit_range_values(SelectRangeContext* context,
                               const TinyDBValue* values,
                               uint32_t value_count) {
    if (!predicate_matches(&context->predicate,
                           &values[context->predicate.column_index])) {
        return true;
    }

    context->matched++;
    if (context->count_only) return true;
    if (context->matched <= context->offset) return true;
    if (context->has_limit && context->emitted >= context->limit) return true;

    if (context->project_column) {
        print_value(&values[context->projection_column_index]);
    } else {
        print_payload_row(values, value_count);
    }
    context->emitted++;
    return true;
}

static bool visit_range_record(const TableSchema* schema,
                               const TinyDBRecord* record,
                               void* raw_context) {
    SelectRangeContext* context = (SelectRangeContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_decode(schema,
                              record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &value_count,
                              message,
                              sizeof(message)) ||
        value_count != schema->num_columns) {
        context->decode_failed = true;
        return false;
    }

    if (!predicate_matches(&context->predicate,
                           &values[context->predicate.column_index])) {
        return true;
    }

    context->matched++;
    if (context->count_only) return true;
    if (context->matched <= context->offset) return true;
    if (context->has_limit && context->emitted >= context->limit) return false;

    if (context->project_column) {
        print_value(&values[context->projection_column_index]);
    } else {
        tinydb_record_print(context->schema, record);
    }
    context->emitted++;
    return true;
}

static bool visit_range_payload(const TableSchema* schema,
                                const TinyDBRecordPayload* payload,
                                void* raw_context) {
    SelectRangeContext* context = (SelectRangeContext*)raw_context;
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
    return visit_range_values(context, values, value_count);
}

static bool payload_primary_range(const SelectRangePredicate* predicate,
                                  uint32_t* min_id,
                                  uint32_t* max_id,
                                  bool* empty) {
    if (predicate == NULL || min_id == NULL || max_id == NULL || empty == NULL ||
        predicate->column_index != 0u || predicate->value.type != COL_TYPE_INT) {
        return false;
    }

    uint32_t value = predicate->value.int_value;
    *empty = false;
    switch (predicate->op) {
        case SELECT_RANGE_GT:
            if (value == UINT32_MAX) {
                *empty = true;
                return true;
            }
            *min_id = value + 1u;
            *max_id = UINT32_MAX;
            return true;
        case SELECT_RANGE_GTE:
            *min_id = value;
            *max_id = UINT32_MAX;
            return true;
        case SELECT_RANGE_LT:
            if (value == 0u) {
                *empty = true;
                return true;
            }
            *min_id = 0u;
            *max_id = value - 1u;
            return true;
        case SELECT_RANGE_LTE:
            *min_id = 0u;
            *max_id = value;
            return true;
        case SELECT_RANGE_EQ:
            *min_id = value;
            *max_id = value;
            return true;
    }
    return false;
}

static TinyDBGenericSqlStatus try_range_select(Table* table,
                                                const char* sql,
                                                TinyDBGenericSqlResult* result) {
    SelectRangeParser parser;
    parser.current = sql;
    if (!consume_word(&parser, "select")) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;

    /* Resolve the target table before validating the projection so routing stays
     * fail-closed for legacy users and unknown tables. */
    SelectRangeParser routing = parser;
    bool routing_count = false;
    bool routing_project = false;
    uint32_t routing_projection_index = 0;
    char routing_projection_name[MAX_NAME_SIZE];
    routing_projection_name[0] = '\0';

    if (consume_char(&routing, '*')) {
        routing_project = false;
    } else {
        SelectRangeParser count_backup = routing;
        if (consume_word(&routing, "count") && consume_char(&routing, '(') &&
            consume_char(&routing, '*') && consume_char(&routing, ')')) {
            routing_count = true;
        } else {
            routing = count_backup;
            if (!parse_identifier(&routing,
                                  routing_projection_name,
                                  sizeof(routing_projection_name))) {
                return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
            }
            routing_project = true;
        }
    }
    (void)routing_count;
    (void)routing_project;
    (void)routing_projection_index;

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
    bool payload_native = schema->row_size > ROW_SIZE;
    if (payload_native) {
        if (!tinydb_record_payload_schema_supported(schema,
                                                    schema_message,
                                                    sizeof(schema_message))) {
            return execute_error(result, schema_message);
        }
    } else if (!tinydb_schema_supports_records(schema,
                                               schema_message,
                                               sizeof(schema_message))) {
        return execute_error(result, schema_message);
    }

    bool count_only = false;
    bool project_column = false;
    uint32_t projection_column_index = 0;
    if (!parse_projection(&parser,
                          schema,
                          &count_only,
                          &project_column,
                          &projection_column_index)) {
        return syntax_error(result,
                            "generic SELECT projection references an unknown or unsupported column");
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

    SelectRangePredicate predicate;
    memset(&predicate, 0, sizeof(predicate));
    if (!parse_predicate(&parser, schema, &predicate)) {
        return syntax_error(result,
                            "generic SELECT WHERE references an unknown column, operator, or typed value");
    }
    if (predicate.op == SELECT_RANGE_EQ) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    bool has_limit = false;
    uint32_t limit = 0;
    uint32_t offset = 0;
    if (consume_word(&parser, "limit")) {
        if (!parse_uint32(&parser, &limit)) {
            return syntax_error(result, "LIMIT requires an integer");
        }
        has_limit = true;
        if (consume_word(&parser, "offset")) {
            if (!parse_uint32(&parser, &offset)) {
                return syntax_error(result, "OFFSET requires an integer");
            }
        }
    } else if (consume_word(&parser, "offset")) {
        if (!parse_uint32(&parser, &offset)) {
            return syntax_error(result, "OFFSET requires an integer");
        }
    }

    if (!consume_end(&parser)) {
        return syntax_error(result,
                            "generic SELECT contains an unsupported clause");
    }

    SelectRangeContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    context.predicate = predicate;
    context.count_only = count_only;
    context.project_column = project_column;
    context.projection_column_index = projection_column_index;
    context.offset = offset;
    context.limit = limit;
    context.has_limit = has_limit;

    if (payload_native) {
        bool scan_complete = true;
        char scan_message[TINYDB_RECORD_MESSAGE_MAX];
        scan_message[0] = '\0';
        uint32_t min_id = 0u;
        uint32_t max_id = UINT32_MAX;
        bool empty_range = false;
        bool has_primary_range = payload_primary_range(&predicate,
                                                       &min_id,
                                                       &max_id,
                                                       &empty_range);
        if (!empty_range) {
            if (has_primary_range) {
                (void)tinydb_record_payload_scan_range(table,
                                                       schema,
                                                       min_id,
                                                       max_id,
                                                       visit_range_payload,
                                                       &context,
                                                       &scan_complete,
                                                       scan_message,
                                                       sizeof(scan_message));
            } else {
                (void)tinydb_record_payload_scan(table,
                                                schema,
                                                visit_range_payload,
                                                &context,
                                                &scan_complete,
                                                scan_message,
                                                sizeof(scan_message));
            }
        }
        if (!scan_complete || context.decode_failed) {
            return execute_error(result,
                                 scan_message[0] != '\0'
                                     ? scan_message
                                     : "unable to decode schema-sized payload during range SELECT");
        }
    } else {
        (void)tinydb_record_scan(table, schema, visit_range_record, &context);
        if (context.decode_failed) {
            return execute_error(result, "unable to decode generic record during range SELECT");
        }
    }

    if (count_only) {
        uint32_t count = context.matched;
        if (offset > 0) count = 0;
        if (has_limit && limit == 0) count = 0;
        printf("%u\n", count);
    }

    return success(result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    TinyDBGenericSqlStatus range_select_status = try_range_select(table, sql, output);
    if (range_select_status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) {
        return range_select_status;
    }

    initialize_result(output);
    return tinydb_generic_sql_try_execute_range_mutations(table, sql, output);
}
