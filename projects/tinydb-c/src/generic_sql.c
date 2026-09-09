#include "generic_sql.h"

#include <ctype.h>
#include <errno.h>

#define GENERIC_SQL_MAX_VALUES MAX_COLUMNS_PER_TABLE

typedef struct {
    const char* current;
} GenericParser;

typedef struct {
    const TableSchema* schema;
    bool has_filter;
    uint32_t filter_column_index;
    TinyDBValue filter_value;
    bool project_column;
    uint32_t projection_column_index;
    uint32_t matched;
    uint32_t emitted;
    uint32_t offset;
    uint32_t limit;
    bool has_limit;
    bool count_only;
} GenericSelectContext;

typedef struct {
    uint32_t column_index;
    TinyDBValue value;
} GenericAssignment;

typedef struct {
    uint32_t column_index;
    TinyDBValue value;
} GenericPredicate;

typedef struct {
    const TableSchema* schema;
    GenericPredicate predicate;
    uint32_t* ids;
    uint32_t count;
    uint32_t capacity;
    bool allocation_failed;
    bool decode_failed;
} GenericKeyCollector;

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static void set_message(TinyDBGenericSqlResult* result, const char* message) {
    snprintf(result->message, sizeof(result->message), "%s", message);
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

static void skip_spaces(GenericParser* parser) {
    while (isspace((unsigned char)*parser->current)) parser->current++;
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool consume_word(GenericParser* parser, const char* word) {
    GenericParser backup = *parser;
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

static bool consume_char(GenericParser* parser, char expected) {
    skip_spaces(parser);
    if (*parser->current != expected) return false;
    parser->current++;
    return true;
}

static bool parse_identifier(GenericParser* parser,
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

static bool parse_uint32(GenericParser* parser, uint32_t* value) {
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

static bool parse_string(GenericParser* parser, char* output, size_t output_size) {
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

static bool consume_end(GenericParser* parser) {
    skip_spaces(parser);
    if (*parser->current == ';') parser->current++;
    skip_spaces(parser);
    return *parser->current == '\0';
}

static TableSchema* find_schema_exact(Table* table, const char* name) {
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
    /*
     * Keep the exact compatibility predicate used by multitable.c. Older
     * CREATE TABLE metadata records VARCHAR columns as 256 bytes each even
     * though the legacy physical Row slot is 293 bytes, so row_size itself
     * cannot decide which execution path owns the table.
     */
    return schema != NULL &&
           schema->num_columns == 3 &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool parse_target_after_prefix(const char* sql,
                                      const char* first,
                                      const char* second,
                                      char* table_name,
                                      size_t table_name_size) {
    GenericParser parser;
    parser.current = sql;
    if (!consume_word(&parser, first)) return false;
    if (second != NULL && !consume_word(&parser, second)) return false;
    return parse_identifier(&parser, table_name, table_name_size);
}

static bool parse_select_projection_for_routing(GenericParser* parser) {
    if (consume_char(parser, '*')) return true;

    GenericParser backup = *parser;
    if (consume_word(parser, "count") &&
        consume_char(parser, '(') &&
        consume_char(parser, '*') &&
        consume_char(parser, ')')) {
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    return parse_identifier(parser, column, sizeof(column));
}

static TableSchema* resolve_generic_target(Table* table,
                                           const char* sql,
                                           StatementType* type) {
    char table_name[MAX_NAME_SIZE];
    if (parse_target_after_prefix(sql,
                                  "insert",
                                  "into",
                                  table_name,
                                  sizeof(table_name))) {
        *type = STATEMENT_INSERT;
    } else if (parse_target_after_prefix(sql,
                                         "update",
                                         NULL,
                                         table_name,
                                         sizeof(table_name))) {
        *type = STATEMENT_UPDATE;
    } else if (parse_target_after_prefix(sql,
                                         "delete",
                                         "from",
                                         table_name,
                                         sizeof(table_name))) {
        *type = STATEMENT_DELETE;
    } else {
        GenericParser parser;
        parser.current = sql;
        if (!consume_word(&parser, "select") ||
            !parse_select_projection_for_routing(&parser) ||
            !consume_word(&parser, "from") ||
            !parse_identifier(&parser, table_name, sizeof(table_name))) {
            return NULL;
        }
        *type = STATEMENT_SELECT;
    }

    TableSchema* schema = find_schema_exact(table, table_name);
    if (schema == NULL || is_legacy_fixed_row_schema(schema)) return NULL;
    return schema;
}

static TinyDBGenericSqlStatus generic_syntax_error(TinyDBGenericSqlResult* result,
                                                    StatementType type,
                                                    const char* message) {
    result->status = TINYDB_GENERIC_SQL_SYNTAX_ERROR;
    result->statement_type = type;
    result->statement_type_valid = true;
    set_message(result, message);
    return result->status;
}

static TinyDBGenericSqlStatus generic_execute_error(TinyDBGenericSqlResult* result,
                                                     StatementType type,
                                                     ExecuteResult execute_result,
                                                     const char* message) {
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = execute_result;
    set_message(result, message);
    return result->status;
}

static TinyDBGenericSqlStatus generic_success(TinyDBGenericSqlResult* result,
                                              StatementType type) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = type;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
}

static bool parse_value_for_column(GenericParser* parser,
                                   const TableColumn* column,
                                   TinyDBValue* value) {
    memset(value, 0, sizeof(*value));
    value->type = column->type;
    if (column->type == COL_TYPE_INT) {
        return parse_uint32(parser, &value->int_value);
    }
    return parse_string(parser, value->text, sizeof(value->text));
}

static bool values_equal(const TinyDBValue* left, const TinyDBValue* right) {
    if (left->type != right->type) return false;
    if (left->type == COL_TYPE_INT) {
        return left->int_value == right->int_value;
    }
    return strcmp(left->text, right->text) == 0;
}

static bool decode_record_values(const TableSchema* schema,
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

static bool parse_predicate(GenericParser* parser,
                            const TableSchema* schema,
                            GenericPredicate* predicate) {
    char column[MAX_NAME_SIZE];
    if (!parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = find_column_index(schema, column);
    if (column_index < 0 || !consume_char(parser, '=')) return false;

    predicate->column_index = (uint32_t)column_index;
    return parse_value_for_column(parser,
                                  &schema->columns[predicate->column_index],
                                  &predicate->value);
}

static bool append_collected_id(GenericKeyCollector* collector, uint32_t id) {
    if (collector->count == collector->capacity) {
        uint32_t new_capacity = collector->capacity == 0 ? 16u : collector->capacity * 2u;
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

static bool collect_matching_key(const TableSchema* schema,
                                 const TinyDBRecord* record,
                                 void* raw_context) {
    GenericKeyCollector* collector = (GenericKeyCollector*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    if (!decode_record_values(schema, record, values)) {
        collector->decode_failed = true;
        return false;
    }
    if (!values_equal(&values[collector->predicate.column_index],
                      &collector->predicate.value)) {
        return true;
    }
    return append_collected_id(collector, values[0].int_value);
}

static bool collect_matching_ids(Table* table,
                                 const TableSchema* schema,
                                 const GenericPredicate* predicate,
                                 GenericKeyCollector* collector) {
    memset(collector, 0, sizeof(*collector));
    collector->schema = schema;
    collector->predicate = *predicate;

    if (predicate->column_index == 0) {
        TinyDBRecord record;
        if (tinydb_record_find(table,
                               schema,
                               predicate->value.int_value,
                               &record)) {
            if (!append_collected_id(collector, predicate->value.int_value)) {
                return false;
            }
        }
        return true;
    }

    (void)tinydb_record_scan(table, schema, collect_matching_key, collector);
    return !collector->allocation_failed && !collector->decode_failed;
}

static bool begin_internal_statement_transaction(Table* table) {
    if (table->in_transaction) return false;
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    return true;
}

static void finish_internal_statement_transaction(Table* table,
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

static TinyDBGenericSqlStatus execute_insert(Table* table,
                                              TableSchema* schema,
                                              const char* sql,
                                              TinyDBGenericSqlResult* result) {
    GenericParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];
    TinyDBValue values[GENERIC_SQL_MAX_VALUES];
    memset(values, 0, sizeof(values));

    if (!consume_word(&parser, "insert") ||
        !consume_word(&parser, "into") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name) ||
        !consume_word(&parser, "values") ||
        !consume_char(&parser, '(')) {
        return generic_syntax_error(result,
                                    STATEMENT_INSERT,
                                    "generic INSERT syntax is INSERT INTO table VALUES (...)");
    }

    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (!parse_value_for_column(&parser, &schema->columns[i], &values[i])) {
            return generic_syntax_error(result,
                                        STATEMENT_INSERT,
                                        "generic INSERT value does not match the target column type");
        }
        if (i + 1 < schema->num_columns && !consume_char(&parser, ',')) {
            return generic_syntax_error(result,
                                        STATEMENT_INSERT,
                                        "generic INSERT value count does not match the table schema");
        }
    }

    if (!consume_char(&parser, ')') || !consume_end(&parser)) {
        return generic_syntax_error(result,
                                    STATEMENT_INSERT,
                                    "generic INSERT has trailing or extra values");
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_insert(table,
                              schema,
                              values,
                              schema->num_columns,
                              message,
                              sizeof(message))) {
        ExecuteResult execute_result = strcmp(message, "duplicate primary key") == 0
            ? EXECUTE_DUPLICATE_KEY
            : EXECUTE_KEY_NOT_FOUND;
        return generic_execute_error(result,
                                     STATEMENT_INSERT,
                                     execute_result,
                                     message);
    }

    return generic_success(result, STATEMENT_INSERT);
}

static TinyDBGenericSqlStatus execute_update(Table* table,
                                              TableSchema* schema,
                                              const char* sql,
                                              TinyDBGenericSqlResult* result) {
    GenericParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];
    GenericAssignment assignments[MAX_COLUMNS_PER_TABLE];
    uint32_t assignment_count = 0;

    if (!consume_word(&parser, "update") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name) ||
        !consume_word(&parser, "set")) {
        return generic_syntax_error(
            result,
            STATEMENT_UPDATE,
            "generic UPDATE syntax is UPDATE table SET column=value WHERE column=value");
    }

    while (assignment_count < MAX_COLUMNS_PER_TABLE) {
        char column_name[MAX_NAME_SIZE];
        if (!parse_identifier(&parser, column_name, sizeof(column_name))) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE requires a column assignment");
        }
        int column_index = find_column_index(schema, column_name);
        if (column_index < 0) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE references an unknown column");
        }
        if (column_index == 0) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE cannot change the primary-key id");
        }
        if (!consume_char(&parser, '=')) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE assignment requires '='");
        }

        GenericAssignment* assignment = &assignments[assignment_count++];
        assignment->column_index = (uint32_t)column_index;
        if (!parse_value_for_column(&parser,
                                    &schema->columns[column_index],
                                    &assignment->value)) {
            return generic_syntax_error(result,
                                        STATEMENT_UPDATE,
                                        "generic UPDATE value does not match the target column type");
        }

        if (consume_word(&parser, "where")) break;
        if (!consume_char(&parser, ',')) {
            return generic_syntax_error(
                result,
                STATEMENT_UPDATE,
                "generic UPDATE requires WHERE after its assignments");
        }
    }

    GenericPredicate predicate;
    memset(&predicate, 0, sizeof(predicate));
    if (assignment_count == 0 ||
        !parse_predicate(&parser, schema, &predicate) ||
        !consume_end(&parser)) {
        return generic_syntax_error(
            result,
            STATEMENT_UPDATE,
            "generic UPDATE requires a typed equality predicate in WHERE");
    }

    GenericKeyCollector collector;
    if (!collect_matching_ids(table, schema, &predicate, &collector)) {
        free(collector.ids);
        return generic_execute_error(result,
                                     STATEMENT_UPDATE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     "unable to collect generic UPDATE target rows");
    }

    if (predicate.column_index == 0 && collector.count == 0) {
        free(collector.ids);
        return generic_execute_error(result,
                                     STATEMENT_UPDATE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     "primary key not found");
    }
    if (collector.count == 0) {
        free(collector.ids);
        return generic_success(result, STATEMENT_UPDATE);
    }

    /*
     * A non-PK predicate can match many rows. Collect every primary key before
     * touching the tree, then mutate by key. This prevents a split/merge from
     * invalidating the scan cursor. In autocommit mode the whole statement is
     * committed as one Pager transaction rather than one commit per row.
     */
    bool started_transaction = begin_internal_statement_transaction(table);
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    bool success = true;

    for (uint32_t i = 0; i < collector.count; i++) {
        uint32_t id = collector.ids[i];
        TinyDBRecord existing;
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t value_count = 0;

        if (!tinydb_record_find(table, schema, id, &existing) ||
            !tinydb_record_decode(schema,
                                  &existing,
                                  values,
                                  MAX_COLUMNS_PER_TABLE,
                                  &value_count,
                                  message,
                                  sizeof(message))) {
            success = false;
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
            success = false;
            break;
        }
    }

    finish_internal_statement_transaction(table, started_transaction, success);
    free(collector.ids);

    if (!success) {
        return generic_execute_error(result,
                                     STATEMENT_UPDATE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     message[0] != '\0'
                                         ? message
                                         : "generic UPDATE failed");
    }
    return generic_success(result, STATEMENT_UPDATE);
}

static TinyDBGenericSqlStatus execute_delete(Table* table,
                                              TableSchema* schema,
                                              const char* sql,
                                              TinyDBGenericSqlResult* result) {
    GenericParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];

    if (!consume_word(&parser, "delete") ||
        !consume_word(&parser, "from") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name)) {
        return generic_syntax_error(
            result,
            STATEMENT_DELETE,
            "generic DELETE syntax is DELETE FROM table [WHERE column=value]");
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (consume_end(&parser)) {
        (void)tinydb_record_delete_all(table, schema, message, sizeof(message));
        if (message[0] != '\0') {
            return generic_execute_error(result,
                                         STATEMENT_DELETE,
                                         EXECUTE_KEY_NOT_FOUND,
                                         message);
        }
        return generic_success(result, STATEMENT_DELETE);
    }

    GenericPredicate predicate;
    memset(&predicate, 0, sizeof(predicate));
    if (!consume_word(&parser, "where") ||
        !parse_predicate(&parser, schema, &predicate) ||
        !consume_end(&parser)) {
        return generic_syntax_error(
            result,
            STATEMENT_DELETE,
            "generic DELETE requires a typed equality predicate in WHERE");
    }

    GenericKeyCollector collector;
    if (!collect_matching_ids(table, schema, &predicate, &collector)) {
        free(collector.ids);
        return generic_execute_error(result,
                                     STATEMENT_DELETE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     "unable to collect generic DELETE target rows");
    }

    if (predicate.column_index == 0 && collector.count == 0) {
        free(collector.ids);
        return generic_execute_error(result,
                                     STATEMENT_DELETE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     "primary key not found");
    }
    if (collector.count == 0) {
        free(collector.ids);
        return generic_success(result, STATEMENT_DELETE);
    }

    bool started_transaction = begin_internal_statement_transaction(table);
    message[0] = '\0';
    bool success = true;
    for (uint32_t i = 0; i < collector.count; i++) {
        if (!tinydb_record_delete(table,
                                  schema,
                                  collector.ids[i],
                                  message,
                                  sizeof(message))) {
            success = false;
            break;
        }
    }

    finish_internal_statement_transaction(table, started_transaction, success);
    free(collector.ids);

    if (!success) {
        return generic_execute_error(result,
                                     STATEMENT_DELETE,
                                     EXECUTE_KEY_NOT_FOUND,
                                     message[0] != '\0'
                                         ? message
                                         : "generic DELETE failed");
    }
    return generic_success(result, STATEMENT_DELETE);
}

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static bool record_matches_filter(const GenericSelectContext* context,
                                  const TinyDBValue* values) {
    if (!context->has_filter) return true;
    return values_equal(&values[context->filter_column_index],
                        &context->filter_value);
}

static void print_selected_record(const GenericSelectContext* context,
                                  const TinyDBRecord* record,
                                  const TinyDBValue* values) {
    if (context->project_column) {
        print_value(&values[context->projection_column_index]);
    } else {
        tinydb_record_print(context->schema, record);
    }
}

static bool scan_select_record(const TableSchema* schema,
                               const TinyDBRecord* record,
                               void* raw_context) {
    GenericSelectContext* context = (GenericSelectContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    if (!decode_record_values(schema, record, values)) return false;
    if (!record_matches_filter(context, values)) return true;

    context->matched++;
    if (context->count_only) return true;
    if (context->matched <= context->offset) return true;
    if (context->has_limit && context->emitted >= context->limit) return false;

    print_selected_record(context, record, values);
    context->emitted++;
    return true;
}

static bool parse_select_projection(GenericParser* parser,
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

    GenericParser backup = *parser;
    if (consume_word(parser, "count") &&
        consume_char(parser, '(') &&
        consume_char(parser, '*') &&
        consume_char(parser, ')')) {
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

static TinyDBGenericSqlStatus execute_select(Table* table,
                                              TableSchema* schema,
                                              const char* sql,
                                              TinyDBGenericSqlResult* result) {
    GenericParser parser;
    parser.current = sql;
    char table_name[MAX_NAME_SIZE];
    bool count_only = false;
    bool project_column = false;
    uint32_t projection_column_index = 0;
    bool has_filter = false;
    uint32_t filter_column_index = 0;
    TinyDBValue filter_value;
    memset(&filter_value, 0, sizeof(filter_value));
    bool has_limit = false;
    uint32_t limit = 0;
    uint32_t offset = 0;

    if (!consume_word(&parser, "select")) {
        return generic_syntax_error(result, STATEMENT_SELECT, "invalid generic SELECT");
    }
    if (!parse_select_projection(&parser,
                                 schema,
                                 &count_only,
                                 &project_column,
                                 &projection_column_index)) {
        return generic_syntax_error(
            result,
            STATEMENT_SELECT,
            "generic SELECT projection references an unknown or unsupported column");
    }

    if (!consume_word(&parser, "from") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name)) {
        return generic_syntax_error(result,
                                    STATEMENT_SELECT,
                                    "generic SELECT requires a catalog-backed table target");
    }

    if (consume_word(&parser, "where")) {
        GenericPredicate predicate;
        memset(&predicate, 0, sizeof(predicate));
        if (!parse_predicate(&parser, schema, &predicate)) {
            return generic_syntax_error(
                result,
                STATEMENT_SELECT,
                "generic SELECT WHERE references an unknown column, operator, or typed value");
        }
        filter_column_index = predicate.column_index;
        filter_value = predicate.value;
        has_filter = true;
    }

    if (consume_word(&parser, "limit")) {
        if (!parse_uint32(&parser, &limit)) {
            return generic_syntax_error(result,
                                        STATEMENT_SELECT,
                                        "LIMIT requires an integer");
        }
        has_limit = true;
        if (consume_word(&parser, "offset")) {
            if (!parse_uint32(&parser, &offset)) {
                return generic_syntax_error(result,
                                            STATEMENT_SELECT,
                                            "OFFSET requires an integer");
            }
        }
    } else if (consume_word(&parser, "offset")) {
        if (!parse_uint32(&parser, &offset)) {
            return generic_syntax_error(result,
                                        STATEMENT_SELECT,
                                        "OFFSET requires an integer");
        }
    }

    if (!consume_end(&parser)) {
        return generic_syntax_error(result,
                                    STATEMENT_SELECT,
                                    "generic SELECT contains an unsupported clause");
    }

    GenericSelectContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    context.has_filter = has_filter;
    context.filter_column_index = filter_column_index;
    context.filter_value = filter_value;
    context.project_column = project_column;
    context.projection_column_index = projection_column_index;
    context.offset = offset;
    context.limit = limit;
    context.has_limit = has_limit;
    context.count_only = count_only;

    /*
     * Preserve the direct B+ tree primary-key path whenever WHERE targets id.
     * All other schema-aware equality predicates use a decoded record scan.
     */
    if (has_filter && filter_column_index == 0) {
        TinyDBRecord record;
        bool found = tinydb_record_find(table,
                                        schema,
                                        filter_value.int_value,
                                        &record);
        if (found) {
            TinyDBValue values[MAX_COLUMNS_PER_TABLE];
            if (!decode_record_values(schema, &record, values)) {
                return generic_execute_error(result,
                                             STATEMENT_SELECT,
                                             EXECUTE_KEY_NOT_FOUND,
                                             "unable to decode generic record");
            }
            context.matched = 1;
            if (!count_only && offset == 0 && (!has_limit || limit > 0)) {
                print_selected_record(&context, &record, values);
                context.emitted = 1;
            }
        }
    } else {
        (void)tinydb_record_scan(table, schema, scan_select_record, &context);
    }

    if (count_only) {
        uint32_t count = context.matched;
        if (offset > 0) count = 0;
        if (has_limit && limit == 0) count = 0;
        printf("%u\n", count);
    }

    return generic_success(result, STATEMENT_SELECT);
}

static bool parse_projection_token(GenericParser* parser,
                                   bool* count_only,
                                   bool* star,
                                   char* column,
                                   size_t column_size) {
    *count_only = false;
    *star = false;
    column[0] = '\0';

    if (consume_char(parser, '*')) {
        *star = true;
        return true;
    }

    GenericParser backup = *parser;
    if (consume_word(parser, "count") &&
        consume_char(parser, '(') &&
        consume_char(parser, '*') &&
        consume_char(parser, ')')) {
        *count_only = true;
        return true;
    }
    *parser = backup;
    return parse_identifier(parser, column, column_size);
}

static void format_plan_value(const TinyDBValue* value,
                              char* output,
                              size_t output_size) {
    if (value->type == COL_TYPE_INT) {
        snprintf(output, output_size, "%u", value->int_value);
    } else {
        snprintf(output, output_size, "'%s'", value->text);
    }
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

    if (table == NULL || sql == NULL || plan == NULL) {
        return output->status;
    }

    GenericParser parser;
    parser.current = sql;
    if (!consume_word(&parser, "select")) return output->status;

    bool count_only = false;
    bool star = false;
    char projection_column[MAX_NAME_SIZE];
    if (!parse_projection_token(&parser,
                                &count_only,
                                &star,
                                projection_column,
                                sizeof(projection_column))) {
        return output->status;
    }

    char table_name[MAX_NAME_SIZE];
    if (!consume_word(&parser, "from") ||
        !parse_identifier(&parser, table_name, sizeof(table_name))) {
        return output->status;
    }

    TableSchema* schema = find_schema_exact(table, table_name);
    if (schema == NULL || is_legacy_fixed_row_schema(schema)) {
        return output->status;
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return generic_execute_error(output,
                                     STATEMENT_SELECT,
                                     EXECUTE_KEY_NOT_FOUND,
                                     schema_message);
    }

    if (count_only) {
        snprintf(plan->projection, sizeof(plan->projection), "COUNT(*)");
    } else if (star) {
        snprintf(plan->projection, sizeof(plan->projection), "*");
    } else {
        int projection_index = find_column_index(schema, projection_column);
        if (projection_index < 0) {
            return generic_syntax_error(
                output,
                STATEMENT_SELECT,
                "generic SELECT projection references an unknown column");
        }
        snprintf(plan->projection,
                 sizeof(plan->projection),
                 "%s",
                 schema->columns[projection_index].name);
    }

    uint32_t filter_column_index = 0;
    if (consume_word(&parser, "where")) {
        GenericPredicate predicate;
        memset(&predicate, 0, sizeof(predicate));
        if (!parse_predicate(&parser, schema, &predicate)) {
            return generic_syntax_error(
                output,
                STATEMENT_SELECT,
                "generic SELECT WHERE references an unknown column, operator, or typed value");
        }
        filter_column_index = predicate.column_index;
        plan->has_filter = true;
        snprintf(plan->filter_column,
                 sizeof(plan->filter_column),
                 "%s",
                 schema->columns[filter_column_index].name);
        format_plan_value(&predicate.value,
                          plan->filter_value,
                          sizeof(plan->filter_value));
    }

    if (consume_word(&parser, "limit")) {
        uint32_t ignored_limit = 0;
        if (!parse_uint32(&parser, &ignored_limit)) {
            return generic_syntax_error(output,
                                        STATEMENT_SELECT,
                                        "LIMIT requires an integer");
        }
        if (consume_word(&parser, "offset")) {
            uint32_t ignored_offset = 0;
            if (!parse_uint32(&parser, &ignored_offset)) {
                return generic_syntax_error(output,
                                            STATEMENT_SELECT,
                                            "OFFSET requires an integer");
            }
        }
    } else if (consume_word(&parser, "offset")) {
        uint32_t ignored_offset = 0;
        if (!parse_uint32(&parser, &ignored_offset)) {
            return generic_syntax_error(output,
                                        STATEMENT_SELECT,
                                        "OFFSET requires an integer");
        }
    }

    if (!consume_end(&parser)) {
        return generic_syntax_error(output,
                                    STATEMENT_SELECT,
                                    "generic SELECT contains an unsupported clause");
    }

    plan->applicable = true;
    plan->root_page_num = schema->root_page_num;
    plan->kind = plan->has_filter && filter_column_index == 0
        ? TINYDB_GENERIC_PLAN_PRIMARY_KEY_LOOKUP
        : TINYDB_GENERIC_PLAN_FULL_SCAN;
    snprintf(plan->table_name, sizeof(plan->table_name), "%s", schema->name);

    output->status = TINYDB_GENERIC_SQL_SUCCESS;
    output->statement_type = STATEMENT_SELECT;
    output->statement_type_valid = true;
    output->execute_result = EXECUTE_SUCCESS;
    return output->status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable) return;

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
        printf("  FILTER: %s = %s\n",
               plan->filter_column,
               plan->filter_value);
    } else {
        printf("  FILTER: none\n");
    }
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(Table* table,
                                                       const char* sql,
                                                       TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    if (table == NULL || sql == NULL) {
        return output->status;
    }

    StatementType type = STATEMENT_SELECT;
    TableSchema* schema = resolve_generic_target(table, sql, &type);
    if (schema == NULL) return output->status;

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return generic_execute_error(output,
                                     type,
                                     EXECUTE_KEY_NOT_FOUND,
                                     schema_message);
    }

    switch (type) {
        case STATEMENT_INSERT:
            return execute_insert(table, schema, sql, output);
        case STATEMENT_UPDATE:
            return execute_update(table, schema, sql, output);
        case STATEMENT_DELETE:
            return execute_delete(table, schema, sql, output);
        case STATEMENT_SELECT:
            return execute_select(table, schema, sql, output);
        default:
            return output->status;
    }
}
