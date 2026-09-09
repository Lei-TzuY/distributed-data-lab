#include "generic_boolean.h"
#include "generic_index_candidates.h"
#include "generic_index_cost.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_wide_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_wide_base(
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
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static GenericSecondaryIndex* find_single_column_index(Table* table,
                                                        const char* table_name,
                                                        const char* column_name) {
    if (table == NULL || table_name == NULL || column_name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && index->num_columns == 1u &&
            ci_equal(index->table_name, table_name) &&
            ci_equal(index->column_name, column_name)) {
            return index;
        }
    }
    return NULL;
}

static bool is_legacy_fixed_row_schema(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3u &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static void initialize_result(TinyDBGenericSqlResult* result) {
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static TinyDBGenericSqlStatus syntax_error(TinyDBGenericSqlResult* result,
                                            const char* message) {
    if (result != NULL) {
        initialize_result(result);
        result->status = TINYDB_GENERIC_SQL_SYNTAX_ERROR;
        result->statement_type = STATEMENT_SELECT;
        result->statement_type_valid = true;
        snprintf(result->message, sizeof(result->message), "%s", message);
    }
    return TINYDB_GENERIC_SQL_SYNTAX_ERROR;
}

static TinyDBGenericSqlStatus execute_error(TinyDBGenericSqlResult* result,
                                             const char* message) {
    if (result != NULL) {
        initialize_result(result);
        result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
        result->statement_type = STATEMENT_SELECT;
        result->statement_type_valid = true;
        result->execute_result = EXECUTE_KEY_NOT_FOUND;
        snprintf(result->message, sizeof(result->message), "%s", message);
    }
    return TINYDB_GENERIC_SQL_EXECUTE_ERROR;
}

static TinyDBGenericSqlStatus success(TinyDBGenericSqlResult* result) {
    if (result != NULL) {
        initialize_result(result);
        result->status = TINYDB_GENERIC_SQL_SUCCESS;
        result->statement_type = STATEMENT_SELECT;
        result->statement_type_valid = true;
        result->execute_result = EXECUTE_SUCCESS;
    }
    return TINYDB_GENERIC_SQL_SUCCESS;
}

static bool parse_projection(TinyDBGenericParser* parser,
                             char* projection_name,
                             size_t projection_name_size,
                             bool* is_star,
                             bool* is_count) {
    *is_star = false;
    *is_count = false;
    projection_name[0] = '\0';

    if (tinydb_generic_consume_char(parser, '*')) {
        *is_star = true;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        *is_count = true;
        return true;
    }
    *parser = backup;

    return tinydb_generic_parse_identifier(
        parser, projection_name, projection_name_size);
}

static const TinyDBGenericPredicate* first_predicate(
    const TinyDBGenericBooleanExpression* expression) {
    for (uint32_t i = 0u; i < expression->count; i++) {
        if (expression->nodes[i].kind == TINYDB_GENERIC_BOOLEAN_PREDICATE) {
            return &expression->nodes[i].predicate;
        }
    }
    return NULL;
}

static bool is_single_predicate(const TinyDBGenericBooleanExpression* expression,
                                const TinyDBGenericPredicate** predicate) {
    if (expression == NULL || expression->count == 0u ||
        expression->root >= expression->count) {
        return false;
    }
    const TinyDBGenericBooleanNode* root = &expression->nodes[expression->root];
    if (root->kind != TINYDB_GENERIC_BOOLEAN_PREDICATE) return false;
    if (predicate != NULL) *predicate = &root->predicate;
    return true;
}

static bool is_exact_primary_key_lookup(
    const TinyDBGenericBooleanExpression* expression) {
    const TinyDBGenericPredicate* predicate = NULL;
    return is_single_predicate(expression, &predicate) &&
           predicate->column_index == 0u &&
           predicate->op == TINYDB_GENERIC_COMPARE_EQ;
}

static bool collect_and_predicates_at(
    const TinyDBGenericBooleanExpression* expression,
    uint32_t node_index,
    TinyDBGenericPredicate* predicates,
    uint32_t* predicate_count) {
    if (expression == NULL || predicates == NULL || predicate_count == NULL ||
        node_index >= expression->count) {
        return false;
    }

    const TinyDBGenericBooleanNode* node = &expression->nodes[node_index];
    if (node->kind == TINYDB_GENERIC_BOOLEAN_PREDICATE) {
        if (*predicate_count >= TINYDB_GENERIC_BOOLEAN_MAX_NODES) return false;
        predicates[*predicate_count] = node->predicate;
        (*predicate_count)++;
        return true;
    }
    if (node->kind != TINYDB_GENERIC_BOOLEAN_AND) return false;

    return collect_and_predicates_at(expression,
                                     node->left,
                                     predicates,
                                     predicate_count) &&
           collect_and_predicates_at(expression,
                                     node->right,
                                     predicates,
                                     predicate_count);
}

static bool collect_and_predicates(
    const TinyDBGenericBooleanExpression* expression,
    TinyDBGenericPredicate* predicates,
    uint32_t* predicate_count) {
    if (predicate_count == NULL) return false;
    *predicate_count = 0u;
    if (expression == NULL || expression->count == 0u ||
        expression->root >= expression->count) {
        return false;
    }
    return collect_and_predicates_at(expression,
                                     expression->root,
                                     predicates,
                                     predicate_count);
}

static bool secondary_index_operator_supported(TinyDBGenericCompareOp op) {
    return op == TINYDB_GENERIC_COMPARE_EQ ||
           op == TINYDB_GENERIC_COMPARE_GT ||
           op == TINYDB_GENERIC_COMPARE_GTE ||
           op == TINYDB_GENERIC_COMPARE_LT ||
           op == TINYDB_GENERIC_COMPARE_LTE;
}

static GenericSecondaryIndex* choose_secondary_index_anchor(
    Table* table,
    const TableSchema* schema,
    const TinyDBGenericBooleanExpression* expression,
    TinyDBGenericPredicate* anchor_predicates,
    uint32_t* anchor_predicate_count) {
    TinyDBGenericPredicate predicates[TINYDB_GENERIC_BOOLEAN_MAX_NODES];
    uint32_t predicate_count = 0u;
    if (anchor_predicate_count != NULL) *anchor_predicate_count = 0u;
    if (table == NULL || schema == NULL || anchor_predicates == NULL ||
        anchor_predicate_count == NULL ||
        !collect_and_predicates(expression, predicates, &predicate_count)) {
        return NULL;
    }

    GenericSecondaryIndex* best_index = NULL;
    uint32_t best_column = 0u;
    uint32_t best_count = 0u;

    for (uint32_t i = 0u; i < predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &predicates[i];
        if (predicate->column_index == 0u ||
            predicate->column_index >= schema->num_columns ||
            !secondary_index_operator_supported(predicate->op)) {
            continue;
        }

        GenericSecondaryIndex* index = find_single_column_index(
            table,
            schema->name,
            schema->columns[predicate->column_index].name);
        if (index == NULL) continue;

        uint32_t same_column_count = 0u;
        for (uint32_t j = 0u; j < predicate_count; j++) {
            if (predicates[j].column_index == predicate->column_index &&
                secondary_index_operator_supported(predicates[j].op)) {
                same_column_count++;
            }
        }

        if (same_column_count > best_count) {
            best_index = index;
            best_column = predicate->column_index;
            best_count = same_column_count;
        }
    }

    if (best_index == NULL) return NULL;

    for (uint32_t i = 0u; i < predicate_count; i++) {
        if (predicates[i].column_index == best_column &&
            secondary_index_operator_supported(predicates[i].op)) {
            anchor_predicates[*anchor_predicate_count] = predicates[i];
            (*anchor_predicate_count)++;
        }
    }
    return best_index;
}

static void annotate_secondary_index_estimate(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericSelectPlan* plan) {
    if (table == NULL || schema == NULL || index == NULL || predicates == NULL ||
        predicate_count == 0u || plan == NULL) {
        return;
    }

    TinyDBGenericIndexEstimate estimate;
    char message[TINYDB_RECORD_MESSAGE_MAX] = {0};
    bool estimated = false;
    if (predicate_count == 1u) {
        estimated = tinydb_generic_index_estimate_candidates(table,
                                                              schema,
                                                              index,
                                                              &predicates[0],
                                                              &estimate,
                                                              message,
                                                              sizeof(message));
    } else {
        estimated = tinydb_generic_index_estimate_conjunctive_candidates(
            table,
            schema,
            index,
            predicates,
            predicate_count,
            &estimate,
            message,
            sizeof(message));
    }
    if (!estimated) return;

    plan->has_cost_estimate = true;
    plan->estimated_rows = estimate.candidate_count;
    plan->estimated_table_rows = estimate.total_count;
    plan->estimated_cost = tinydb_generic_anchor_cost(estimate.candidate_count);
    plan->estimated_scan_cost = tinydb_generic_scan_cost(estimate.total_count);
}

static void format_filter_value(const TinyDBGenericPredicate* predicate,
                                char* output,
                                size_t output_size) {
    if (predicate->value.type == COL_TYPE_INT) {
        snprintf(output, output_size, "%u", predicate->value.int_value);
    } else {
        snprintf(output, output_size, "'%s'", predicate->value.text);
    }
}

static TinyDBGenericSqlStatus try_build_wide_payload_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    if (table == NULL || sql == NULL || plan == NULL) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char projection_name[MAX_NAME_SIZE];
    bool projection_star = false;
    bool projection_count = false;
    if (!parse_projection(&parser,
                          projection_name,
                          sizeof(projection_name),
                          &projection_star,
                          &projection_count)) {
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
    if (schema == NULL || schema->row_size <= ROW_SIZE ||
        is_legacy_fixed_row_schema(schema)) {
        return TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_schema_supported(schema,
                                                schema_message,
                                                sizeof(schema_message))) {
        return execute_error(result, schema_message);
    }

    memset(plan, 0, sizeof(*plan));
    plan->applicable = true;
    plan->kind = TINYDB_GENERIC_PLAN_FULL_SCAN;
    plan->root_page_num = schema->root_page_num;
    snprintf(plan->table_name, sizeof(plan->table_name), "%s", schema->name);

    if (projection_star) {
        snprintf(plan->projection, sizeof(plan->projection), "*");
    } else if (projection_count) {
        snprintf(plan->projection, sizeof(plan->projection), "COUNT(*)");
    } else {
        int projection_index =
            tinydb_generic_find_column_index(schema, projection_name);
        if (projection_index < 0) {
            return syntax_error(
                result,
                "generic SELECT projection references an unknown column");
        }
        snprintf(plan->projection,
                 sizeof(plan->projection),
                 "%s",
                 schema->columns[projection_index].name);
    }

    if (tinydb_generic_consume_end(&parser)) return success(result);
    if (!tinydb_generic_consume_word(&parser, "where")) {
        return syntax_error(result,
                            "generic SELECT contains an unsupported clause");
    }

    TinyDBGenericBooleanExpression expression;
    if (!tinydb_generic_parse_boolean_expression(&parser, schema, &expression)) {
        return syntax_error(
            result,
            "generic SELECT WHERE requires a typed predicate expression");
    }

    if (tinydb_generic_consume_word(&parser, "limit")) {
        uint32_t ignored_limit = 0u;
        if (!tinydb_generic_parse_uint32(&parser, &ignored_limit)) {
            return syntax_error(result, "LIMIT requires an integer");
        }
        if (tinydb_generic_consume_word(&parser, "offset")) {
            uint32_t ignored_offset = 0u;
            if (!tinydb_generic_parse_uint32(&parser, &ignored_offset)) {
                return syntax_error(result, "OFFSET requires an integer");
            }
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        uint32_t ignored_offset = 0u;
        if (!tinydb_generic_parse_uint32(&parser, &ignored_offset)) {
            return syntax_error(result, "OFFSET requires an integer");
        }
    }

    if (!tinydb_generic_consume_end(&parser)) {
        return syntax_error(
            result,
            "generic SELECT contains an unsupported clause after its predicates");
    }

    plan->has_filter = true;
    if (is_exact_primary_key_lookup(&expression)) {
        plan->kind = TINYDB_GENERIC_PLAN_PRIMARY_KEY_LOOKUP;
    } else {
        TinyDBGenericPredicate anchor_predicates[TINYDB_GENERIC_BOOLEAN_MAX_NODES];
        uint32_t anchor_predicate_count = 0u;
        GenericSecondaryIndex* index = choose_secondary_index_anchor(
            table,
            schema,
            &expression,
            anchor_predicates,
            &anchor_predicate_count);
        if (index != NULL && anchor_predicate_count > 0u) {
            plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_LOOKUP;
            plan->index_term_count = anchor_predicate_count;
            snprintf(plan->index_name,
                     sizeof(plan->index_name),
                     "%s",
                     index->name);
            annotate_secondary_index_estimate(table,
                                              schema,
                                              index,
                                              anchor_predicates,
                                              anchor_predicate_count,
                                              plan);
        } else {
            plan->kind = TINYDB_GENERIC_PLAN_FULL_SCAN;
        }
    }

    if (!tinydb_generic_boolean_format(&expression,
                                       schema,
                                       plan->filter_expression,
                                       sizeof(plan->filter_expression))) {
        return syntax_error(result, "predicate plan text exceeds capacity");
    }

    const TinyDBGenericPredicate* first = first_predicate(&expression);
    if (first != NULL) {
        snprintf(plan->filter_column,
                 sizeof(plan->filter_column),
                 "%s",
                 schema->columns[first->column_index].name);
        snprintf(plan->filter_operator,
                 sizeof(plan->filter_operator),
                 "%s",
                 tinydb_generic_compare_op_text(first->op));
        format_filter_value(first,
                            plan->filter_value,
                            sizeof(plan->filter_value));
    }

    return success(result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    if (result != NULL) initialize_result(result);
    if (plan != NULL) memset(plan, 0, sizeof(*plan));

    TinyDBGenericSqlStatus wide_status =
        try_build_wide_payload_plan(table, sql, plan, result);
    if (wide_status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) {
        return wide_status;
    }

    if (result != NULL) initialize_result(result);
    if (plan != NULL) memset(plan, 0, sizeof(*plan));
    return tinydb_generic_sql_build_select_plan_wide_base(
        table, sql, plan, result);
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_wide_base(plan);
    if (plan != NULL && plan->has_filter && plan->filter_expression[0] != '\0' &&
        strstr(plan->filter_expression, " AND ") != NULL) {
        printf("  FILTER EXPRESSION: %s\n", plan->filter_expression);
    }
}
