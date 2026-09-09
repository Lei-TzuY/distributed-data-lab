#include "generic_sql.h"

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_index_base(
    const TinyDBGenericSelectPlan* plan);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlStatus status = tinydb_generic_sql_build_select_plan_index_base(
        table, sql, plan, result);
    if (status == TINYDB_GENERIC_SQL_SUCCESS &&
        plan != NULL && plan->filter_expression[0] != '\0' &&
        plan->kind == TINYDB_GENERIC_PLAN_SECONDARY_INDEX_LOOKUP) {
        /*
         * The current generic secondary-index executor only proves a complete
         * single equality predicate. A compound AND plan may have an indexed
         * equality term, but the remaining terms still require decoded row
         * evaluation. Keep EXPLAIN aligned with the execution path until an
         * index-plus-residual-filter plan exists.
         */
        plan->kind = TINYDB_GENERIC_PLAN_FULL_SCAN;
        plan->index_name[0] = '\0';
    }
    return status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_index_base(plan);
}
