#include "generic_index_cost.h"

/*
 * Reuse the proven intersection parser/executor, but make its local admission
 * check unconditional. The correlation wrapper has already compared the exact
 * pairwise joint estimate against anchor/scan costs before calling this layer.
 * We still run the normal unary estimators so source ordering and EXPLAIN
 * metadata remain deterministic; only the old independence-based rejection is
 * suppressed here.
 */
static inline uint64_t tinydb_generic_intersection_cost_correlated(
    const uint32_t* source_rows,
    uint32_t source_count,
    uint32_t table_rows,
    uint32_t* estimated_rows) {
    (void)tinydb_generic_intersection_cost(source_rows,
                                           source_count,
                                           table_rows,
                                           estimated_rows);
    return 0;
}

#define tinydb_generic_intersection_cost tinydb_generic_intersection_cost_correlated
#define tinydb_generic_sql_try_execute tinydb_generic_sql_try_execute_intersection_forced
#define tinydb_generic_sql_build_select_plan tinydb_generic_sql_build_select_plan_intersection_forced
#define tinydb_generic_sql_print_plan tinydb_generic_sql_print_plan_intersection_forced
#include "generic_index_intersection_sql.c"
