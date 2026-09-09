#include "generic_boolean.h"
#include "generic_index_candidates.h"
#include "generic_index_cost.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define WIDE_BOOLEAN_COST_MIN_TABLE_ROWS 32u
#define WIDE_BOOLEAN_COST_SCAN_MARKER "wide-boolean"

typedef struct {
    TableSchema* schema;
    TinyDBGenericBooleanExpression expression;
    bool has_limit_or_offset;
} WideBooleanCostSelect;

typedef struct {
    bool bounded;
    bool has_table_rows;
    bool contains_or;
    uint32_t estimated_rows;
    uint32_t table_rows;
    uint32_t bounded_terms;
    uint32_t union_branches;
    uint64_t source_cost;
} WideBooleanCostNode;

typedef struct {
    bool valid;
    uint32_t estimated_rows;
    uint32_t table_rows;
    uint32_t bounded_terms;
    uint32_t union_branches;
    uint64_t candidate_cost;
    uint64_t scan_cost;
} WideBooleanCostEstimate;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_boolean_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_atomic_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_wide_boolean_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_wide_boolean_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_wide_boolean_index_base(
    const TinyDBGenericSelectPlan* plan);

static int wide_boolean_cost_ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool wide_boolean_cost_ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (wide_boolean_cost_ci_char(*left) !=
            wide_boolean_cost_ci_char(*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool wide_boolean_cost_legacy_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           wide_boolean_cost_ci_equal(schema->columns[0].name, "id") &&
           wide_boolean_cost_ci_equal(schema->columns[1].name, "username") &&
           wide_boolean_cost_ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static TableSchema* wide_boolean_cost_find_schema(Table* table,
                                                  const char* table_name) {
    if (table == NULL || table_name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (wide_boolean_cost_ci_equal(table->catalog.schemas[i].name,
                                       table_name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static GenericSecondaryIndex* wide_boolean_cost_find_index(
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
            wide_boolean_cost_ci_equal(index->table_name, schema->name) &&
            wide_boolean_cost_ci_equal(index->column_name,
                                       schema->columns[column_index].name)) {
            return index;
        }
    }
    return NULL;
}

static bool wide_boolean_cost_operator_supported(TinyDBGenericCompareOp op) {
    return op == TINYDB_GENERIC_COMPARE_EQ ||
           op == TINYDB_GENERIC_COMPARE_GT ||
           op == TINYDB_GENERIC_COMPARE_GTE ||
           op == TINYDB_GENERIC_COMPARE_LT ||
           op == TINYDB_GENERIC_COMPARE_LTE;
}

static bool wide_boolean_cost_skip_projection(TinyDBGenericParser* parser) {
    if (tinydb_generic_consume_char(parser, '*')) return true;

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        return true;
    }
    *parser = backup;

    char ignored[MAX_NAME_SIZE];
    return tinydb_generic_parse_identifier(parser, ignored, sizeof(ignored));
}

static bool wide_boolean_cost_has_or(
    const TinyDBGenericBooleanExpression* expression) {
    if (expression == NULL) return false;
    for (uint32_t i = 0u; i < expression->count; i++) {
        if (expression->nodes[i].kind == TINYDB_GENERIC_BOOLEAN_OR) return true;
    }
    return false;
}

static bool wide_boolean_cost_parse_select(Table* table,
                                           const char* sql,
                                           WideBooleanCostSelect* select) {
    if (table == NULL || sql == NULL || select == NULL) return false;
    memset(select, 0, sizeof(*select));

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select") ||
        !wide_boolean_cost_skip_projection(&parser) ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return false;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }
    select->schema = wide_boolean_cost_find_schema(table, table_name);
    if (select->schema == NULL || select->schema->row_size <= ROW_SIZE ||
        wide_boolean_cost_legacy_shape(select->schema) ||
        !tinydb_generic_consume_word(&parser, "where") ||
        !tinydb_generic_parse_boolean_expression(&parser,
                                                 select->schema,
                                                 &select->expression)) {
        return false;
    }

    if (tinydb_generic_consume_word(&parser, "limit")) {
        uint32_t ignored = 0u;
        if (!tinydb_generic_parse_uint32(&parser, &ignored)) return false;
        select->has_limit_or_offset = true;
        if (tinydb_generic_consume_word(&parser, "offset") &&
            !tinydb_generic_parse_uint32(&parser, &ignored)) {
            return false;
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        uint32_t ignored = 0u;
        if (!tinydb_generic_parse_uint32(&parser, &ignored)) return false;
        select->has_limit_or_offset = true;
    }
    if (!tinydb_generic_consume_end(&parser)) return false;

    return select->expression.saw_grouping ||
           wide_boolean_cost_has_or(&select->expression);
}

static bool wide_boolean_cost_merge_table_rows(
    const WideBooleanCostNode* left,
    const WideBooleanCostNode* right,
    WideBooleanCostNode* output) {
    if (left->has_table_rows && right->has_table_rows &&
        left->table_rows != right->table_rows) {
        return false;
    }
    output->has_table_rows = left->has_table_rows || right->has_table_rows;
    output->table_rows = left->has_table_rows
        ? left->table_rows
        : right->table_rows;
    return true;
}

static bool wide_boolean_cost_estimate_node(
    Table* table,
    const TableSchema* schema,
    const TinyDBGenericBooleanExpression* expression,
    uint32_t node_index,
    WideBooleanCostNode* output) {
    memset(output, 0, sizeof(*output));
    if (expression == NULL || node_index >= expression->count) return false;

    const TinyDBGenericBooleanNode* node = &expression->nodes[node_index];
    if (node->kind == TINYDB_GENERIC_BOOLEAN_PREDICATE) {
        const TinyDBGenericPredicate* predicate = &node->predicate;
        if (predicate->column_index == 0u &&
            predicate->op == TINYDB_GENERIC_COMPARE_EQ &&
            predicate->value.type == COL_TYPE_INT) {
            output->bounded = true;
            output->estimated_rows = 1u;
            output->bounded_terms = 1u;
            return true;
        }
        if (predicate->column_index == 0u ||
            !wide_boolean_cost_operator_supported(predicate->op)) {
            return true;
        }

        GenericSecondaryIndex* index = wide_boolean_cost_find_index(
            table, schema, predicate->column_index);
        if (index == NULL) return true;

        TinyDBGenericIndexEstimate estimate;
        char message[TINYDB_RECORD_MESSAGE_MAX] = {0};
        if (!tinydb_generic_index_estimate_candidates(table,
                                                      schema,
                                                      index,
                                                      predicate,
                                                      &estimate,
                                                      message,
                                                      sizeof(message))) {
            return false;
        }
        output->bounded = true;
        output->has_table_rows = true;
        output->estimated_rows = estimate.candidate_count;
        output->table_rows = estimate.total_count;
        output->bounded_terms = 1u;
        output->source_cost =
            (uint64_t)estimate.candidate_count * TINYDB_GENERIC_COST_INDEX_ENTRY;
        return true;
    }

    WideBooleanCostNode left;
    WideBooleanCostNode right;
    if (!wide_boolean_cost_estimate_node(table,
                                         schema,
                                         expression,
                                         node->left,
                                         &left) ||
        !wide_boolean_cost_estimate_node(table,
                                         schema,
                                         expression,
                                         node->right,
                                         &right) ||
        !wide_boolean_cost_merge_table_rows(&left, &right, output)) {
        return false;
    }

    output->source_cost = left.source_cost + right.source_cost;
    output->bounded_terms = left.bounded_terms + right.bounded_terms;
    output->contains_or = left.contains_or || right.contains_or;

    if (node->kind == TINYDB_GENERIC_BOOLEAN_AND) {
        output->bounded = left.bounded || right.bounded;
        if (left.bounded && right.bounded) {
            output->estimated_rows = left.estimated_rows < right.estimated_rows
                ? left.estimated_rows
                : right.estimated_rows;
        } else if (left.bounded) {
            output->estimated_rows = left.estimated_rows;
        } else if (right.bounded) {
            output->estimated_rows = right.estimated_rows;
        }
        output->union_branches = left.union_branches + right.union_branches;
        return true;
    }

    if (node->kind == TINYDB_GENERIC_BOOLEAN_OR) {
        output->contains_or = true;
        output->bounded = left.bounded && right.bounded;
        if (!output->bounded) return true;

        uint64_t rows = (uint64_t)left.estimated_rows + right.estimated_rows;
        if (output->has_table_rows && rows > output->table_rows) {
            rows = output->table_rows;
        }
        if (rows > UINT32_MAX) return false;
        output->estimated_rows = (uint32_t)rows;

        uint32_t left_branches = left.contains_or
            ? left.union_branches
            : 1u;
        uint32_t right_branches = right.contains_or
            ? right.union_branches
            : 1u;
        output->union_branches = left_branches + right_branches;
        return true;
    }
    return false;
}

static bool wide_boolean_cost_estimate(
    Table* table,
    const WideBooleanCostSelect* select,
    WideBooleanCostEstimate* estimate) {
    memset(estimate, 0, sizeof(*estimate));
    if (select == NULL || select->schema == NULL ||
        select->has_limit_or_offset) {
        return false;
    }

    WideBooleanCostNode root;
    if (!wide_boolean_cost_estimate_node(table,
                                         select->schema,
                                         &select->expression,
                                         select->expression.root,
                                         &root) ||
        !root.bounded || !root.has_table_rows || root.table_rows == 0u) {
        return false;
    }

    uint32_t rows = root.estimated_rows;
    if (rows > root.table_rows) rows = root.table_rows;
    estimate->valid = true;
    estimate->estimated_rows = rows;
    estimate->table_rows = root.table_rows;
    estimate->bounded_terms = root.bounded_terms;
    estimate->union_branches = root.union_branches;
    estimate->candidate_cost = root.source_cost +
        (uint64_t)rows * TINYDB_GENERIC_COST_RANDOM_FETCH;
    estimate->scan_cost = tinydb_generic_scan_cost(root.table_rows);
    return true;
}

static bool wide_boolean_cost_prefers_scan(
    const WideBooleanCostEstimate* estimate) {
    return estimate != NULL && estimate->valid &&
           estimate->table_rows >= WIDE_BOOLEAN_COST_MIN_TABLE_ROWS &&
           estimate->candidate_cost > estimate->scan_cost;
}

static void wide_boolean_cost_annotate_plan(
    TinyDBGenericSelectPlan* plan,
    const WideBooleanCostEstimate* estimate) {
    if (plan == NULL || estimate == NULL || !estimate->valid) return;
    plan->has_cost_estimate = true;
    plan->estimated_rows = estimate->estimated_rows;
    plan->estimated_table_rows = estimate->table_rows;
    plan->estimated_cost = estimate->candidate_cost;
    plan->estimated_scan_cost = estimate->scan_cost;
    plan->index_term_count = estimate->bounded_terms;
    if (estimate->union_branches > 0u) {
        plan->index_branch_count = estimate->union_branches;
    }
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    WideBooleanCostSelect select;
    WideBooleanCostEstimate estimate;
    if (wide_boolean_cost_parse_select(table, sql, &select) &&
        wide_boolean_cost_estimate(table, &select, &estimate) &&
        wide_boolean_cost_prefers_scan(&estimate)) {
        return tinydb_generic_sql_try_execute_wide_atomic_base(table,
                                                               sql,
                                                               result);
    }
    return tinydb_generic_sql_try_execute_wide_boolean_index_base(table,
                                                                  sql,
                                                                  result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    WideBooleanCostSelect select;
    WideBooleanCostEstimate estimate;
    bool estimated = wide_boolean_cost_parse_select(table, sql, &select) &&
                     wide_boolean_cost_estimate(table, &select, &estimate);

    if (estimated && wide_boolean_cost_prefers_scan(&estimate)) {
        TinyDBGenericSqlStatus status =
            tinydb_generic_sql_build_select_plan_wide_boolean_base(
                table, sql, plan, result);
        if (status == TINYDB_GENERIC_SQL_SUCCESS && plan != NULL &&
            plan->applicable) {
            wide_boolean_cost_annotate_plan(plan, &estimate);
            snprintf(plan->index_name,
                     sizeof(plan->index_name),
                     "%s",
                     WIDE_BOOLEAN_COST_SCAN_MARKER);
        }
        return status;
    }

    TinyDBGenericSqlStatus status =
        tinydb_generic_sql_build_select_plan_wide_boolean_index_base(
            table, sql, plan, result);
    if (status == TINYDB_GENERIC_SQL_SUCCESS && plan != NULL &&
        plan->applicable && estimated) {
        wide_boolean_cost_annotate_plan(plan, &estimate);
    }
    return status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    bool wide_scan_choice =
        plan != NULL && plan->applicable &&
        plan->kind == TINYDB_GENERIC_PLAN_FULL_SCAN &&
        plan->has_cost_estimate &&
        wide_boolean_cost_ci_equal(plan->index_name,
                                   WIDE_BOOLEAN_COST_SCAN_MARKER);

    if (!wide_scan_choice) {
        tinydb_generic_sql_print_plan_wide_boolean_index_base(plan);
        return;
    }

    TinyDBGenericSelectPlan base_view = *plan;
    base_view.has_cost_estimate = false;
    base_view.estimated_rows = 0u;
    base_view.estimated_table_rows = 0u;
    base_view.estimated_cost = 0u;
    base_view.estimated_scan_cost = 0u;
    base_view.index_name[0] = '\0';
    tinydb_generic_sql_print_plan_wide_boolean_index_base(&base_view);

    printf("  ESTIMATED ROWS: %u / %u\n",
           plan->estimated_rows,
           plan->estimated_table_rows);
    printf("  ESTIMATED COST: %llu (scan %llu)\n",
           (unsigned long long)plan->estimated_cost,
           (unsigned long long)plan->estimated_scan_cost);
    printf("  COST CHOICE: table scan cheaper than wide boolean candidate plan\n");
}
