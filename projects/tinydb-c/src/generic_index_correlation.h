#ifndef GENERIC_INDEX_CORRELATION_H
#define GENERIC_INDEX_CORRELATION_H

#include "generic_predicate.h"

#include <stddef.h>

typedef struct {
    uint32_t candidate_count;
    uint32_t total_count;
    bool persisted;
    bool exact_mcv;
} TinyDBGenericPairEstimate;

bool tinydb_generic_index_pair_stats_filename(
    Table* table,
    const GenericSecondaryIndex* first,
    const GenericSecondaryIndex* second,
    char* output,
    size_t output_size);

bool tinydb_generic_index_refresh_pair_statistics(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* first,
    GenericSecondaryIndex* second,
    char* message,
    size_t message_size);

bool tinydb_generic_index_estimate_pair_equality(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* first,
    const TinyDBGenericPredicate* first_predicate,
    GenericSecondaryIndex* second,
    const TinyDBGenericPredicate* second_predicate,
    TinyDBGenericPairEstimate* estimate,
    char* message,
    size_t message_size);

#endif /* GENERIC_INDEX_CORRELATION_H */
