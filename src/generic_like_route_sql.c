#include "generic_sql.h"

#include <ctype.h>
#include <string.h>

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_range_select(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_like_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_or_scan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_anchor_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_range(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_like_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_or_scan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_anchor_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_like_base(
    const TinyDBGenericSelectPlan* plan);

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool starts_keyword(const char* current,
                           const char* sql,
                           const char* keyword) {
    size_t length = strlen(keyword);
    for (size_t i = 0; i < length; i++) {
        if (current[i] == '\0' || ci_char(current[i]) != ci_char(keyword[i])) {
            return false;
        }
    }
    return (current == sql || !is_identifier_char(current[-1])) &&
           !is_identifier_char(current[length]);
}

static bool contains_keyword(const char* sql, const char* keyword) {
    bool in_string = false;
    const char* current = sql;

    while (*current != '\0') {
        if (*current == '\'') {
            if (in_string && current[1] == '\'') {
                current += 2;
                continue;
            }
            in_string = !in_string;
            current++;
            continue;
        }
        if (!in_string && starts_keyword(current, sql, keyword)) return true;
        current++;
    }
    return false;
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    if (sql != NULL && contains_keyword(sql, "like")) {
        /*
         * LIKE remains residual-only. Flat OR must stay on the boolean scan
         * evaluator because index union cannot use LIKE as a candidate source.
         * For flat AND, however, the intersection/anchor chain may safely use
         * other ordered predicates to narrow candidates and then re-evaluate
         * LIKE against the fetched row. A lone LIKE predicate stays on the
         * schema-aware residual scan path.
         *
         * Parenthesized expressions are intercepted by the outer grouped layer
         * before this router is reached.
         */
        if (contains_keyword(sql, "or")) {
            return tinydb_generic_sql_try_execute_or_scan_base(table, sql, result);
        }
        if (contains_keyword(sql, "and")) {
            return tinydb_generic_sql_try_execute_anchor_index_base(table, sql, result);
        }
        return tinydb_generic_sql_try_execute_range_select(table, sql, result);
    }
    return tinydb_generic_sql_try_execute_like_base(table, sql, result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    if (sql != NULL && contains_keyword(sql, "like")) {
        if (contains_keyword(sql, "or")) {
            return tinydb_generic_sql_build_select_plan_or_scan_base(
                table, sql, plan, result);
        }
        if (contains_keyword(sql, "and")) {
            return tinydb_generic_sql_build_select_plan_anchor_index_base(
                table, sql, plan, result);
        }
        return tinydb_generic_sql_build_select_plan_range(table, sql, plan, result);
    }
    return tinydb_generic_sql_build_select_plan_like_base(table, sql, plan, result);
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    tinydb_generic_sql_print_plan_like_base(plan);
}
