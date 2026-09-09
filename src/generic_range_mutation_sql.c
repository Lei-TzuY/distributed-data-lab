#include "generic_sql.h"

#include <ctype.h>
#include <errno.h>

typedef struct {
    const char* current;
} RangeParser;

typedef enum {
    RANGE_COMPARE_EQ = 0,
    RANGE_COMPARE_GT,
    RANGE_COMPARE_GTE,
    RANGE_COMPARE_LT,
    RANGE_COMPARE_LTE
} RangeCompareOp;

typedef struct {
    uint32_t column_index;
    TinyDBValue value;
} RangeAssignment;

typedef struct {
    uint32_t column_index;
    RangeCompareOp op;
    TinyDBValue value;
} RangePredicate;

typedef struct {
    RangePredicate predicate;
    uint32_t* ids;
    uint32_t count;
    uint32_t capacity;
    bool allocation_failed;
    bool decode_failed;
} RangeCollector;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

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

static void skip_spaces(RangeParser* parser) {
    while (isspace((unsigned char)*parser->current)) parser->current++;
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool consume_word(RangeParser* parser, const char* word) {
    RangeParser backup = *parser;
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

static bool consume_char(RangeParser* parser, char expected) {
    skip_spaces(parser);
    if (*parser->current != expected) return false;
    parser->current++;
    return true;
}

static bool parse_identifier(RangeParser* parser,
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

static bool parse_uint32(RangeParser* parser, uint32_t* value) {
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

static bool parse_string(RangeParser* parser,
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

static bool consume_end(RangeParser* parser) {
    skip_spaces(parser);
    if (*parser->current == ';') parser->current++;
    skip_spaces(parser);
    return *parser->current == '\0';
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

static bool parse_value_for_column(RangeParser* parser,
                                   const TableColumn* column,
                                   TinyDBValue* value) {
    memset(value, 0, sizeof(*value));
    value->type = column->type;
    if (column->type == COL_TYPE_INT) {
        return parse_uint32(parser, &value->int_value);
    }
    return parse_string(parser, value->text, sizeof(value->text));
}

static bool parse_compare_op(RangeParser* parser, RangeCompareOp* op) {
    skip_spaces(parser);
    if (*parser->current == '=') {
        parser->current++;
        *op = RANGE_COMPARE_EQ;
        return true;
    }
    if (*parser->current == '>') {
        parser->current++;
        if (*parser->current == '=') {
            parser->current++;
            *op = RANGE_COMPARE_GTE;
        } else {
            *op = RANGE_COMPARE_GT;
        }
        return true;
    }
    if (*parser->current == '<') {
        parser->current++;
        if (*parser->current == '=') {
            parser->current++;
            *op = RANGE_COMPARE_LTE;
        } else {
            *op = RANGE_COMPARE_LT;
        }
        return true;
    }
    return false;
}

static bool parse_predicate(RangeParser* parser,
                            const TableSchema* schema,
                            RangePredicate* predicate) {
    char column[MAX_NAME_SIZE];
    if (!parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = find_column_index(schema, column);
    if (column_index < 0 || !parse_compare_op(parser, &predicate->op)) {
        return false;
    }
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

static bool predicate_matches(const RangePredicate* predicate,
                              const TinyDBValue* value) {
    if (value->type != predicate->value.type) return false;
    int compared = compare_values(value, &predicate->value);
    switch (predicate->op) {
        case RANGE_COMPARE_EQ:
            return compared == 0;
        case RANGE_COMPARE_GT:
            return compared > 0;
        case RANGE_COMPARE_GTE:
            return compared >= 0;
        case RANGE_COMPARE_LT:
            return compared < 0;
        case RANGE_COMPARE_LTE:
            return compared <= 0;
    }
    return false;
}

static bool append_id(RangeCollector* collector, uint32_t id) {
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

static bool collect_range_match(const TableSchema* schema,
                                const TinyDBRecord* record,
                                void* raw_context) {
    RangeCollector* collector = (RangeCollector*)raw_context;
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
        collector->decode_failed = true;
        return false;
    }
    if (!predicate_matches(&collector->predicate,
                           &values[collector->predicate.column_index])) {
        return true;
    }
    return append_id(collector, values[0].int_value);
}

static bool collect_matching_ids(Table* table,
                                 const TableSchema* schema,
                                 const RangePredicate* predicate,
                                 RangeCollector* collector) {
    memset(collector, 0, sizeof(*collector));
    collector->predicate = *predicate;
    (void)tinydb_record_scan(table, schema, collect_range_match, collector);
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

static TinyDBGenericSqlStatus execute_range_update(
    Table* table,
    TableSchema* schema,
    RangeParser* parser,
    TinyDBGenericSqlResult* result) {
    RangeAssignment assignments[MAX_COLUMNS_PER_TABLE];
    uint32_t assignment_count = 0;

    while (assignment_count < MAX_COLUMNS_PER_TABLE) {
        char column_name[MAX_NAME_SIZE];
        if (!parse_identifier(parser, column_name, sizeof(column_name))) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE requires a column assignment");
        }
        int column_index = find_column_index(schema, column_name);
        if (column_index < 0) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE references an unknown column");
        }
        if (column_index == 0) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE cannot change the primary-key id");
        }
        if (!consume_char(parser, '=')) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE assignment requires '='");
        }

        RangeAssignment* assignment = &assignments[assignment_count++];
        assignment->column_index = (uint32_t)column_index;
        if (!parse_value_for_column(parser,
                                    &schema->columns[column_index],
                                    &assignment->value)) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE assignment value has the wrong type");
        }

        if (consume_word(parser, "where")) break;
        if (!consume_char(parser, ',')) {
            return syntax_error(result,
                                STATEMENT_UPDATE,
                                "generic UPDATE requires WHERE after assignments");
        }
    }

    RangePredicate predicate;
    memset(&predicate, 0, sizeof(predicate));
    if (assignment_count == 0 ||
        !parse_predicate(parser, schema, &predicate) ||
        !consume_end(parser)) {
        return syntax_error(result,
                            STATEMENT_UPDATE,
                            "generic UPDATE requires a typed comparison predicate in WHERE");
    }
    if (predicate.op == RANGE_COMPARE_EQ) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    RangeCollector collector;
    if (!collect_matching_ids(table, schema, &predicate, &collector)) {
        free(collector.ids);
        return execute_error(result,
                             STATEMENT_UPDATE,
                             "unable to collect generic UPDATE range targets");
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
        TinyDBRecord existing;
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t value_count = 0;
        uint32_t id = collector.ids[i];

        if (!tinydb_record_find(table, schema, id, &existing) ||
            !tinydb_record_decode(schema,
                                  &existing,
                                  values,
                                  MAX_COLUMNS_PER_TABLE,
                                  &value_count,
                                  message,
                                  sizeof(message))) {
            mutation_succeeded = false;
            break;
        }

        for (uint32_t j = 0; j < assignment_count; j++) {
            values[assignments[j].column_index] = assignments[j].value;
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
        return execute_error(result,
                             STATEMENT_UPDATE,
                             message[0] != '\0' ? message : "generic UPDATE failed");
    }
    return success(result, STATEMENT_UPDATE);
}

static TinyDBGenericSqlStatus execute_range_delete(
    Table* table,
    TableSchema* schema,
    RangeParser* parser,
    TinyDBGenericSqlResult* result) {
    if (!consume_word(parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    RangePredicate predicate;
    memset(&predicate, 0, sizeof(predicate));
    if (!parse_predicate(parser, schema, &predicate) || !consume_end(parser)) {
        return syntax_error(result,
                            STATEMENT_DELETE,
                            "generic DELETE requires a typed comparison predicate in WHERE");
    }
    if (predicate.op == RANGE_COMPARE_EQ) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    RangeCollector collector;
    if (!collect_matching_ids(table, schema, &predicate, &collector)) {
        free(collector.ids);
        return execute_error(result,
                             STATEMENT_DELETE,
                             "unable to collect generic DELETE range targets");
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
        return execute_error(result,
                             STATEMENT_DELETE,
                             message[0] != '\0' ? message : "generic DELETE failed");
    }
    return success(result, STATEMENT_DELETE);
}

static TinyDBGenericSqlStatus try_range_mutation(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    RangeParser parser;
    parser.current = sql;
    StatementType type;
    char table_name[MAX_NAME_SIZE];

    if (consume_word(&parser, "update")) {
        type = STATEMENT_UPDATE;
        if (!parse_identifier(&parser, table_name, sizeof(table_name))) {
            return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
        }
    } else if (consume_word(&parser, "delete")) {
        type = STATEMENT_DELETE;
        if (!consume_word(&parser, "from") ||
            !parse_identifier(&parser, table_name, sizeof(table_name))) {
            return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
        }
    } else {
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
        return execute_error(result, type, schema_message);
    }

    if (type == STATEMENT_UPDATE) {
        if (!consume_word(&parser, "set")) {
            return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
        }
        return execute_range_update(table, schema, &parser, result);
    }
    return execute_range_delete(table, schema, &parser, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    if (table == NULL || sql == NULL) return output->status;

    TinyDBGenericSqlStatus range_status = try_range_mutation(table, sql, output);
    if (range_status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) {
        return range_status;
    }
    return tinydb_generic_sql_try_execute_base(table, sql, output);
}
