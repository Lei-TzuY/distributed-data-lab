#include "generic_index_candidates.h"

#include <string.h>

#define GENERIC_STATS_MIN_TABLE_ROWS 64u

bool tinydb_generic_index_estimate_candidates_stats_base(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size);

bool tinydb_generic_index_estimate_conjunctive_candidates_stats_base(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size);

bool tinydb_generic_index_estimate_candidates_exact_base(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size);

bool tinydb_generic_index_estimate_conjunctive_candidates_exact_base(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size);

bool tinydb_generic_index_estimate_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size) {
    TinyDBGenericIndexEstimate synopsis;
    memset(&synopsis, 0, sizeof(synopsis));
    if (tinydb_generic_index_estimate_candidates_stats_base(
            table,
            schema,
            index,
            predicate,
            &synopsis,
            message,
            message_size) &&
        synopsis.total_count >= GENERIC_STATS_MIN_TABLE_ROWS) {
        if (estimate != NULL) *estimate = synopsis;
        return true;
    }

    return tinydb_generic_index_estimate_candidates_exact_base(
        table,
        schema,
        index,
        predicate,
        estimate,
        message,
        message_size);
}

bool tinydb_generic_index_estimate_conjunctive_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size) {
    TinyDBGenericIndexEstimate synopsis;
    memset(&synopsis, 0, sizeof(synopsis));
    if (tinydb_generic_index_estimate_conjunctive_candidates_stats_base(
            table,
            schema,
            index,
            predicates,
            predicate_count,
            &synopsis,
            message,
            message_size) &&
        synopsis.total_count >= GENERIC_STATS_MIN_TABLE_ROWS) {
        if (estimate != NULL) *estimate = synopsis;
        return true;
    }

    return tinydb_generic_index_estimate_conjunctive_candidates_exact_base(
        table,
        schema,
        index,
        predicates,
        predicate_count,
        estimate,
        message,
        message_size);
}
