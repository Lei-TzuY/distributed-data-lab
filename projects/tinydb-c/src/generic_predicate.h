#ifndef GENERIC_PREDICATE_H
#define GENERIC_PREDICATE_H

#include "record.h"

/* Predicate opcodes are stored as a fixed-width scalar rather than an enum.
 * Ordered index code can therefore switch only over sortable comparisons,
 * while residual-only operators such as LIKE remain explicit opcodes without
 * triggering compiler enum-exhaustiveness diagnostics in legacy range paths. */
typedef uint32_t TinyDBGenericCompareOp;
enum {
    TINYDB_GENERIC_COMPARE_EQ = 0,
    TINYDB_GENERIC_COMPARE_GT,
    TINYDB_GENERIC_COMPARE_GTE,
    TINYDB_GENERIC_COMPARE_LT,
    TINYDB_GENERIC_COMPARE_LTE,
    TINYDB_GENERIC_COMPARE_LIKE
};

typedef struct {
    const char* current;
} TinyDBGenericParser;

typedef struct {
    uint32_t column_index;
    TinyDBGenericCompareOp op;
    TinyDBValue value;
} TinyDBGenericPredicate;

void tinydb_generic_parser_init(TinyDBGenericParser* parser, const char* sql);
void tinydb_generic_skip_spaces(TinyDBGenericParser* parser);
bool tinydb_generic_consume_word(TinyDBGenericParser* parser, const char* word);
bool tinydb_generic_consume_char(TinyDBGenericParser* parser, char expected);
bool tinydb_generic_parse_identifier(TinyDBGenericParser* parser,
                                     char* output,
                                     size_t output_size);
bool tinydb_generic_parse_uint32(TinyDBGenericParser* parser, uint32_t* value);
bool tinydb_generic_consume_end(TinyDBGenericParser* parser);
int tinydb_generic_find_column_index(const TableSchema* schema, const char* name);
bool tinydb_generic_parse_value_for_column(TinyDBGenericParser* parser,
                                           const TableColumn* column,
                                           TinyDBValue* value);
bool tinydb_generic_parse_predicate(TinyDBGenericParser* parser,
                                    const TableSchema* schema,
                                    TinyDBGenericPredicate* predicate);
bool tinydb_generic_predicate_matches(const TinyDBGenericPredicate* predicate,
                                      const TinyDBValue* value);
const char* tinydb_generic_compare_op_text(TinyDBGenericCompareOp op);

#endif /* GENERIC_PREDICATE_H */
