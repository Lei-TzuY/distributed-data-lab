#include "generic_predicate.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdlib.h>

#define GENERIC_OR_MUTATION_MAX_GROUPS MAX_COLUMNS_PER_TABLE
#define GENERIC_OR_MUTATION_MAX_TERMS MAX_COLUMNS_PER_TABLE

typedef struct {
    TinyDBGenericPredicate terms[GENERIC_OR_MUTATION_MAX_TERMS];
    uint32_t count;
} GenericOrMutationGroup;

typedef struct {
    GenericOrMutationGroup groups[GENERIC_OR_MUTATION_MAX_GROUPS];
    uint32_t count;
} GenericOrMutationExpression;

typedef struct {
    uint32_t column_index;
    TinyDBValue value;
} GenericOrMutationAssignment;

typedef struct {
    const GenericOrMutationExpression* expression;
    uint32_t* ids;
    uint32_t count;
    uint32_t capacity;
    bool allocation_failed;
    bool decode_failed;
} GenericOrMutationCollector;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_or_select_base(
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

static bool payload_native_schema(const TableSchema* schema) {
    return schema != NULL && schema->row_size > ROW_SIZE;
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

static bool parse_or_expression(TinyDBGenericParser* parser,
                                const TableSchema* schema,
                                GenericOrMutationExpression* expression,
                                bool* saw_or) {
    memset(expression, 0, sizeof(*expression));
    *saw_or = false;
    expression->count = 1;

    for (;;) {
        GenericOrMutationGroup* group =
            &expression->groups[expression->count - 1u];
        if (group->count >= GENERIC_OR_MUTATION_MAX_TERMS ||
            !tinydb_generic_parse_predicate(parser,
                                            schema,
                                            &group->terms[group->count])) {
            return false;
        }
        group->count++;

        if (tinydb_generic_consume_word(parser, "and")) continue;
        if (tinydb_generic_consume_word(parser, "or")) {
            *saw_or = true;
            if (expression->count >= GENERIC_OR_MUTATION_MAX_GROUPS) {
                return false;
            }
            expression->count++;
            continue;
        }
        return true;
    }
}

static bool expression_matches(const GenericOrMutationExpression* expression,
                               const TinyDBValue* values) {
    for (uint32_t group_index = 0;
         group_index < expression->count;
         group_index++) {
        const GenericOrMutationGroup* group = &expression->groups[group_index];
        bool group_matches = true;
        for (uint32_t term_index = 0;
             term_index < group->count;
             term_index++) {
            const TinyDBGenericPredicate* predicate = &group->terms[term_index];
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

static bool append_id(GenericOrMutationCollector* collector, uint32_t id) {
    if (collector->count == collector->capacity) {
        uint32_t new_capacity = collector->capacity == 0
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

static bool visit_collect_record(const TableSchema* schema,
                                 const TinyDBRecord* record,
                                 void* raw_context) {
    GenericOrMutationCollector* collector =
        (GenericOrMutationCollector*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    if (!decode_values(schema, record, values)) {
        collector->decode_failed = true;
        return false;
    }
    if (!expression_matches(collector->expression, values)) return true;
    return append_id(collector, values[0].int_value);
}

static bool visit_collect_payload(const TableSchema* schema,
                                  const TinyDBRecordPayload* payload,
                                  void* raw_context) {
    GenericOrMutationCollector* collector =
        (GenericOrMutationCollector*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    if (!decode_payload_values(schema, payload, values)) {
        collector->decode_failed = true;
        return false;
    }
    if (!expression_matches(collector->expression, values)) return true;
    return append_id(collector, values[0].int_value);
}

static bool collect_matching_ids(
    Table* table,
    const TableSchema* schema,
    const GenericOrMutationExpression* expression,
    GenericOrMutationCollector* collector) {
    memset(collector, 0, sizeof(*collector));
    collector->expression = expression;

    if (payload_native_schema(schema)) {
        bool scan_complete = false;
        char message[TINYDB_RECORD_MESSAGE_MAX];
        message[0] = '\0';
        (void)tinydb_record_payload_scan(table,
                                        schema,
                                        visit_collect_payload,
                                        collector,
                                        &scan_complete,
                                        message,
                                        sizeof(message));
        return scan_complete && !collector->allocation_failed &&
               !collector->decode_failed;
    }

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

static bool parse_assignments(TinyDBGenericParser* parser,
                              const TableSchema* schema,
                              GenericOrMutationAssignment* assignments,
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

        GenericOrMutationAssignment* assignment =
            &assignments[(*assignment_count)++];
        assignment->column_index = (uint32_t)column_index;
        if (!tinydb_generic_parse_value_for_column(
                parser, &schema->columns[column_index], &assignment->value)) {
            return false;
        }

        if (tinydb_generic_consume_word(parser, "where")) return true;
        if (!tinydb_generic_consume_char(parser, ',')) return false;
    }
    return false;
}

static bool load_row_values(Table* table,
                            const TableSchema* schema,
                            uint32_t id,
                            TinyDBValue* values,
                            uint32_t* value_count,
                            char* message,
                            size_t message_size) {
    *value_count = 0u;
    if (payload_native_schema(schema)) {
        TinyDBRecordPayload payload;
        return tinydb_record_payload_find(table,
                                          schema,
                                          id,
                                          &payload,
                                          message,
                                          message_size) &&
               tinydb_record_payload_decode_values(schema,
                                                   &payload,
                                                   values,
                                                   MAX_COLUMNS_PER_TABLE,
                                                   value_count,
                                                   message,
                                                   message_size) &&
               *value_count == schema->num_columns;
    }

    TinyDBRecord record;
    return tinydb_record_find(table, schema, id, &record) &&
           tinydb_record_decode(schema,
                                &record,
                                values,
                                MAX_COLUMNS_PER_TABLE,
                                value_count,
                                message,
                                message_size) &&
           *value_count == schema->num_columns;
}

static bool store_row_values(Table* table,
                             const TableSchema* schema,
                             uint32_t id,
                             const TinyDBValue* values,
                             uint32_t value_count,
                             char* message,
                             size_t message_size) {
    if (payload_native_schema(schema)) {
        TinyDBRecordPayload replacement;
        return tinydb_record_payload_encode_values(schema,
                                                   values,
                                                   value_count,
                                                   &replacement,
                                                   message,
                                                   message_size) &&
               tinydb_record_payload_update(table,
                                            schema,
                                            id,
                                            &replacement,
                                            message,
                                            message_size);
    }
    return tinydb_record_update(table,
                                schema,
                                id,
                                values,
                                value_count,
                                message,
                                message_size);
}

static TinyDBGenericSqlStatus apply_or_update(
    Table* table,
    TableSchema* schema,
    const GenericOrMutationAssignment* assignments,
    uint32_t assignment_count,
    const GenericOrMutationExpression* expression,
    TinyDBGenericSqlResult* result) {
    GenericOrMutationCollector collector;
    if (!collect_matching_ids(table, schema, expression, &collector)) {
        free(collector.ids);
        return execute_error(result,
                             STATEMENT_UPDATE,
                             "unable to collect generic OR UPDATE targets");
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
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t value_count = 0u;
        if (!load_row_values(table,
                             schema,
                             id,
                             values,
                             &value_count,
                             message,
                             sizeof(message))) {
            mutation_succeeded = false;
            break;
        }

        for (uint32_t assignment_index = 0;
             assignment_index < assignment_count;
             assignment_index++) {
            values[assignments[assignment_index].column_index] =
                assignments[assignment_index].value;
        }
        if (!store_row_values(table,
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
                             message[0] != '\0'
                                 ? message
                                 : "generic OR UPDATE failed");
    }
    return success(result, STATEMENT_UPDATE);
}

static TinyDBGenericSqlStatus try_or_update(Table* table,
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
    if (payload_native_schema(schema)) {
        if (!tinydb_record_payload_schema_supported(schema,
                                                    schema_message,
                                                    sizeof(schema_message))) {
            return execute_error(result, STATEMENT_UPDATE, schema_message);
        }
    } else if (!tinydb_schema_supports_records(schema,
                                               schema_message,
                                               sizeof(schema_message))) {
        return execute_error(result, STATEMENT_UPDATE, schema_message);
    }
    if (!tinydb_generic_consume_word(&parser, "set")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    GenericOrMutationAssignment assignments[MAX_COLUMNS_PER_TABLE];
    uint32_t assignment_count = 0;
    if (!parse_assignments(&parser,
                           schema,
                           assignments,
                           &assignment_count) ||
        assignment_count == 0) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    GenericOrMutationExpression expression;
    bool saw_or = false;
    if (!parse_or_expression(&parser, schema, &expression, &saw_or)) {
        if (saw_or) {
            return syntax_error(
                result,
                STATEMENT_UPDATE,
                "generic UPDATE WHERE requires typed predicates joined by AND/OR");
        }
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    if (!saw_or) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    if (!tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_UPDATE,
            "generic UPDATE contains an unsupported clause after its OR predicate");
    }

    return apply_or_update(table,
                           schema,
                           assignments,
                           assignment_count,
                           &expression,
                           result);
}

static TinyDBGenericSqlStatus apply_or_delete(
    Table* table,
    TableSchema* schema,
    const GenericOrMutationExpression* expression,
    TinyDBGenericSqlResult* result) {
    GenericOrMutationCollector collector;
    if (!collect_matching_ids(table, schema, expression, &collector)) {
        free(collector.ids);
        return execute_error(result,
                             STATEMENT_DELETE,
                             "unable to collect generic OR DELETE targets");
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
        bool deleted = payload_native_schema(schema)
            ? tinydb_record_payload_delete(table,
                                           schema,
                                           collector.ids[i],
                                           message,
                                           sizeof(message))
            : tinydb_record_delete(table,
                                   schema,
                                   collector.ids[i],
                                   message,
                                   sizeof(message));
        if (!deleted) {
            mutation_succeeded = false;
            break;
        }
    }

    finish_internal_transaction(table, started_transaction, mutation_succeeded);
    free(collector.ids);
    if (!mutation_succeeded) {
        return execute_error(result,
                             STATEMENT_DELETE,
                             message[0] != '\0'
                                 ? message
                                 : "generic OR DELETE failed");
    }
    return success(result, STATEMENT_DELETE);
}

static TinyDBGenericSqlStatus try_or_delete(Table* table,
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
    if (payload_native_schema(schema)) {
        if (!tinydb_record_payload_schema_supported(schema,
                                                    schema_message,
                                                    sizeof(schema_message))) {
            return execute_error(result, STATEMENT_DELETE, schema_message);
        }
    } else if (!tinydb_schema_supports_records(schema,
                                               schema_message,
                                               sizeof(schema_message))) {
        return execute_error(result, STATEMENT_DELETE, schema_message);
    }
    if (!tinydb_generic_consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    GenericOrMutationExpression expression;
    bool saw_or = false;
    if (!parse_or_expression(&parser, schema, &expression, &saw_or)) {
        if (saw_or) {
            return syntax_error(
                result,
                STATEMENT_DELETE,
                "generic DELETE WHERE requires typed predicates joined by AND/OR");
        }
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }
    if (!saw_or) return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    if (!tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_DELETE,
            "generic DELETE contains an unsupported clause after its OR predicate");
    }

    return apply_or_delete(table, schema, &expression, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    TinyDBGenericSqlStatus status = try_or_update(table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    status = try_or_delete(table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    return tinydb_generic_sql_try_execute_or_select_base(table, sql, output);
}
