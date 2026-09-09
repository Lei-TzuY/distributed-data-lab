#include "generic_index_candidates.h"
#include "generic_index_cost.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define SINGLE_COST_MIN_TABLE_ROWS 32u

typedef struct {
    TableSchema* schema;
    GenericSecondaryIndex* index;
    TinyDBGenericPredicate predicate;
    bool has_limit_or_offset;
} SingleCostSelect;

typedef struct {
    bool valid;
    uint32_t candidate_rows;
    uint32_t table_rows;
    uint64_t index_cost;
    uint64_t scan_cost;
} SingleCostEstimate;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_range_index_uncosted_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_range_index_uncosted_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
void tinydb_generic_sql_print_plan_range_index_uncosted_base(
    const TinyDBGenericSelectPlan* plan);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_range_select(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_range(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
void tinydb_generic_sql_print_plan_range(const TinyDBGenericSelectPlan* plan);

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

static bool skip_projection(TinyDBGenericParser* parser) {
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

static bool plan_is_single_secondary_index(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable || plan->index_name[0] == '\0') return false;
    return plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_LOOKUP ||
           plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RANGE;
}

static bool parse_single_cost_select(Table* table,
                                     const char* sql,
                                     const TinyDBGenericSelectPlan* plan,
                                     SingleCostSelect* select) {
    memset(select, 0, sizeof(*select));
    if (!plan_is_single_secondary_index(plan)) return false;

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select") ||
        !skip_projection(&parser) ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return false;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name)) ||
        !ci_equal(table_name, plan->table_name)) {
        return false;
    }
    select->schema = find_schema(table, table_name);
    if (select->schema == NULL ||
        !tinydb_generic_consume_word(&parser, "where") ||
        !tinydb_generic_parse_predicate(&parser,
                                        select->schema,
                                        &select->predicate)) {
        return false;
    }
    if (select->predicate.op == TINYDB_GENERIC_COMPARE_LIKE ||
        select->predicate.column_index == 0) {
        return false;
    }
    if (tinydb_generic_consume_word(&parser, "and") ||
        tinydb_generic_consume_word(&parser, "or")) {
        return false;
    }

    if (tinydb_generic_consume_word(&parser, "limit")) {
        uint32_t ignored = 0;
        if (!tinydb_generic_parse_uint32(&parser, &ignored)) return false;
        select->has_limit_or_offset = true;
        if (tinydb_generic_consume_word(&parser, "offset")) {
            if (!tinydb_generic_parse_uint32(&parser, &ignored)) return false;
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        uint32_t ignored = 0;
        if (!tinydb_generic_parse_uint32(&parser, &ignored)) return false;
        select->has_limit_or_offset = true;
    }
    if (!tinydb_generic_consume_end(&parser)) return false;

    select->index = table_find_index_by_name(table, plan->index_name);
    return select->index != NULL;
}

static bool estimate_single(Table* table,
                            const SingleCostSelect* select,
                            SingleCostEstimate* estimate) {
    memset(estimate, 0, sizeof(*estimate));
    if (select == NULL || select->schema == NULL || select->index == NULL ||
        select->has_limit_or_offset) {
        return false;
    }

    TinyDBGenericIndexEstimate source;
    char message[TINYDB_RECORD_MESSAGE_MAX] = {0};
    if (!tinydb_generic_index_estimate_candidates(table,
                                                  select->schema,
                                                  select->index,
                                                  &select->predicate,
                                                  &source,
                                                  message,
                                                  sizeof(message))) {
        return false;
    }

    estimate->candidate_rows = source.candidate_count;
    estimate->table_rows = source.total_count;
    estimate->index_cost = tinydb_generic_anchor_cost(source.candidate_count);
    estimate->scan_cost = tinydb_generic_scan_cost(source.total_count);
    estimate->valid = true;
    return true;
}

static bool cost_prefers_scan(const SingleCostEstimate* estimate) {
    return estimate != NULL && estimate->valid &&
           estimate->table_rows >= SINGLE_COST_MIN_TABLE_ROWS &&
           estimate->index_cost > estimate->scan_cost;
}

static void annotate_cost(TinyDBGenericSelectPlan* plan,
                          const SingleCostEstimate* estimate) {
    if (plan == NULL || estimate == NULL || !estimate->valid) return;
    plan->has_cost_estimate = true;
    plan->estimated_rows = estimate->candidate_rows;
    plan->estimated_table_rows = estimate->table_rows;
    plan->estimated_cost = estimate->index_cost;
    plan->estimated_scan_cost = estimate->scan_cost;
}

static bool uncosted_single_plan(Table* table,
                                 const char* sql,
                                 TinyDBGenericSelectPlan* plan) {
    TinyDBGenericSqlResult result;
    memset(&result, 0, sizeof(result));
    memset(plan, 0, sizeof(*plan));
    return tinydb_generic_sql_build_select_plan_range_index_uncosted_base(
               table, sql, plan, &result) == TINYDB_GENERIC_SQL_SUCCESS &&
           plan_is_single_secondary_index(plan);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_range_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSelectPlan plan;
    SingleCostSelect select;
    SingleCostEstimate estimate;
    if (table != NULL && sql != NULL &&
        uncosted_single_plan(table, sql, &plan) &&
        parse_single_cost_select(table, sql, &plan, &select) &&
        estimate_single(table, &select, &estimate) &&
        cost_prefers_scan(&estimate)) {
        TinyDBGenericSqlStatus status =
            tinydb_generic_sql_try_execute_range_select(table, sql, result);
        if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;
    }
    return tinydb_generic_sql_try_execute_range_index_uncosted_base(
        table, sql, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_range_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSelectPlan single_plan;
    if (!uncosted_single_plan(table, sql, &single_plan)) {
        return tinydb_generic_sql_build_select_plan_range_index_uncosted_base(
            table, sql, plan, result);
    }

    SingleCostSelect select;
    SingleCostEstimate estimate;
    bool estimated = parse_single_cost_select(table, sql, &single_plan, &select) &&
                     estimate_single(table, &select, &estimate);
    if (estimated && cost_prefers_scan(&estimate)) {
        TinyDBGenericSqlStatus status = tinydb_generic_sql_build_select_plan_range(
            table, sql, plan, result);
        if (status == TINYDB_GENERIC_SQL_SUCCESS &&
            plan != NULL && plan->applicable) {
            snprintf(plan->index_name,
                     sizeof(plan->index_name),
                     "%s",
                     single_plan.index_name);
            annotate_cost(plan, &estimate);
            return status;
        }
    }

    TinyDBGenericSqlStatus status =
        tinydb_generic_sql_build_select_plan_range_index_uncosted_base(
            table, sql, plan, result);
    if (status == TINYDB_GENERIC_SQL_SUCCESS &&
        plan != NULL && plan->applicable && estimated) {
        annotate_cost(plan, &estimate);
    }
    return status;
}

static void print_cost(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->has_cost_estimate) return;
    printf("  ESTIMATED ROWS: %u / %u\n",
           plan->estimated_rows,
           plan->estimated_table_rows);
    printf("  ESTIMATED COST: %llu (scan %llu)\n",
           (unsigned long long)plan->estimated_cost,
           (unsigned long long)plan->estimated_scan_cost);
}

void tinydb_generic_sql_print_plan_range_index_base(
    const TinyDBGenericSelectPlan* plan) {
    if (plan != NULL && plan->applicable &&
        plan->kind == TINYDB_GENERIC_PLAN_FULL_SCAN &&
        plan->has_cost_estimate &&
        plan->index_name[0] != '\0') {
        tinydb_generic_sql_print_plan_range(plan);
        print_cost(plan);
        printf("  COST CHOICE: table scan cheaper than single secondary index\n");
        return;
    }

    tinydb_generic_sql_print_plan_range_index_uncosted_base(plan);
    if (plan != NULL && plan->applicable && plan_is_single_secondary_index(plan)) {
        print_cost(plan);
    }
}
