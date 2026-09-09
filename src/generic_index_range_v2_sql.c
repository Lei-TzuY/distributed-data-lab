#include "generic_index_candidates.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    RANGE_V2_PROJECTION_STAR = 0,
    RANGE_V2_PROJECTION_COUNT,
    RANGE_V2_PROJECTION_COLUMN
} RangeV2ProjectionKind;

typedef struct {
    TableSchema* schema;
    GenericSecondaryIndex* index;
    TinyDBGenericPredicate predicate;
    RangeV2ProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} RangeV2Select;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_snapshot_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_compound_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
void tinydb_generic_sql_print_plan_compound_base(
    const TinyDBGenericSelectPlan* plan);

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static TinyDBGenericSqlStatus success(TinyDBGenericSqlResult* result) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
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

static GenericSecondaryIndex* find_single_column_index(
    Table* table,
    const TableSchema* schema,
    uint32_t column_index) {
    if (schema == NULL || column_index == 0 || column_index >= schema->num_columns) {
        return NULL;
    }
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && index->num_columns == 1 &&
            ci_equal(index->table_name, schema->name) &&
            ci_equal(index->column_name, schema->columns[column_index].name)) {
            return index;
        }
    }
    return NULL;
}

static bool parse_projection(TinyDBGenericParser* parser,
                             const TableSchema* schema,
                             RangeV2Select* select) {
    if (tinydb_generic_consume_char(parser, '*')) {
        select->projection_kind = RANGE_V2_PROJECTION_STAR;
        return true;
    }
    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        select->projection_kind = RANGE_V2_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = tinydb_generic_find_column_index(schema, column);
    if (column_index < 0) return false;
    select->projection_kind = RANGE_V2_PROJECTION_COLUMN;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool parse_range_select(Table* table,
                               const char* sql,
                               RangeV2Select* select) {
    memset(select, 0, sizeof(*select));
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) return false;

    TinyDBGenericParser routing = parser;
    if (tinydb_generic_consume_char(&routing, '*')) {
        /* projection consumed */
    } else {
        TinyDBGenericParser backup = routing;
        if (!(tinydb_generic_consume_word(&routing, "count") &&
              tinydb_generic_consume_char(&routing, '(') &&
              tinydb_generic_consume_char(&routing, '*') &&
              tinydb_generic_consume_char(&routing, ')'))) {
            routing = backup;
            char ignored[MAX_NAME_SIZE];
            if (!tinydb_generic_parse_identifier(&routing,
                                                 ignored,
                                                 sizeof(ignored))) {
                return false;
            }
        }
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_consume_word(&routing, "from") ||
        !tinydb_generic_parse_identifier(&routing,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }
    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL || is_legacy_fixed_row_schema(schema)) return false;
    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return false;
    }
    select->schema = schema;

    if (!parse_projection(&parser, schema, select) ||
        !tinydb_generic_consume_word(&parser, "from") ||
        !tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name)) ||
        !ci_equal(table_name, schema->name) ||
        !tinydb_generic_consume_word(&parser, "where") ||
        !tinydb_generic_parse_predicate(&parser, schema, &select->predicate)) {
        return false;
    }

    if (select->predicate.op == TINYDB_GENERIC_COMPARE_EQ ||
        select->predicate.op == TINYDB_GENERIC_COMPARE_LIKE ||
        select->predicate.column_index == 0) {
        return false;
    }
    select->index = find_single_column_index(table,
                                              schema,
                                              select->predicate.column_index);
    if (select->index == NULL) return false;

    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->limit)) return false;
        select->has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset")) {
            if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
    }
    return tinydb_generic_consume_end(&parser);
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

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static TinyDBGenericSqlStatus execute_range_select(
    Table* table,
    const RangeV2Select* select,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericIndexCandidates candidates;
    char message[TINYDB_GENERIC_SQL_MESSAGE_MAX] = {0};
    if (!tinydb_generic_index_collect_candidates(table,
                                                 select->schema,
                                                 select->index,
                                                 &select->predicate,
                                                 &candidates,
                                                 message,
                                                 sizeof(message))) {
        return execute_error(result,
                             message[0] != '\0' ? message
                                                 : "unable to collect range index candidates");
    }

    uint32_t matched = 0;
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < candidates.count; i++) {
        TinyDBRecord record;
        if (!tinydb_record_find(table,
                                select->schema,
                                candidates.ids[i],
                                &record)) {
            continue;
        }
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        if (!decode_values(select->schema, &record, values)) {
            tinydb_generic_index_candidates_free(&candidates);
            return execute_error(result, "unable to decode range index candidate");
        }
        if (!tinydb_generic_predicate_matches(
                &select->predicate,
                &values[select->predicate.column_index])) {
            continue;
        }
        matched++;
        if (select->projection_kind == RANGE_V2_PROJECTION_COUNT) continue;
        if (matched <= select->offset) continue;
        if (select->has_limit && emitted >= select->limit) break;
        if (select->projection_kind == RANGE_V2_PROJECTION_COLUMN) {
            print_value(&values[select->projection_column_index]);
        } else {
            tinydb_record_print(select->schema, &record);
        }
        emitted++;
    }

    if (select->projection_kind == RANGE_V2_PROJECTION_COUNT) {
        uint32_t count = matched;
        if (select->offset > 0) count = 0;
        if (select->has_limit && select->limit == 0) count = 0;
        printf("%u\n", count);
    }
    tinydb_generic_index_candidates_free(&candidates);
    return success(result);
}

static void format_predicate_value(const TinyDBGenericPredicate* predicate,
                                   char* output,
                                   size_t output_size) {
    if (predicate->value.type == COL_TYPE_INT) {
        snprintf(output, output_size, "%u", predicate->value.int_value);
    } else {
        snprintf(output, output_size, "'%s'", predicate->value.text);
    }
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    RangeV2Select select;
    if (parse_range_select(table, sql, &select)) {
        return execute_range_select(table, &select, output);
    }
    return tinydb_generic_sql_try_execute_snapshot_base(table, sql, output);
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

    RangeV2Select select;
    if (!parse_range_select(table, sql, &select)) {
        return tinydb_generic_sql_build_select_plan_compound_base(
            table, sql, plan, output);
    }

    plan->applicable = true;
    plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RANGE;
    plan->root_page_num = select.schema->root_page_num;
    plan->has_filter = true;
    snprintf(plan->table_name, sizeof(plan->table_name), "%s", select.schema->name);
    snprintf(plan->projection,
             sizeof(plan->projection),
             "%s",
             select.projection_kind == RANGE_V2_PROJECTION_STAR
                 ? "*"
                 : select.projection_kind == RANGE_V2_PROJECTION_COUNT
                       ? "COUNT(*)"
                       : select.schema->columns[select.projection_column_index].name);
    snprintf(plan->filter_column,
             sizeof(plan->filter_column),
             "%s",
             select.schema->columns[select.predicate.column_index].name);
    snprintf(plan->filter_operator,
             sizeof(plan->filter_operator),
             "%s",
             tinydb_generic_compare_op_text(select.predicate.op));
    format_predicate_value(&select.predicate,
                           plan->filter_value,
                           sizeof(plan->filter_value));
    snprintf(plan->index_name, sizeof(plan->index_name), "%s", select.index->name);

    output->status = TINYDB_GENERIC_SQL_SUCCESS;
    output->statement_type = STATEMENT_SELECT;
    output->statement_type_valid = true;
    output->execute_result = EXECUTE_SUCCESS;
    return output->status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable ||
        plan->kind != TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RANGE) {
        tinydb_generic_sql_print_plan_compound_base(plan);
        return;
    }
    printf("PLAN: GENERIC SECONDARY INDEX RANGE SCAN\n");
    printf("  TABLE: %s (root page %u)\n", plan->table_name, plan->root_page_num);
    printf("  INDEX: %s\n", plan->index_name);
    printf("  PROJECTION: %s\n", plan->projection);
    printf("  FILTER: %s %s %s\n",
           plan->filter_column,
           plan->filter_operator,
           plan->filter_value);
}
