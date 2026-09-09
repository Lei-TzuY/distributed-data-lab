#include "generic_boolean.h"
#include "generic_index_candidates.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_atomic_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_wide_boolean_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_wide_boolean_base(
    const TinyDBGenericSelectPlan* plan);

typedef enum {
    WIDE_BOOLEAN_PROJECTION_STAR = 0,
    WIDE_BOOLEAN_PROJECTION_COUNT,
    WIDE_BOOLEAN_PROJECTION_COLUMN
} WideBooleanProjectionKind;

typedef struct {
    TableSchema* schema;
    TinyDBGenericBooleanExpression expression;
    WideBooleanProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} WideBooleanSelect;

typedef struct {
    uint32_t* ids;
    uint32_t count;
    bool bounded;
} WideCandidateSet;

typedef struct {
    bool bounded;
    bool uses_secondary;
    bool uses_primary;
    bool contains_union;
    bool residual;
    uint32_t bounded_terms;
    uint32_t union_branches;
    char first_index_name[MAX_NAME_SIZE];
} WideCandidateShape;

static int wide_boolean_ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool wide_boolean_ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (wide_boolean_ci_char(*left) != wide_boolean_ci_char(*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool wide_boolean_legacy_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           wide_boolean_ci_equal(schema->columns[0].name, "id") &&
           wide_boolean_ci_equal(schema->columns[1].name, "username") &&
           wide_boolean_ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static TableSchema* wide_boolean_find_schema(Table* table,
                                             const char* table_name) {
    if (table == NULL || table_name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (wide_boolean_ci_equal(table->catalog.schemas[i].name, table_name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static GenericSecondaryIndex* wide_boolean_find_index(
    Table* table,
    const TableSchema* schema,
    uint32_t column_index) {
    if (table == NULL || schema == NULL || column_index == 0u ||
        column_index >= schema->num_columns) {
        return NULL;
    }
    for (uint32_t i = 0u; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && index->num_columns == 1u &&
            wide_boolean_ci_equal(index->table_name, schema->name) &&
            wide_boolean_ci_equal(index->column_name,
                                  schema->columns[column_index].name)) {
            return index;
        }
    }
    return NULL;
}

static bool wide_boolean_index_operator_supported(TinyDBGenericCompareOp op) {
    return op == TINYDB_GENERIC_COMPARE_EQ ||
           op == TINYDB_GENERIC_COMPARE_GT ||
           op == TINYDB_GENERIC_COMPARE_GTE ||
           op == TINYDB_GENERIC_COMPARE_LT ||
           op == TINYDB_GENERIC_COMPARE_LTE;
}

static bool wide_boolean_parse_projection(TinyDBGenericParser* parser,
                                          const TableSchema* schema,
                                          WideBooleanSelect* select) {
    if (tinydb_generic_consume_char(parser, '*')) {
        select->projection_kind = WIDE_BOOLEAN_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        select->projection_kind = WIDE_BOOLEAN_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    char column_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(parser,
                                         column_name,
                                         sizeof(column_name))) {
        return false;
    }
    int column_index = tinydb_generic_find_column_index(schema, column_name);
    if (column_index < 0) return false;
    select->projection_kind = WIDE_BOOLEAN_PROJECTION_COLUMN;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool wide_boolean_expression_contains_or(
    const TinyDBGenericBooleanExpression* expression) {
    if (expression == NULL) return false;
    for (uint32_t i = 0u; i < expression->count; i++) {
        if (expression->nodes[i].kind == TINYDB_GENERIC_BOOLEAN_OR) return true;
    }
    return false;
}

static bool wide_boolean_parse_select(Table* table,
                                      const char* sql,
                                      WideBooleanSelect* select) {
    if (table == NULL || sql == NULL || select == NULL) return false;
    memset(select, 0, sizeof(*select));

    TinyDBGenericParser routing;
    tinydb_generic_parser_init(&routing, sql);
    if (!tinydb_generic_consume_word(&routing, "select")) return false;

    TinyDBGenericParser projection_probe = routing;
    if (tinydb_generic_consume_char(&projection_probe, '*')) {
        /* projection consumed only to find the table */
    } else {
        TinyDBGenericParser backup = projection_probe;
        if (!(tinydb_generic_consume_word(&projection_probe, "count") &&
              tinydb_generic_consume_char(&projection_probe, '(') &&
              tinydb_generic_consume_char(&projection_probe, '*') &&
              tinydb_generic_consume_char(&projection_probe, ')'))) {
            projection_probe = backup;
            char ignored[MAX_NAME_SIZE];
            if (!tinydb_generic_parse_identifier(&projection_probe,
                                                 ignored,
                                                 sizeof(ignored))) {
                return false;
            }
        }
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_consume_word(&projection_probe, "from") ||
        !tinydb_generic_parse_identifier(&projection_probe,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }
    TableSchema* schema = wide_boolean_find_schema(table, table_name);
    if (schema == NULL || schema->row_size <= ROW_SIZE ||
        wide_boolean_legacy_shape(schema)) {
        return false;
    }
    select->schema = schema;

    TinyDBGenericParser parser = routing;
    if (!wide_boolean_parse_projection(&parser, schema, select) ||
        !tinydb_generic_consume_word(&parser, "from") ||
        !tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name)) ||
        !wide_boolean_ci_equal(table_name, schema->name) ||
        !tinydb_generic_consume_word(&parser, "where") ||
        !tinydb_generic_parse_boolean_expression(&parser,
                                                 schema,
                                                 &select->expression)) {
        return false;
    }

    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->limit)) return false;
        select->has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset") &&
            !tinydb_generic_parse_uint32(&parser, &select->offset)) {
            return false;
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
    }
    if (!tinydb_generic_consume_end(&parser)) return false;

    return select->expression.saw_grouping ||
           wide_boolean_expression_contains_or(&select->expression);
}

static void wide_candidate_free(WideCandidateSet* set) {
    if (set == NULL) return;
    free(set->ids);
    set->ids = NULL;
    set->count = 0u;
    set->bounded = false;
}

static int wide_candidate_compare(const void* left, const void* right) {
    uint32_t a = *(const uint32_t*)left;
    uint32_t b = *(const uint32_t*)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void wide_candidate_normalize(WideCandidateSet* set) {
    if (set == NULL || !set->bounded || set->count < 2u) return;
    qsort(set->ids,
          (size_t)set->count,
          sizeof(set->ids[0]),
          wide_candidate_compare);
    uint32_t write_index = 1u;
    for (uint32_t read_index = 1u; read_index < set->count; read_index++) {
        if (set->ids[read_index] != set->ids[write_index - 1u]) {
            set->ids[write_index++] = set->ids[read_index];
        }
    }
    set->count = write_index;
}

static void wide_candidate_move(WideCandidateSet* destination,
                                WideCandidateSet* source) {
    *destination = *source;
    source->ids = NULL;
    source->count = 0u;
    source->bounded = false;
}

static bool wide_candidate_intersection(WideCandidateSet* left,
                                        WideCandidateSet* right,
                                        WideCandidateSet* output) {
    memset(output, 0, sizeof(*output));
    output->bounded = true;
    wide_candidate_normalize(left);
    wide_candidate_normalize(right);
    uint32_t capacity = left->count < right->count ? left->count : right->count;
    if (capacity == 0u) return true;

    output->ids = (uint32_t*)malloc((size_t)capacity * sizeof(uint32_t));
    if (output->ids == NULL) return false;

    uint32_t i = 0u;
    uint32_t j = 0u;
    while (i < left->count && j < right->count) {
        uint32_t a = left->ids[i];
        uint32_t b = right->ids[j];
        if (a == b) {
            output->ids[output->count++] = a;
            i++;
            j++;
        } else if (a < b) {
            i++;
        } else {
            j++;
        }
    }
    return true;
}

static bool wide_candidate_union(WideCandidateSet* left,
                                 WideCandidateSet* right,
                                 WideCandidateSet* output) {
    memset(output, 0, sizeof(*output));
    output->bounded = true;
    wide_candidate_normalize(left);
    wide_candidate_normalize(right);
    if (UINT32_MAX - left->count < right->count) return false;
    uint32_t capacity = left->count + right->count;
    if (capacity == 0u) return true;

    output->ids = (uint32_t*)malloc((size_t)capacity * sizeof(uint32_t));
    if (output->ids == NULL) return false;

    uint32_t i = 0u;
    uint32_t j = 0u;
    while (i < left->count || j < right->count) {
        uint32_t value;
        if (j >= right->count ||
            (i < left->count && left->ids[i] < right->ids[j])) {
            value = left->ids[i++];
        } else if (i >= left->count || right->ids[j] < left->ids[i]) {
            value = right->ids[j++];
        } else {
            value = left->ids[i];
            i++;
            j++;
        }
        if (output->count == 0u ||
            output->ids[output->count - 1u] != value) {
            output->ids[output->count++] = value;
        }
    }
    return true;
}

static bool wide_boolean_build_candidates_at(
    Table* table,
    const TableSchema* schema,
    const TinyDBGenericBooleanExpression* expression,
    uint32_t node_index,
    WideCandidateSet* output,
    char* message,
    size_t message_size) {
    memset(output, 0, sizeof(*output));
    if (expression == NULL || node_index >= expression->count) {
        snprintf(message, message_size, "%s", "invalid wide boolean expression");
        return false;
    }

    const TinyDBGenericBooleanNode* node = &expression->nodes[node_index];
    if (node->kind == TINYDB_GENERIC_BOOLEAN_PREDICATE) {
        const TinyDBGenericPredicate* predicate = &node->predicate;
        if (predicate->column_index == 0u &&
            predicate->op == TINYDB_GENERIC_COMPARE_EQ &&
            predicate->value.type == COL_TYPE_INT) {
            output->ids = (uint32_t*)malloc(sizeof(uint32_t));
            if (output->ids == NULL) {
                snprintf(message, message_size, "%s", "unable to allocate primary-key candidate");
                return false;
            }
            output->ids[0] = predicate->value.int_value;
            output->count = 1u;
            output->bounded = true;
            return true;
        }

        if (predicate->column_index != 0u &&
            wide_boolean_index_operator_supported(predicate->op)) {
            GenericSecondaryIndex* index = wide_boolean_find_index(
                table, schema, predicate->column_index);
            if (index != NULL) {
                TinyDBGenericIndexCandidates candidates;
                memset(&candidates, 0, sizeof(candidates));
                if (!tinydb_generic_index_collect_candidates(table,
                                                             schema,
                                                             index,
                                                             predicate,
                                                             &candidates,
                                                             message,
                                                             message_size)) {
                    return false;
                }
                output->ids = candidates.ids;
                output->count = candidates.count;
                output->bounded = true;
                return true;
            }
        }
        return true;
    }

    WideCandidateSet left;
    WideCandidateSet right;
    if (!wide_boolean_build_candidates_at(table,
                                          schema,
                                          expression,
                                          node->left,
                                          &left,
                                          message,
                                          message_size)) {
        return false;
    }
    if (!wide_boolean_build_candidates_at(table,
                                          schema,
                                          expression,
                                          node->right,
                                          &right,
                                          message,
                                          message_size)) {
        wide_candidate_free(&left);
        return false;
    }

    bool success = true;
    if (node->kind == TINYDB_GENERIC_BOOLEAN_AND) {
        if (left.bounded && right.bounded) {
            success = wide_candidate_intersection(&left, &right, output);
        } else if (left.bounded) {
            wide_candidate_move(output, &left);
        } else if (right.bounded) {
            wide_candidate_move(output, &right);
        }
    } else if (node->kind == TINYDB_GENERIC_BOOLEAN_OR) {
        if (left.bounded && right.bounded) {
            success = wide_candidate_union(&left, &right, output);
        }
    } else {
        snprintf(message, message_size, "%s", "unsupported wide boolean node kind");
        success = false;
    }

    wide_candidate_free(&left);
    wide_candidate_free(&right);
    if (!success) {
        wide_candidate_free(output);
        if (message != NULL && message_size > 0u && message[0] == '\0') {
            snprintf(message, message_size, "%s", "unable to combine wide index candidates");
        }
    }
    return success;
}

static void wide_boolean_shape_merge_index_name(WideCandidateShape* output,
                                                const WideCandidateShape* left,
                                                const WideCandidateShape* right) {
    if (left->first_index_name[0] != '\0') {
        snprintf(output->first_index_name,
                 sizeof(output->first_index_name),
                 "%s",
                 left->first_index_name);
    } else if (right->first_index_name[0] != '\0') {
        snprintf(output->first_index_name,
                 sizeof(output->first_index_name),
                 "%s",
                 right->first_index_name);
    }
}

static bool wide_boolean_shape_at(Table* table,
                                  const TableSchema* schema,
                                  const TinyDBGenericBooleanExpression* expression,
                                  uint32_t node_index,
                                  WideCandidateShape* output) {
    memset(output, 0, sizeof(*output));
    if (expression == NULL || node_index >= expression->count) return false;

    const TinyDBGenericBooleanNode* node = &expression->nodes[node_index];
    if (node->kind == TINYDB_GENERIC_BOOLEAN_PREDICATE) {
        const TinyDBGenericPredicate* predicate = &node->predicate;
        if (predicate->column_index == 0u &&
            predicate->op == TINYDB_GENERIC_COMPARE_EQ &&
            predicate->value.type == COL_TYPE_INT) {
            output->bounded = true;
            output->uses_primary = true;
            output->bounded_terms = 1u;
            return true;
        }
        if (predicate->column_index != 0u &&
            wide_boolean_index_operator_supported(predicate->op)) {
            GenericSecondaryIndex* index = wide_boolean_find_index(
                table, schema, predicate->column_index);
            if (index != NULL) {
                output->bounded = true;
                output->uses_secondary = true;
                output->bounded_terms = 1u;
                snprintf(output->first_index_name,
                         sizeof(output->first_index_name),
                         "%s",
                         index->name);
                return true;
            }
        }
        output->residual = true;
        return true;
    }

    WideCandidateShape left;
    WideCandidateShape right;
    if (!wide_boolean_shape_at(table,
                               schema,
                               expression,
                               node->left,
                               &left) ||
        !wide_boolean_shape_at(table,
                               schema,
                               expression,
                               node->right,
                               &right)) {
        return false;
    }

    output->uses_secondary = left.uses_secondary || right.uses_secondary;
    output->uses_primary = left.uses_primary || right.uses_primary;
    output->bounded_terms = left.bounded_terms + right.bounded_terms;
    output->contains_union = left.contains_union || right.contains_union;
    output->residual = left.residual || right.residual;
    wide_boolean_shape_merge_index_name(output, &left, &right);

    if (node->kind == TINYDB_GENERIC_BOOLEAN_AND) {
        output->bounded = left.bounded || right.bounded;
        if (!left.bounded || !right.bounded) output->residual = true;
        output->union_branches = left.union_branches + right.union_branches;
        return true;
    }
    if (node->kind == TINYDB_GENERIC_BOOLEAN_OR) {
        output->contains_union = true;
        output->bounded = left.bounded && right.bounded;
        if (!output->bounded) {
            output->residual = true;
            output->union_branches = 0u;
        } else {
            uint32_t left_branches = left.contains_union
                ? left.union_branches
                : 1u;
            uint32_t right_branches = right.contains_union
                ? right.union_branches
                : 1u;
            output->union_branches = left_branches + right_branches;
        }
        return true;
    }
    return false;
}

static TinyDBGenericSqlStatus wide_boolean_execute_error(
    TinyDBGenericSqlResult* result,
    const char* message) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_KEY_NOT_FOUND;
    result->executed = false;
    snprintf(result->message,
             sizeof(result->message),
             "%s",
             message != NULL ? message : "wide boolean index execution failed");
    return result->status;
}

static TinyDBGenericSqlStatus wide_boolean_success(
    TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
}

static void wide_boolean_print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static void wide_boolean_print_row(const TinyDBValue* values,
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

static TinyDBGenericSqlStatus wide_boolean_execute_candidates(
    Table* table,
    const WideBooleanSelect* select,
    WideCandidateSet* candidates,
    TinyDBGenericSqlResult* result) {
    char message[TINYDB_RECORD_MESSAGE_MAX] = {0};
    if (!tinydb_record_payload_schema_supported(select->schema,
                                                message,
                                                sizeof(message))) {
        return wide_boolean_execute_error(result, message);
    }

    wide_candidate_normalize(candidates);
    uint32_t matched = 0u;
    uint32_t emitted = 0u;
    for (uint32_t i = 0u; i < candidates->count; i++) {
        TinyDBRecordPayload payload;
        message[0] = '\0';
        if (!tinydb_record_payload_find(table,
                                        select->schema,
                                        candidates->ids[i],
                                        &payload,
                                        message,
                                        sizeof(message))) {
            continue;
        }

        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t value_count = 0u;
        if (!tinydb_record_payload_decode_values(select->schema,
                                                 &payload,
                                                 values,
                                                 MAX_COLUMNS_PER_TABLE,
                                                 &value_count,
                                                 message,
                                                 sizeof(message)) ||
            value_count != select->schema->num_columns) {
            return wide_boolean_execute_error(
                result,
                message[0] != '\0'
                    ? message
                    : "unable to decode wide boolean index candidate");
        }
        if (!tinydb_generic_boolean_matches(&select->expression,
                                            values,
                                            value_count)) {
            continue;
        }

        matched++;
        if (select->projection_kind == WIDE_BOOLEAN_PROJECTION_COUNT ||
            matched <= select->offset ||
            (select->has_limit && emitted >= select->limit)) {
            continue;
        }

        if (select->projection_kind == WIDE_BOOLEAN_PROJECTION_COLUMN) {
            wide_boolean_print_value(&values[select->projection_column_index]);
        } else {
            wide_boolean_print_row(values, value_count);
        }
        emitted++;
    }

    if (select->projection_kind == WIDE_BOOLEAN_PROJECTION_COUNT) {
        uint32_t count = matched;
        if (select->offset > 0u) count = 0u;
        if (select->has_limit && select->limit == 0u) count = 0u;
        printf("%u\n", count);
    }
    return wide_boolean_success(result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;

    WideBooleanSelect select;
    if (!wide_boolean_parse_select(table, sql, &select)) {
        return tinydb_generic_sql_try_execute_wide_atomic_base(table,
                                                               sql,
                                                               output);
    }

    char message[TINYDB_RECORD_MESSAGE_MAX] = {0};
    WideCandidateSet candidates;
    if (!wide_boolean_build_candidates_at(table,
                                          select.schema,
                                          &select.expression,
                                          select.expression.root,
                                          &candidates,
                                          message,
                                          sizeof(message))) {
        return wide_boolean_execute_error(
            output,
            message[0] != '\0'
                ? message
                : "unable to derive wide boolean index candidates");
    }
    if (!candidates.bounded) {
        wide_candidate_free(&candidates);
        return tinydb_generic_sql_try_execute_wide_atomic_base(table,
                                                               sql,
                                                               output);
    }

    TinyDBGenericSqlStatus status =
        wide_boolean_execute_candidates(table, &select, &candidates, output);
    wide_candidate_free(&candidates);
    return status;
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    WideBooleanSelect select;
    if (!wide_boolean_parse_select(table, sql, &select) ||
        !wide_boolean_expression_contains_or(&select.expression)) {
        return tinydb_generic_sql_build_select_plan_wide_boolean_base(
            table, sql, plan, result);
    }

    WideCandidateShape shape;
    if (!wide_boolean_shape_at(table,
                               select.schema,
                               &select.expression,
                               select.expression.root,
                               &shape) ||
        !shape.bounded) {
        return tinydb_generic_sql_build_select_plan_wide_boolean_base(
            table, sql, plan, result);
    }

    TinyDBGenericSqlStatus status =
        tinydb_generic_sql_build_select_plan_wide_boolean_base(
            table, sql, plan, result);
    if (status != TINYDB_GENERIC_SQL_SUCCESS || plan == NULL ||
        !plan->applicable) {
        return status;
    }

    const TinyDBGenericBooleanNode* root =
        &select.expression.nodes[select.expression.root];
    if (root->kind == TINYDB_GENERIC_BOOLEAN_OR) {
        plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_UNION;
    } else if (root->kind == TINYDB_GENERIC_BOOLEAN_AND) {
        WideCandidateShape left;
        WideCandidateShape right;
        if (wide_boolean_shape_at(table,
                                  select.schema,
                                  &select.expression,
                                  root->left,
                                  &left) &&
            wide_boolean_shape_at(table,
                                  select.schema,
                                  &select.expression,
                                  root->right,
                                  &right) &&
            left.bounded && right.bounded) {
            plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_INTERSECTION;
        } else {
            plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_UNION;
        }
    }

    plan->has_cost_estimate = false;
    plan->estimated_rows = 0u;
    plan->estimated_table_rows = 0u;
    plan->estimated_cost = 0u;
    plan->estimated_scan_cost = 0u;
    plan->index_term_count = shape.bounded_terms;
    plan->index_branch_count = shape.union_branches > 0u
        ? shape.union_branches
        : shape.bounded_terms;
    if (shape.uses_secondary) {
        snprintf(plan->index_name,
                 sizeof(plan->index_name),
                 "%s",
                 shape.contains_union ? "multiple" : shape.first_index_name);
    }
    return status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_wide_boolean_base(plan);
}
