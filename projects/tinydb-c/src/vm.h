#ifndef VM_H
#define VM_H

#include "compiler.h"
#include "table.h"

typedef enum {
    EXECUTE_SUCCESS,
    EXECUTE_TABLE_FULL,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_KEY_NOT_FOUND,
    EXECUTE_TRANSACTION_ALREADY_ACTIVE,
    EXECUTE_NO_ACTIVE_TRANSACTION,
    EXECUTE_DDL_INSIDE_TRANSACTION,
    EXECUTE_SAVEPOINT_NOT_FOUND,
    EXECUTE_SAVEPOINT_STACK_FULL
} ExecuteResult;

ExecuteResult execute_statement(Statement* statement, Table* table);

#endif // VM_H
