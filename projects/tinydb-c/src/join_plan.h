#ifndef JOIN_PLAN_H
#define JOIN_PLAN_H

#include "multitable.h"

typedef struct {
    bool applicable;
    char left_table[MAX_NAME_SIZE];
    char right_table[MAX_NAME_SIZE];
    uint32_t left_root_page_num;
    uint32_t right_root_page_num;
} TinyDBJoinPlan;

MultiTableRouteResult tinydb_build_join_plan(Table* table,
                                             const Statement* statement,
                                             TinyDBJoinPlan* plan);
void tinydb_print_join_plan(const TinyDBJoinPlan* plan,
                            const SelectStatement* select_statement);

#endif /* JOIN_PLAN_H */
