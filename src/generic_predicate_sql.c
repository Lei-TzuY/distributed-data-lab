#include "generic_predicate.h"
#include "generic_sql.h"

#include <ctype.h>

typedef struct {
    uint32_t column_index;
    TinyDBValue value;
} GenericPredicateAssignment;

typedef struct {
    TinyDBGenericPredicate items[MAX_COLUMNS_PER_TABLE];
    uint32_t count;
} GenericPredicateList;

typedef struct {
    const TableSchema* schema;
    const GenericPredicateList* predicates;
    bool count_only;
    bool project_column;
    uint32_t projection_column_index;
    uint32_t matched;
    uint32_t emitted;
    uint32_t offset;
    uint32_t limit;
    bool has_limit;
    bool decode_failed;
} GenericPredicateSelectContext;

typedef struct {
    const GenericPredicateList* predicates;
    uint32_t* ids;
    uint32_t count;
    uint32_t capacity;
    bool allocation_failed;
    bool decode_failed;
} GenericPredicateCollector;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_predicate_base(
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

static bool parse_predicate_list(TinyDBGenericParser* parser,
                                 const TableSchema* schema,
                                 GenericPredicateList* predicates) {
    memset(predicates, 0, sizeof(*predicates));
    if (!tinydb_generic_parse_predicate(parser,
                                        schema,
                                        &predicates->items[0])) {
        return false;
    }
    predicates->count = 1;

    while (tinydb_generic_consume_word(parser, "and")) {
        if (predicates->count >= MAX_COLUMNS_PER_TABLE) return false;
        if (!tinydb_generic_parse_predicate(
                parser, schema, &predicates->items[predicates->count])) {
            return false;
        }
        predicates->count++;
    }
    return true;
}

static bool predicates_match(const GenericPredicateList* predicates,
                             const TinyDBValue* values) {
    for (uint32_t i = 0; i < predicates->count; i++) {
        const TinyDBGenericPredicate* predicate = &predicates->items[i];
        if (!tinydb_generic_predicate_matches(
                predicate, &values[predicate->column_index])) {
            return false;
        }
    }
    return true;
}

static bool find_primary_key_equality(const GenericPredicateList* predicates,
                                      uint32_t* id) {
    for (uint32_t i = 0; i < predicates->count; i++) {
        const TinyDBGenericPredicate* predicate = &predicates->items[i];
        if (predicate->column_index == 0 &&
            predicate->op == TINYDB_GENERIC_COMPARE_EQ) {
            *id = predicate->value.int_value;
            return true;
        }
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

static void emit_selected_record(GenericPredicateSelectContext* context,
                                 const TinyDBRecord* record,
                                 const TinyDBValue* values) {
    context->matched++;
    if (context->count_only) return;
    if (context->matched <= context->offset) return;
    if (context->has_limit && context->emitted >= context->limit) return;

    if (context->project_column) {
        print_value(&values[context->projection_column_index]);
    } else {
        tinydb_record_print(context->schema, record);
    }
    context->emitted++;
}

static bool visit_select_record(const TableSchema* schema,
                                const TinyDBRecord* record,
                                void* raw_context) {
    GenericPredicateSelectContext* context =
        (GenericPredicateSelectContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    if (!decode_values(schema, record, values)) {
        context->decode_failed = true;
        return false;
    }
    if (!predicates_match(context->predicates, values)) return true;

    emit_selected_record(context, record, values);
    if (!context->count_only && context->has_limit &&
        context->emitted >= context->limit) {
        return false;
    }
    return true;
}

static bool append_id(GenericPredicateCollector* collector, uint32_t id) {
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
    GenericPredicateCollector* collector =
        (GenericPredicateCollector*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    if (!decode_values(schema, record, values)) {
        collector->decode_failed = true;
        return false;
    }
    if (!predicates_match(collector->predicates, values)) return true;
    return append_id(collector, values[0].int_value);
}

static bool collect_matching_ids(Table* table,
                                 const TableSchema* schema,
                                 const GenericPredicateList* predicates,
                                 GenericPredicateCollector* collector) {
    memset(collector, 0, sizeof(*collector));
    collector->predicates = predicates;

    uint32_t id = 0;
    if (find_primary_key_equality(predicates, &id)) {
        TinyDBRecord record;
        if (!tinydb_record_find(table, schema, id, &record)) return true;
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        if (!decode_values(schema, &record, values)) {
            collector->decode_failed = true;
            return false;
        }
        if (predicates_match(predicates, values)) return append_id(collector, id);
        return true;
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

typedef enum {
    GENERIC_PROJECTION_STAR = 0,
    GENERIC_PROJECTION_COUNT,
    GENERIC_PROJECTION_COLUMN
} GenericProjectionKind;

static bool parse_projection_token(TinyDBGenericParser* parser,
                                   GenericProjectionKind* kind,
                                   char* column,
                                   size_t column_size) {
    column[0] = '\0';
    if (tinydb_generic_consume_char(parser, '*')) {
        *kind = GENERIC_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        *kind = GENERIC_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    if (!tinydb_generic_parse_identifier(parser, column, column_size)) return false;
    *kind = GENERIC_PROJECTION_COLUMN;
    return true;
}

static TinyDBGenericSqlStatus execute_predicate_select(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    GenericProjectionKind projection_kind;
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
    /* Wide rows must not enter the TinyDBRecord predicate executor. Delegate
     * them to the schema-sized range/equality wrappers further down the chain,
     * which can preserve payload-native V2 storage without narrowing. */
    if (schema->row_size > ROW_SIZE) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return execute_error(result, STATEMENT_SELECT, schema_message);
    }

    uint32_t projection_column_index = 0;
    if (projection_kind == GENERIC_PROJECTION_COLUMN) {
        int index = tinydb_generic_find_column_index(schema, projection_column);
        if (index < 0) {
            return syntax_error(
                result,
                STATEMENT_SELECT,
                "generic SELECT projection references an unknown column");
        }
        projection_column_index = (uint32_t)index;
    }

    if (!tinydb_generic_consume_word(&parser, "where")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    GenericPredicateList predicates;
    if (!parse_predicate_list(&parser, schema, &predicates)) {
        return syntax_error(
            result,
            STATEMENT_SELECT,
            "generic SELECT WHERE requires typed comparison predicates joined by AND");
    }

    bool has_limit = false;
    uint32_t limit = 0;
    uint32_t offset = 0;
    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &limit)) {
            return syntax_error(result, STATEMENT_SELECT, "LIMIT requires an integer");
        }
        has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset")) {
            if (!tinydb_generic_parse_uint32(&parser, &offset)) {
                return syntax_error(result, STATEMENT_SELECT, "OFFSET requires an integer");
            }
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &offset)) {
            return syntax_error(result, STATEMENT_SELECT, "OFFSET requires an integer");
        }
    }

    if (!tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_SELECT,
            "generic SELECT contains an unsupported clause after its predicates");
    }

    GenericPredicateSelectContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    context.predicates = &predicates;
    context.count_only = projection_kind == GENERIC_PROJECTION_COUNT;
    context.project_column = projection_kind == GENERIC_PROJECTION_COLUMN;
    context.projection_column_index = projection_column_index;
    context.offset = offset;
    context.limit = limit;
    context.has_limit = has_limit;

    uint32_t id = 0;
    if (find_primary_key_equality(&predicates, &id)) {
        TinyDBRecord record;
        if (tinydb_record_find(table, schema, id, &record)) {
            TinyDBValue values[MAX_COLUMNS_PER_TABLE];
            if (!decode_values(schema, &record, values)) {
                return execute_error(
                    result, STATEMENT_SELECT, "unable to decode generic record");
            }
            if (predicates_match(&predicates, values)) {
                emit_selected_record(&context, &record, values);
            }
        }
    } else {
        (void)tinydb_record_scan(table, schema, visit_select_record, &context);
        if (context.decode_failed) {
            return execute_error(
                result,
                STATEMENT_SELECT,
                "unable to decode generic record during predicate SELECT");
        }
    }

    if (context.count_only) {
        uint32_t count = context.matched;
        if (offset > 0) count = 0;
        if (has_limit && limit == 0) count = 0;
        printf("%u\n", count);
    }
    return success(result, STATEMENT_SELECT);
}

static bool parse_assignments(TinyDBGenericParser* parser,
                              const TableSchema* schema,
                              GenericPredicateAssignment* assignments,
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
        if (column_index <= 0) return false;
        if (!tinydb_generic_consume_char(parser, '=')) return false;

        GenericPredicateAssignment* assignment =
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

static TinyDBGenericSqlStatus apply_update(
    Table* table,
    TableSchema* schema,
    const GenericPredicateAssignment* assignments,
    uint32_t assignment_count,
    const GenericPredicateList* predicates,
    TinyDBGenericSqlResult* result) {
    GenericPredicateCollector collector;
    if (!collect_matching_ids(table, schema, predicates, &collector)) {
        free(collector.ids);
        return execute_error(result,
                             STATEMENT_UPDATE,
                             "unable to collect generic UPDATE predicate targets");
    }

    uint32_t ignored_id = 0;
    bool primary_key_equality = find_primary_key_equality(predicates, &ignored_id);
    if (primary_key_equality && collector.count == 0) {
        free(collector.ids);
        return execute_error(result, STATEMENT_UPDATE, "primary key not found");
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

static TinyDBGenericSqlStatus execute_predicate_update(
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
        return syntax_error(result,
                            STATEMENT_UPDATE,
                            "generic UPDATE requires SET assignments");
    }

    GenericPredicateAssignment assignments[MAX_COLUMNS_PER_TABLE];
    uint32_t assignment_count = 0;
    if (!parse_assignments(&parser,
                           schema,
                           assignments,
                           &assignment_count) ||
        assignment_count == 0) {
        return syntax_error(
            result,
            STATEMENT_UPDATE,
            "generic UPDATE requires typed non-primary-key assignments followed by WHERE");
    }

    GenericPredicateList predicates;
    if (!parse_predicate_list(&parser, schema, &predicates) ||
        !tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_UPDATE,
            "generic UPDATE WHERE requires typed comparison predicates joined by AND");
    }
    return apply_update(table,
                        schema,
                        assignments,
                        assignment_count,
                        &predicates,
                        result);
}

static TinyDBGenericSqlStatus apply_delete(
    Table* table,
    TableSchema* schema,
    const GenericPredicateList* predicates,
    TinyDBGenericSqlResult* result) {
    GenericPredicateCollector collector;
    if (!collect_matching_ids(table, schema, predicates, &collector)) {
        free(collector.ids);
        return execute_error(result,
                             STATEMENT_DELETE,
                             "unable to collect generic DELETE predicate targets");
    }

    uint32_t ignored_id = 0;
    bool primary_key_equality = find_primary_key_equality(predicates, &ignored_id);
    if (primary_key_equality && collector.count == 0) {
        free(collector.ids);
        return execute_error(result, STATEMENT_DELETE, "primary key not found");
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

static TinyDBGenericSqlStatus execute_predicate_delete(
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

    GenericPredicateList predicates;
    if (!parse_predicate_list(&parser, schema, &predicates) ||
        !tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            STATEMENT_DELETE,
            "generic DELETE WHERE requires typed comparison predicates joined by AND");
    }
    return apply_delete(table, schema, &predicates, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    TinyDBGenericSqlStatus status = execute_predicate_select(table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    status = execute_predicate_update(table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    status = execute_predicate_delete(table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;

    initialize_result(output);
    return tinydb_generic_sql_try_execute_predicate_base(table, sql, output);
}
