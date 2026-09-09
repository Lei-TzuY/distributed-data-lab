#ifndef ENGINE_H
#define ENGINE_H

#include "multitable.h"
#include "profile.h"

#define TINYDB_ENGINE_FILENAME_MAX 768
#define TINYDB_ENGINE_MESSAGE_MAX 256

typedef struct {
    Table* table;
    char filename[TINYDB_ENGINE_FILENAME_MAX];
} TinyDB;

typedef enum {
    TINYDB_SQL_SUCCESS = 0,
    TINYDB_SQL_SYNTAX_ERROR,
    TINYDB_SQL_UNRECOGNIZED_STATEMENT,
    TINYDB_SQL_POLICY_ERROR,
    TINYDB_SQL_ROUTE_ERROR,
    TINYDB_SQL_EXECUTE_ERROR,
    TINYDB_SQL_CATALOG_PERSIST_ERROR
} TinyDBSqlStatus;

typedef struct {
    TinyDBSqlStatus status;
    PrepareResult prepare_result;
    ExecuteResult execute_result;
    MultiTableRouteResult route_result;
    StatementType statement_type;
    bool statement_type_valid;
    bool executed;
    bool join_handled;
    bool schema_persisted;
    bool has_profile;
    QueryProfile profile;
    char message[TINYDB_ENGINE_MESSAGE_MAX];
} TinyDBSqlResult;

TinyDB* tinydb_open(const char* filename);
void tinydb_close(TinyDB* database);
Table* tinydb_table(TinyDB* database);
const char* tinydb_filename(const TinyDB* database);

TinyDBSqlStatus tinydb_execute_sql(TinyDB* database,
                                   const char* sql,
                                   TinyDBSqlResult* result);

const char* tinydb_sql_status_string(TinyDBSqlStatus status);

#endif /* ENGINE_H */
