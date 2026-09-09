#include "profile.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

bool query_profile_execute(Statement* statement, Table* table, QueryProfile* profile) {
    Statement plan_statement;
    Statement actual_statement;
    uint32_t hits_before;
    uint32_t misses_before;
    uint32_t evictions_before;
    clock_t started;
    clock_t finished;

    if (statement == NULL || table == NULL || profile == NULL ||
        statement->type != STATEMENT_SELECT) {
        return false;
    }

    memset(profile, 0, sizeof(*profile));

    plan_statement = *statement;
    plan_statement.explain = true;
    printf("QUERY PLAN\n");
    profile->plan_result = execute_statement(&plan_statement, table);
    if (profile->plan_result != EXECUTE_SUCCESS) {
        profile->execute_result = profile->plan_result;
        return true;
    }

    actual_statement = *statement;
    actual_statement.explain = false;

    hits_before = table->pager->cache_hits;
    misses_before = table->pager->cache_misses;
    evictions_before = table->pager->evictions;

    printf("ACTUAL RESULT\n");
    started = clock();
    profile->execute_result = execute_statement(&actual_statement, table);
    finished = clock();

    profile->execution_time_ms =
        1000.0 * (double)(finished - started) / (double)CLOCKS_PER_SEC;
    profile->cache_hits = table->pager->cache_hits - hits_before;
    profile->cache_misses = table->pager->cache_misses - misses_before;
    profile->evictions = table->pager->evictions - evictions_before;
    profile->page_accesses = profile->cache_hits + profile->cache_misses;

    return true;
}
