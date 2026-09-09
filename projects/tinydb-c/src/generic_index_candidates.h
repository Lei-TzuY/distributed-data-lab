#ifndef GENERIC_INDEX_CANDIDATES_H
#define GENERIC_INDEX_CANDIDATES_H

#include "generic_predicate.h"
#include "record.h"

#include <stddef.h>

typedef struct {
    uint32_t* ids;
    uint32_t count;
} TinyDBGenericIndexCandidates;

typedef struct {
    uint32_t candidate_count;
    uint32_t total_count;
} TinyDBGenericIndexEstimate;

bool tinydb_generic_index_collect_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
    TinyDBGenericIndexCandidates* candidates,
    char* message,
    size_t message_size);

bool tinydb_generic_index_collect_conjunctive_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericIndexCandidates* candidates,
    char* message,
    size_t message_size);

bool tinydb_generic_index_estimate_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size);

bool tinydb_generic_index_estimate_conjunctive_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size);

void tinydb_generic_index_candidates_free(
    TinyDBGenericIndexCandidates* candidates);

#endif /* GENERIC_INDEX_CANDIDATES_H */
