#ifndef CATALOG_PRAGMAS_H
#define CATALOG_PRAGMAS_H

#include "compiler.h"
#include "table.h"

typedef enum {
    CATALOG_PRAGMA_NOT_HANDLED = 0,
    CATALOG_PRAGMA_SUCCESS,
    CATALOG_PRAGMA_TABLE_NOT_FOUND,
    CATALOG_PRAGMA_INVALID_TARGET
} CatalogPragmaResult;

CatalogPragmaResult tinydb_execute_catalog_pragma(Table* table,
                                                  StatementType type,
                                                  const char* sql,
                                                  char* error_message,
                                                  size_t error_message_size);

#endif /* CATALOG_PRAGMAS_H */
