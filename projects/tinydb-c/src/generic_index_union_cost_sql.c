#include "generic_index_candidates.h"
#include "generic_index_cost.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OR_COST_MAX_GROUPS MAX_COLUMNS_PER_TABLE
#define OR_COST_MAX_TERMS MAX_COLUMNS_PER_TABLE
#define OR_COST_MIN_TABLE_ROWS 32u

typedef struct {
    TinyDBGenericPredicate terms[OR_COST_MAX_TERMS];
    uint32_t count;
    bool primary_key_anchor;
    uint32_t anchor_predicate_index;
    GenericSecondaryIndex* index;
} OrCostGroup;

typedef struct {
    TableSchema* schema;
    OrCostGroup groups[OR_COST_MAX_GROUPS];
    uint32_t group_count;
    bool has_limit_or_offset;
} OrCostSelect;

typedef struct {
    bool valid;
    uint32_t estimated_rows;
    uint32_t table_rows;
    uint64_t union_cost;
    uint64_t scan_cost;
} OrCostEstimate;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_union_uncosted_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_union_uncosted_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
void tinydb_generic_sql_print_plan_union_uncosted_base(
    const TinyDBGenericSelectPlan* plan);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_or_scan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_or_scan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
void tinydb_generic_sql_print_plan_or_scan_base(
    const TinyDBGenericSelectPlan* plan);

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

static GenericSecondaryIndex* find_single_column_index(
    Table* table,
    const TableSchema* schema,
    uint32_t column_index) {
    if (table == NULL || schema == NULL ||
        column_index == 0 || column_index >= schema->num_columns) {
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

static bool choose_group_anchor(Table* table,
                                TableSchema* schema,
                                OrCostGroup* group) {
    for (uint32_t i = 0; i < group->count; i++) {
        TinyDBGenericPredicate* predicate = &group->terms[i];
        if (predicate->column_index == 0 &&
            predicate->op == TINYDB_GENERIC_COMPARE_EQ) {
            group->primary_key_anchor = true;
            group->anchor_predicate_index = i;
            return true;
        }
    }

    uint32_t best_score = 0;
    GenericSecondaryIndex* best_index = NULL;
    uint32_t best_predicate = 0;
    for (uint32_t i = 0; i < group->count; i++) {
        TinyDBGenericPredicate* predicate = &group->terms[i];
        if (predicate->op == TINYDB_GENERIC_COMPARE_LIKE) continue;
        GenericSecondaryIndex* index = find_single_column_index(
            table, schema, predicate->column_index);
        if (index == NULL) continue;
        uint32_t score = predicate->op == TINYDB_GENERIC_COMPARE_EQ ? 2u : 1u;
        if (score > best_score) {
            best_score = score;
            best_index = index;
            best_predicate = i;
        }
    }
    if (best_index == NULL) return false;
    group->index = best_index;
    group->anchor_predicate_index = best_predicate;
    return true;
}

static bool parse_or_cost_select(Table* table,
                                 const char* sql,
                                 OrCostSelect* select) {
    memset(select, 0, sizeof(*select));
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
                                         sizeof(table_name))) {
        return false;
    }
    select->schema = find_schema(table, table_name);
    if (select->schema == NULL ||
        !tinydb_generic_consume_word(&parser, "where")) {
        return false;
    }

    select->group_count = 1;
    bool saw_or = false;
    for (;;) {
        OrCostGroup* group = &select->groups[select->group_count - 1u];
        if (group->count >= OR_COST_MAX_TERMS ||
            !tinydb_generic_parse_predicate(&parser,
                                            select->schema,
                                            &group->terms[group->count])) {
            return false;
        }
        group->count++;

        if (tinydb_generic_consume_word(&parser, "and")) continue;
        if (tinydb_generic_consume_word(&parser, "or")) {
            saw_or = true;
            if (select->group_count >= OR_COST_MAX_GROUPS) return false;
            select->group_count++;
            continue;
        }
        break;
    }
    if (!saw_or) return false;

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

    for (uint32_t i = 0; i < select->group_count; i++) {
        if (!choose_group_anchor(table, select->schema, &select->groups[i])) {
            return false;
        }
    }
    return true;
}

static bool estimate_or_union(Table* table,
                              const OrCostSelect* select,
                              OrCostEstimate* estimate) {
    memset(estimate, 0, sizeof(*estimate));
    if (select == NULL || select->schema == NULL ||
        select->group_count == 0 || select->has_limit_or_offset) {
        return false;
    }

    uint64_t candidate_sum = 0;
    uint64_t index_entry_cost = 0;
    uint32_t table_rows = 0;
    bool has_secondary_estimate = false;

    for (uint32_t i = 0; i < select->group_count; i++) {
        const OrCostGroup* group = &select->groups[i];
        const TinyDBGenericPredicate* anchor =
            &group->terms[group->anchor_predicate_index];
        if (group->primary_key_anchor) {
            candidate_sum += 1u;
            continue;
        }

        TinyDBGenericIndexEstimate branch;
        char message[TINYDB_RECORD_MESSAGE_MAX] = {0};
        if (!tinydb_generic_index_estimate_candidates(table,
                                                      select->schema,
                                                      group->index,
                                                      anchor,
                                                      &branch,
                                                      message,
                                                      sizeof(message))) {
            return false;
        }
        if (!has_secondary_estimate) {
            table_rows = branch.total_count;
            has_secondary_estimate = true;
        } else if (branch.total_count != table_rows) {
            return false;
        }
        candidate_sum += branch.candidate_count;
        index_entry_cost +=
            (uint64_t)branch.candidate_count * TINYDB_GENERIC_COST_INDEX_ENTRY;
    }

    if (!has_secondary_estimate || table_rows == 0) return false;
    uint64_t union_rows = candidate_sum;
    if (union_rows > table_rows) union_rows = table_rows;
    if (union_rows > UINT32_MAX) return false;

    estimate->estimated_rows = (uint32_t)union_rows;
    estimate->table_rows = table_rows;
    estimate->union_cost = index_entry_cost +
        union_rows * TINYDB_GENERIC_COST_RANDOM_FETCH;
    estimate->scan_cost = tinydb_generic_scan_cost(table_rows);
    estimate->valid = true;
    return true;
}

static void annotate_cost(TinyDBGenericSelectPlan* plan,
                          const OrCostEstimate* estimate) {
    if (plan == NULL || estimate == NULL || !estimate->valid) return;
    plan->has_cost_estimate = true;
    plan->estimated_rows = estimate->estimated_rows;
    plan->estimated_table_rows = estimate->table_rows;
    plan->estimated_cost = estimate->union_cost;
    plan->estimated_scan_cost = estimate->scan_cost;
}

static bool cost_prefers_scan(const OrCostEstimate* estimate) {
    return estimate != NULL && estimate->valid &&
           estimate->table_rows >= OR_COST_MIN_TABLE_ROWS &&
           estimate->union_cost > estimate->scan_cost;
}

static bool uncosted_union_plan(Table* table,
                                const char* sql,
                                TinyDBGenericSelectPlan* plan) {
    TinyDBGenericSqlResult result;
    memset(&result, 0, sizeof(result));
    memset(plan, 0, sizeof(*plan));
    return tinydb_generic_sql_build_select_plan_union_uncosted_base(
               table, sql, plan, &result) == TINYDB_GENERIC_SQL_SUCCESS &&
           plan->applicable &&
           plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_UNION;
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_union_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSelectPlan union_plan;
    OrCostSelect select;
    OrCostEstimate estimate;
    if (table != NULL && sql != NULL &&
        uncosted_union_plan(table, sql, &union_plan) &&
        parse_or_cost_select(table, sql, &select) &&
        estimate_or_union(table, &select, &estimate) &&
        cost_prefers_scan(&estimate)) {
        TinyDBGenericSqlStatus status =
            tinydb_generic_sql_try_execute_or_scan_base(table, sql, result);
        if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) return status;
    }
    return tinydb_generic_sql_try_execute_union_uncosted_base(table, sql, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_union_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSelectPlan union_plan;
    if (!uncosted_union_plan(table, sql, &union_plan)) {
        return tinydb_generic_sql_build_select_plan_union_uncosted_base(
            table, sql, plan, result);
    }

    OrCostSelect select;
    OrCostEstimate estimate;
    bool estimated = parse_or_cost_select(table, sql, &select) &&
                     estimate_or_union(table, &select, &estimate);
    if (estimated && cost_prefers_scan(&estimate)) {
        TinyDBGenericSqlStatus status =
            tinydb_generic_sql_build_select_plan_or_scan_base(
                table, sql, plan, result);
        if (status == TINYDB_GENERIC_SQL_SUCCESS &&
            plan != NULL && plan->applicable) {
            plan->index_branch_count = select.group_count;
            snprintf(plan->index_name, sizeof(plan->index_name), "%s", "multiple");
            annotate_cost(plan, &estimate);
            return status;
        }
    }

    TinyDBGenericSqlStatus status =
        tinydb_generic_sql_build_select_plan_union_uncosted_base(
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

void tinydb_generic_sql_print_plan_union_base(
    const TinyDBGenericSelectPlan* plan) {
    if (plan != NULL && plan->applicable &&
        plan->kind == TINYDB_GENERIC_PLAN_FULL_SCAN &&
        plan->has_cost_estimate &&
        plan->index_branch_count > 0 &&
        ci_equal(plan->index_name, "multiple")) {
        tinydb_generic_sql_print_plan_or_scan_base(plan);
        print_cost(plan);
        printf("  COST CHOICE: table scan cheaper than OR index union\n");
        return;
    }

    tinydb_generic_sql_print_plan_union_uncosted_base(plan);
    if (plan != NULL && plan->applicable &&
        plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_UNION) {
        print_cost(plan);
    }
}
