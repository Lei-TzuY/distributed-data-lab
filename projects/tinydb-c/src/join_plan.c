#include "join_plan.h"

#include <ctype.h>

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static const TableSchema* find_schema(const Table* table, const char* name) {
    if (table == NULL || name == NULL || name[0] == '\0') return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool schema_is_row_compatible(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3 &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static const char* column_basename(const char* column) {
    const char* dot = strrchr(column, '.');
    return dot == NULL ? column : dot + 1;
}

static bool simple_primary_key_join_supported(const SelectStatement* sel) {
    return sel->aggregate == AGGREGATE_NONE &&
           !sel->has_project_col &&
           !sel->has_id_filter &&
           !sel->has_id_min_filter &&
           !sel->has_id_max_filter &&
           !sel->has_username_filter &&
           !sel->has_username_like &&
           !sel->has_email_filter &&
           !sel->has_email_like &&
           !sel->has_group_by &&
           !sel->has_having &&
           !sel->has_in_subquery &&
           !sel->has_exists_subquery &&
           !sel->is_distinct &&
           !sel->has_match_filter &&
           !sel->has_window_func &&
           !sel->is_union &&
           !sel->has_is_null_filter &&
           !sel->has_is_not_null_filter &&
           !sel->has_order_by_col &&
           !sel->has_secondary_order_by &&
           !sel->has_in_list &&
           !sel->has_between_filter &&
           !sel->has_scalar_subquery &&
           sel->str_func == STRING_FUNC_NONE &&
           sel->math_func == MATH_FUNC_NONE &&
           !sel->has_order_desc &&
           ci_equal(column_basename(sel->join_left_col), "id") &&
           ci_equal(column_basename(sel->join_right_col), "id");
}

MultiTableRouteResult tinydb_build_join_plan(Table* table,
                                             const Statement* statement,
                                             TinyDBJoinPlan* plan) {
    if (plan != NULL) memset(plan, 0, sizeof(*plan));
    if (table == NULL || statement == NULL || plan == NULL ||
        statement->type != STATEMENT_SELECT || !statement->select.has_join) {
        return MULTITABLE_ROUTE_NOT_APPLICABLE;
    }

    const SelectStatement* sel = &statement->select;
    const char* left_name = statement->table_name[0] != '\0'
        ? statement->table_name
        : sel->table_name;
    const TableSchema* left_schema = find_schema(table, left_name);
    const TableSchema* right_schema = find_schema(table, sel->join_table);

    if (left_schema == NULL || right_schema == NULL) {
        plan->applicable = true;
        return MULTITABLE_ROUTE_TABLE_NOT_FOUND;
    }
    if (!schema_is_row_compatible(left_schema) ||
        !schema_is_row_compatible(right_schema)) {
        plan->applicable = true;
        return MULTITABLE_ROUTE_INCOMPATIBLE_SCHEMA;
    }
    if (left_schema->root_page_num == right_schema->root_page_num) {
        return MULTITABLE_ROUTE_NOT_APPLICABLE;
    }

    plan->applicable = true;
    snprintf(plan->left_table, sizeof(plan->left_table), "%s", left_schema->name);
    snprintf(plan->right_table, sizeof(plan->right_table), "%s", right_schema->name);
    plan->left_root_page_num = left_schema->root_page_num;
    plan->right_root_page_num = right_schema->root_page_num;

    if (!simple_primary_key_join_supported(sel)) {
        return MULTITABLE_ROUTE_UNSUPPORTED_QUERY;
    }
    return MULTITABLE_ROUTE_OK;
}

void tinydb_print_join_plan(const TinyDBJoinPlan* plan,
                            const SelectStatement* select_statement) {
    if (plan == NULL || !plan->applicable) return;
    printf("PLAN: CROSS-ROOT PRIMARY KEY NESTED LOOP JOIN\n");
    printf("      LEFT: FULL TABLE SCAN %s (root page %u)\n",
           plan->left_table,
           plan->left_root_page_num);
    printf("      RIGHT: PRIMARY KEY LOOKUP %s.id (root page %u)\n",
           plan->right_table,
           plan->right_root_page_num);
    if (select_statement != NULL && select_statement->has_offset) {
        printf("      OFFSET %u\n", select_statement->offset);
    }
    if (select_statement != NULL && select_statement->has_limit) {
        printf("      LIMIT %u\n", select_statement->limit);
    }
}
