#ifndef GENERIC_SQL_H
#define GENERIC_SQL_H

#include "record.h"
#include "vm.h"

#define TINYDB_GENERIC_SQL_MESSAGE_MAX 256
#define TINYDB_GENERIC_PLAN_TEXT_MAX 256
#define TINYDB_GENERIC_PLAN_OPERATOR_MAX 3
#define TINYDB_GENERIC_PLAN_FILTER_EXPRESSION_MAX 1024

typedef enum {
    TINYDB_GENERIC_SQL_NOT_APPLICABLE = 0,
    TINYDB_GENERIC_SQL_SUCCESS,
    TINYDB_GENERIC_SQL_SYNTAX_ERROR,
    TINYDB_GENERIC_SQL_EXECUTE_ERROR
} TinyDBGenericSqlStatus;

typedef enum {
    TINYDB_GENERIC_PLAN_FULL_SCAN = 0,
    TINYDB_GENERIC_PLAN_PRIMARY_KEY_LOOKUP,
    TINYDB_GENERIC_PLAN_SECONDARY_INDEX_LOOKUP,
    TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RANGE,
    TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RESIDUAL,
    TINYDB_GENERIC_PLAN_SECONDARY_INDEX_INTERSECTION,
    TINYDB_GENERIC_PLAN_SECONDARY_INDEX_UNION
} TinyDBGenericPlanKind;

typedef struct {
    bool applicable;
    TinyDBGenericPlanKind kind;
    uint32_t root_page_num;
    char table_name[MAX_NAME_SIZE];
    char projection[TINYDB_GENERIC_PLAN_TEXT_MAX];
    bool has_filter;
    char filter_column[MAX_NAME_SIZE];
    char filter_operator[TINYDB_GENERIC_PLAN_OPERATOR_MAX];
    char filter_value[TINYDB_GENERIC_PLAN_TEXT_MAX];
    char filter_expression[TINYDB_GENERIC_PLAN_FILTER_EXPRESSION_MAX];
    char index_name[MAX_NAME_SIZE];
    uint32_t index_branch_count;
    uint32_t index_term_count;
    uint32_t index_fused_source_count;
    bool has_cost_estimate;
    uint32_t estimated_rows;
    uint32_t estimated_table_rows;
    uint64_t estimated_cost;
    uint64_t estimated_scan_cost;
    bool has_correlation_estimate;
    uint32_t correlation_estimated_rows;
} TinyDBGenericSelectPlan;

typedef struct {
    TinyDBGenericSqlStatus status;
    StatementType statement_type;
    ExecuteResult execute_result;
    bool statement_type_valid;
    bool executed;
    char message[TINYDB_GENERIC_SQL_MESSAGE_MAX];
} TinyDBGenericSqlResult;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(Table* table,
                                                       const char* sql,
                                                       TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan);

#endif /* GENERIC_SQL_H */
