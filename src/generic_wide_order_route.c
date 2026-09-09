#include "generic_boolean.h"
#include "generic_predicate.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_order_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

typedef enum {
    WIDE_ORDER_PROJECTION_STAR = 0,
    WIDE_ORDER_PROJECTION_COLUMN
} WideOrderProjectionKind;

typedef enum {
    WIDE_ORDER_NOT_APPLICABLE = 0,
    WIDE_ORDER_VALID,
    WIDE_ORDER_INVALID
} WideOrderParseStatus;

typedef struct {
    TableSchema* schema;
    WideOrderProjectionKind projection_kind;
    uint32_t projection_column_index;
    uint32_t order_column_index;
    bool has_filter;
    TinyDBGenericBooleanExpression filter;
    bool descending;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} WideOrderSelect;

typedef struct {
    uint32_t id;
    TinyDBValue sort_key;
} WideOrderEntry;

typedef struct {
    const WideOrderSelect* select;
    WideOrderEntry* entries;
    uint32_t count;
    uint32_t capacity;
    bool allocation_failed;
    bool decode_failed;
} WideOrderCollector;

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

static bool legacy_fixed_row_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static TinyDBGenericSqlStatus result_error(TinyDBGenericSqlResult* result,
                                           TinyDBGenericSqlStatus status,
                                           const char* message) {
    result->status = status;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = status == TINYDB_GENERIC_SQL_EXECUTE_ERROR
        ? EXECUTE_KEY_NOT_FOUND
        : EXECUTE_SUCCESS;
    result->executed = false;
    snprintf(result->message,
             sizeof(result->message),
             "%s",
             message != NULL ? message : "wide ORDER BY failed");
    return status;
}

static TinyDBGenericSqlStatus result_success(TinyDBGenericSqlResult* result) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    result->message[0] = '\0';
    return result->status;
}

static bool consume_limit_offset(TinyDBGenericParser* parser,
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

static WideOrderParseStatus parse_wide_order(Table* table,
                                             const char* sql,
                                             WideOrderSelect* select,
                                             char* message,
                                             size_t message_size) {
    if (table == NULL || sql == NULL || select == NULL) {
        return WIDE_ORDER_NOT_APPLICABLE;
    }
    memset(select, 0, sizeof(*select));
    if (message != NULL && message_size > 0u) message[0] = '\0';

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) {
        return WIDE_ORDER_NOT_APPLICABLE;
    }

    char projection[MAX_NAME_SIZE];
    projection[0] = '\0';
    bool projection_star = tinydb_generic_consume_char(&parser, '*');
    if (!projection_star) {
        TinyDBGenericParser count_backup = parser;
        if (tinydb_generic_consume_word(&parser, "count") &&
            tinydb_generic_consume_char(&parser, '(') &&
            tinydb_generic_consume_char(&parser, '*') &&
            tinydb_generic_consume_char(&parser, ')')) {
            if (message != NULL && message_size > 0u) {
                snprintf(message,
                         message_size,
                         "%s",
                         "wide ORDER BY currently supports * or a single column projection, not COUNT(*)");
            }
            snprintf(projection, sizeof(projection), "%s", "__count__");
        } else {
            parser = count_backup;
            if (!tinydb_generic_parse_identifier(&parser,
                                                 projection,
                                                 sizeof(projection))) {
                return WIDE_ORDER_NOT_APPLICABLE;
            }
        }
    }

    if (!tinydb_generic_consume_word(&parser, "from")) {
        return WIDE_ORDER_NOT_APPLICABLE;
    }
    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return WIDE_ORDER_NOT_APPLICABLE;
    }
    select->schema = find_schema(table, table_name);
    if (select->schema == NULL || select->schema->row_size <= ROW_SIZE ||
        legacy_fixed_row_shape(select->schema)) {
        return WIDE_ORDER_NOT_APPLICABLE;
    }

    if (projection_star) {
        select->projection_kind = WIDE_ORDER_PROJECTION_STAR;
    } else if (strcmp(projection, "__count__") == 0) {
        select->projection_kind = WIDE_ORDER_PROJECTION_STAR;
    } else {
        int projection_index =
            tinydb_generic_find_column_index(select->schema, projection);
        if (projection_index < 0) {
            return WIDE_ORDER_NOT_APPLICABLE;
        }
        select->projection_kind = WIDE_ORDER_PROJECTION_COLUMN;
        select->projection_column_index = (uint32_t)projection_index;
    }

    if (tinydb_generic_consume_word(&parser, "where")) {
        select->has_filter = true;
        if (!tinydb_generic_parse_boolean_expression(&parser,
                                                     select->schema,
                                                     &select->filter)) {
            return WIDE_ORDER_NOT_APPLICABLE;
        }
    }

    if (!tinydb_generic_consume_word(&parser, "order")) {
        return WIDE_ORDER_NOT_APPLICABLE;
    }
    if (!tinydb_generic_consume_word(&parser, "by")) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "%s", "wide ORDER requires BY");
        }
        return WIDE_ORDER_INVALID;
    }

    if (strcmp(projection, "__count__") == 0) {
        return WIDE_ORDER_INVALID;
    }

    char order_column[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         order_column,
                                         sizeof(order_column))) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "%s",
                     "wide ORDER BY requires a column name");
        }
        return WIDE_ORDER_INVALID;
    }
    int order_index = tinydb_generic_find_column_index(select->schema,
                                                       order_column);
    if (order_index < 0) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "%s",
                     "wide ORDER BY references an unknown column");
        }
        return WIDE_ORDER_INVALID;
    }
    if (select->schema->columns[order_index].type != COL_TYPE_INT &&
        select->schema->columns[order_index].type != COL_TYPE_VARCHAR) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "%s",
                     "wide ORDER BY column type is not sortable");
        }
        return WIDE_ORDER_INVALID;
    }
    select->order_column_index = (uint32_t)order_index;

    if (tinydb_generic_consume_word(&parser, "desc")) {
        select->descending = true;
    } else {
        (void)tinydb_generic_consume_word(&parser, "asc");
    }

    if (!consume_limit_offset(&parser,
                              &select->has_limit,
                              &select->limit,
                              &select->offset) ||
        !tinydb_generic_consume_end(&parser)) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "%s",
                     "wide ORDER BY supports only ASC/DESC followed by LIMIT/OFFSET");
        }
        return WIDE_ORDER_INVALID;
    }
    return WIDE_ORDER_VALID;
}

static bool append_entry(WideOrderCollector* collector,
                         uint32_t id,
                         const TinyDBValue* sort_key) {
    if (collector->count == collector->capacity) {
        uint32_t next_capacity = collector->capacity == 0u
            ? 32u
            : collector->capacity * 2u;
        if (next_capacity <= collector->capacity) {
            collector->allocation_failed = true;
            return false;
        }
#if SIZE_MAX < UINT32_MAX
        if ((size_t)next_capacity > SIZE_MAX / sizeof(WideOrderEntry)) {
            collector->allocation_failed = true;
            return false;
        }
#endif
        WideOrderEntry* grown = (WideOrderEntry*)realloc(
            collector->entries,
            (size_t)next_capacity * sizeof(WideOrderEntry));
        if (grown == NULL) {
            collector->allocation_failed = true;
            return false;
        }
        collector->entries = grown;
        collector->capacity = next_capacity;
    }
    collector->entries[collector->count].id = id;
    collector->entries[collector->count].sort_key = *sort_key;
    collector->count++;
    return true;
}

static bool collect_ordered_payload(const TableSchema* schema,
                                    const TinyDBRecordPayload* payload,
                                    void* raw_context) {
    WideOrderCollector* collector = (WideOrderCollector*)raw_context;
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
        value_count != schema->num_columns ||
        values[0].type != COL_TYPE_INT ||
        collector->select->order_column_index >= value_count) {
        collector->decode_failed = true;
        return false;
    }
    if (collector->select->has_filter &&
        !tinydb_generic_boolean_matches(&collector->select->filter,
                                        values,
                                        value_count)) {
        return true;
    }
    return append_entry(
        collector,
        values[0].int_value,
        &values[collector->select->order_column_index]);
}

static int compare_entries(const void* left, const void* right) {
    const WideOrderEntry* a = (const WideOrderEntry*)left;
    const WideOrderEntry* b = (const WideOrderEntry*)right;
    int result = 0;
    if (a->sort_key.type == COL_TYPE_INT && b->sort_key.type == COL_TYPE_INT) {
        result = a->sort_key.int_value < b->sort_key.int_value
            ? -1
            : (a->sort_key.int_value > b->sort_key.int_value ? 1 : 0);
    } else if (a->sort_key.type == COL_TYPE_VARCHAR &&
               b->sort_key.type == COL_TYPE_VARCHAR) {
        result = strcmp(a->sort_key.text, b->sort_key.text);
    } else {
        result = (int)a->sort_key.type - (int)b->sort_key.type;
    }
    if (result != 0) return result;
    return a->id < b->id ? -1 : (a->id > b->id ? 1 : 0);
}

static void wide_order_print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static void wide_order_print_row(const TinyDBValue* values,
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

static bool emit_id(Table* table,
                    const WideOrderSelect* select,
                    uint32_t id,
                    char* message,
                    size_t message_size) {
    TinyDBRecordPayload payload;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0u;
    if (!tinydb_record_payload_find(table,
                                    select->schema,
                                    id,
                                    &payload,
                                    message,
                                    message_size) ||
        !tinydb_record_payload_decode_values(select->schema,
                                             &payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &value_count,
                                             message,
                                             message_size) ||
        value_count != select->schema->num_columns) {
        return false;
    }

    if (select->projection_kind == WIDE_ORDER_PROJECTION_COLUMN) {
        wide_order_print_value(&values[select->projection_column_index]);
    } else {
        wide_order_print_row(values, value_count);
    }
    return true;
}

static TinyDBGenericSqlStatus execute_wide_order(Table* table,
                                                 const WideOrderSelect* select,
                                                 TinyDBGenericSqlResult* result) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    if (!tinydb_record_payload_schema_supported(select->schema,
                                                message,
                                                sizeof(message))) {
        return result_error(result,
                            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                            message);
    }

    WideOrderCollector collector;
    memset(&collector, 0, sizeof(collector));
    collector.select = select;
    bool complete = false;
    (void)tinydb_record_payload_scan(table,
                                    select->schema,
                                    collect_ordered_payload,
                                    &collector,
                                    &complete,
                                    message,
                                    sizeof(message));
    if (!complete || collector.decode_failed || collector.allocation_failed) {
        free(collector.entries);
        return result_error(
            result,
            TINYDB_GENERIC_SQL_EXECUTE_ERROR,
            message[0] != '\0'
                ? message
                : "unable to collect schema-sized rows for wide ORDER BY");
    }

    if (collector.count > 1u) {
        qsort(collector.entries,
              collector.count,
              sizeof(collector.entries[0]),
              compare_entries);
    }

    uint32_t emitted = 0u;
    for (uint32_t logical = select->offset; logical < collector.count; logical++) {
        if (select->has_limit && emitted >= select->limit) break;
        uint32_t index = select->descending
            ? collector.count - 1u - logical
            : logical;
        if (!emit_id(table,
                     select,
                     collector.entries[index].id,
                     message,
                     sizeof(message))) {
            free(collector.entries);
            return result_error(
                result,
                TINYDB_GENERIC_SQL_EXECUTE_ERROR,
                message[0] != '\0'
                    ? message
                    : "unable to fetch a sorted schema-sized row");
        }
        emitted++;
    }

    free(collector.entries);
    return result_success(result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    WideOrderSelect select;
    char message[TINYDB_GENERIC_SQL_MESSAGE_MAX];
    WideOrderParseStatus parse = parse_wide_order(table,
                                                  sql,
                                                  &select,
                                                  message,
                                                  sizeof(message));
    if (parse == WIDE_ORDER_VALID) {
        return execute_wide_order(table, &select, output);
    }
    if (parse == WIDE_ORDER_INVALID) {
        return result_error(output,
                            TINYDB_GENERIC_SQL_SYNTAX_ERROR,
                            message[0] != '\0'
                                ? message
                                : "unsupported wide ORDER BY query");
    }
    return tinydb_generic_sql_try_execute_wide_order_base(table, sql, output);
}
