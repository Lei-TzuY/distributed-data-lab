#ifndef MULTITABLE_H
#define MULTITABLE_H

#include "vm.h"

#include <stddef.h>

typedef enum {
    MULTITABLE_ROUTE_NOT_APPLICABLE = 0,
    MULTITABLE_ROUTE_OK,
    MULTITABLE_ROUTE_TABLE_NOT_FOUND,
    MULTITABLE_ROUTE_INCOMPATIBLE_SCHEMA,
    MULTITABLE_ROUTE_UNSUPPORTED_QUERY
} MultiTableRouteResult;

typedef struct {
    uint32_t previous_root_page_num;
    bool previous_username_index_enabled;
    uint32_t previous_num_sec_indexes;
    bool active;
    bool indexes_suppressed;
    char table_name[MAX_NAME_SIZE];
} MultiTableRouteScope;

bool multitable_catalog_load(Table* table, const char* database_filename);
bool multitable_catalog_save(Table* table, const char* database_filename);

MultiTableRouteResult multitable_begin_statement_scope(
    Table* table,
    Statement* statement,
    const char* sql,
    MultiTableRouteScope* scope
);
void multitable_end_statement_scope(Table* table, MultiTableRouteScope* scope);

MultiTableRouteResult multitable_execute_join(
    Table* table,
    Statement* statement,
    bool* handled,
    ExecuteResult* execute_result
);

ExecuteResult multitable_execute_delete_all(
    Statement* statement,
    Table* table,
    const MultiTableRouteScope* scope
);

bool multitable_is_schema_ddl(StatementType type);
bool multitable_index_target_supported(Table* table, const char* table_name);
const char* multitable_route_error(MultiTableRouteResult result);

#endif /* MULTITABLE_H */
