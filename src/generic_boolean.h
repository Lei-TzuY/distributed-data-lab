#ifndef GENERIC_BOOLEAN_H
#define GENERIC_BOOLEAN_H

#include "generic_predicate.h"

#define TINYDB_GENERIC_BOOLEAN_MAX_NODES 64u

typedef enum {
    TINYDB_GENERIC_BOOLEAN_PREDICATE = 0,
    TINYDB_GENERIC_BOOLEAN_AND,
    TINYDB_GENERIC_BOOLEAN_OR
} TinyDBGenericBooleanKind;

typedef struct {
    TinyDBGenericBooleanKind kind;
    bool grouped;
    TinyDBGenericPredicate predicate;
    uint32_t left;
    uint32_t right;
} TinyDBGenericBooleanNode;

typedef struct {
    TinyDBGenericBooleanNode nodes[TINYDB_GENERIC_BOOLEAN_MAX_NODES];
    uint32_t count;
    uint32_t root;
    bool saw_grouping;
} TinyDBGenericBooleanExpression;

bool tinydb_generic_parse_boolean_expression(
    TinyDBGenericParser* parser,
    const TableSchema* schema,
    TinyDBGenericBooleanExpression* expression);

bool tinydb_generic_boolean_matches(
    const TinyDBGenericBooleanExpression* expression,
    const TinyDBValue* values,
    uint32_t value_count);

bool tinydb_generic_boolean_format(
    const TinyDBGenericBooleanExpression* expression,
    const TableSchema* schema,
    char* output,
    size_t output_size);

#endif /* GENERIC_BOOLEAN_H */
