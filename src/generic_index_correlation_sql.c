#include "generic_index_candidates.h"
#include "generic_index_correlation.h"
#include "generic_index_cost.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    TableSchema* schema;
    TinyDBGenericPredicate first_predicate;
    TinyDBGenericPredicate second_predicate;
    GenericSecondaryIndex* first_index;
    GenericSecondaryIndex* second_index;
    uint32_t first_rows;
    uint32_t second_rows;
    uint32_t joint_rows;
    uint32_t table_rows;
    uint64_t correlation_cost;
    uint64_t anchor_cost;
    uint64_t scan_cost;
    bool downgrade;
} CorrelationDecision;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_intersection_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_intersection_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
void tinydb_generic_sql_print_plan_intersection_base(
    const TinyDBGenericSelectPlan* plan);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_intersection_forced(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_intersection_forced(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_single_anchor_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_single_anchor_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

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

static GenericSecondaryIndex* find_index_for_column(Table* table,
                                                     const TableSchema* schema,
                                                     uint32_t column_index) {
    if (table == NULL || schema == NULL || column_index == 0 ||
        column_index >= schema->num_columns) {
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

static bool consume_projection(TinyDBGenericParser* parser) {
    if (tinydb_generic_consume_char(parser, '*')) return true;

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    return tinydb_generic_parse_identifier(parser, column, sizeof(column));
}

static bool consume_tail(TinyDBGenericParser* parser) {
    uint32_t ignored = 0;
    if (tinydb_generic_consume_word(parser, "limit")) {
        if (!tinydb_generic_parse_uint32(parser, &ignored)) return false;
        if (tinydb_generic_consume_word(parser, "offset") &&
            !tinydb_generic_parse_uint32(parser, &ignored)) {
            return false;
        }
    } else if (tinydb_generic_consume_word(parser, "offset")) {
        if (!tinydb_generic_parse_uint32(parser, &ignored)) return false;
    }
    return tinydb_generic_consume_end(parser);
}

static bool parse_pair(Table* table,
                       const char* sql,
                       CorrelationDecision* decision) {
    memset(decision, 0, sizeof(*decision));
    if (table == NULL || sql == NULL) return false;

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select") ||
        !consume_projection(&parser) ||
        !tinydb_generic_consume_word(&parser, "from")) {
        return false;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
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

    if (!tinydb_generic_consume_word(&parser, "where") ||
        !tinydb_generic_parse_predicate(&parser,
                                        schema,
                                        &decision->first_predicate) ||
        !tinydb_generic_consume_word(&parser, "and") ||
        !tinydb_generic_parse_predicate(&parser,
                                        schema,
                                        &decision->second_predicate)) {
        return false;
    }

    TinyDBGenericParser third = parser;
    if (tinydb_generic_consume_word(&third, "and")) return false;
    if (!consume_tail(&parser)) return false;

    if (decision->first_predicate.op != TINYDB_GENERIC_COMPARE_EQ ||
        decision->second_predicate.op != TINYDB_GENERIC_COMPARE_EQ ||
        decision->first_predicate.column_index == 0 ||
        decision->second_predicate.column_index == 0 ||
        decision->first_predicate.column_index ==
            decision->second_predicate.column_index) {
        return false;
    }

    decision->schema = schema;
    decision->first_index = find_index_for_column(
        table, schema, decision->first_predicate.column_index);
    decision->second_index = find_index_for_column(
        table, schema, decision->second_predicate.column_index);
    return decision->first_index != NULL && decision->second_index != NULL &&
           decision->first_index != decision->second_index;
}

static bool estimate_decision(Table* table, CorrelationDecision* decision) {
    TinyDBGenericIndexEstimate first_estimate;
    TinyDBGenericIndexEstimate second_estimate;
    char message[TINYDB_GENERIC_SQL_MESSAGE_MAX] = {0};
    if (!tinydb_generic_index_estimate_candidates(
            table,
            decision->schema,
            decision->first_index,
            &decision->first_predicate,
            &first_estimate,
            message,
            sizeof(message)) ||
        !tinydb_generic_index_estimate_candidates(
            table,
            decision->schema,
            decision->second_index,
            &decision->second_predicate,
            &second_estimate,
            message,
            sizeof(message))) {
        return false;
    }

    TinyDBGenericPairEstimate pair_estimate;
    if (!tinydb_generic_index_estimate_pair_equality(
            table,
            decision->schema,
            decision->first_index,
            &decision->first_predicate,
            decision->second_index,
            &decision->second_predicate,
            &pair_estimate,
            message,
            sizeof(message))) {
        return false;
    }

    decision->first_rows = first_estimate.candidate_count;
    decision->second_rows = second_estimate.candidate_count;
    decision->joint_rows = pair_estimate.candidate_count;
    decision->table_rows = pair_estimate.total_count;
    if (first_estimate.total_count > decision->table_rows) {
        decision->table_rows = first_estimate.total_count;
    }
    if (second_estimate.total_count > decision->table_rows) {
        decision->table_rows = second_estimate.total_count;
    }

    decision->correlation_cost =
        ((uint64_t)decision->first_rows + (uint64_t)decision->second_rows) *
            TINYDB_GENERIC_COST_INDEX_ENTRY +
        (uint64_t)decision->joint_rows * TINYDB_GENERIC_COST_RANDOM_FETCH;
    uint64_t first_anchor = tinydb_generic_anchor_cost(decision->first_rows);
    uint64_t second_anchor = tinydb_generic_anchor_cost(decision->second_rows);
    decision->anchor_cost = first_anchor < second_anchor
        ? first_anchor
        : second_anchor;
    decision->scan_cost = tinydb_generic_scan_cost(decision->table_rows);
    decision->downgrade =
        decision->correlation_cost > decision->anchor_cost ||
        decision->correlation_cost > decision->scan_cost;
    return true;
}

static bool correlation_decision(Table* table,
                                 const char* sql,
                                 CorrelationDecision* decision) {
    return parse_pair(table, sql, decision) && estimate_decision(table, decision);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    CorrelationDecision decision;
    if (correlation_decision(table, sql, &decision)) {
        if (decision.downgrade) {
            return tinydb_generic_sql_try_execute_single_anchor_base(table, sql, result);
        }
        return tinydb_generic_sql_try_execute_intersection_forced(table, sql, result);
    }
    return tinydb_generic_sql_try_execute_intersection_base(table, sql, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    CorrelationDecision decision;
    bool correlated = correlation_decision(table, sql, &decision);

    TinyDBGenericSqlStatus status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    if (correlated && decision.downgrade) {
        status = tinydb_generic_sql_build_select_plan_single_anchor_base(
            table, sql, plan, result);
    } else if (correlated) {
        status = tinydb_generic_sql_build_select_plan_intersection_forced(
            table, sql, plan, result);
    } else {
        status = tinydb_generic_sql_build_select_plan_intersection_base(
            table, sql, plan, result);
    }

    if (correlated && status == TINYDB_GENERIC_SQL_SUCCESS &&
        plan != NULL && plan->applicable) {
        plan->has_correlation_estimate = true;
        plan->correlation_estimated_rows = decision.joint_rows;
        if (!decision.downgrade &&
            plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_INTERSECTION) {
            plan->has_cost_estimate = true;
            plan->estimated_rows = decision.joint_rows;
            plan->estimated_table_rows = decision.table_rows;
            plan->estimated_cost = decision.correlation_cost;
            plan->estimated_scan_cost = decision.scan_cost;
        }
    }
    return status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_intersection_base(plan);
    if (plan != NULL && plan->applicable && plan->has_correlation_estimate) {
        printf("  CORRELATION ESTIMATE: %u rows (pairwise equality synopsis)\n",
               plan->correlation_estimated_rows);
    }
}
