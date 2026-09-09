#ifndef ANALYZE_SQL_H
#define ANALYZE_SQL_H

#include "table.h"

#define TINYDB_ANALYZE_MESSAGE_MAX 256

typedef enum {
    TINYDB_ANALYZE_NOT_APPLICABLE = 0,
    TINYDB_ANALYZE_SUCCESS,
    TINYDB_ANALYZE_SYNTAX_ERROR,
    TINYDB_ANALYZE_POLICY_ERROR,
    TINYDB_ANALYZE_EXECUTE_ERROR
} TinyDBAnalyzeStatus;

typedef struct {
    TinyDBAnalyzeStatus status;
    uint32_t refreshed_indexes;
    uint32_t refreshed_correlations;
    char message[TINYDB_ANALYZE_MESSAGE_MAX];
} TinyDBAnalyzeResult;

TinyDBAnalyzeStatus tinydb_analyze_try_execute(
    Table* table,
    const char* sql,
    TinyDBAnalyzeResult* result);

#endif /* ANALYZE_SQL_H */
