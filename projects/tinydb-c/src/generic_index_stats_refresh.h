#ifndef GENERIC_INDEX_STATS_REFRESH_H
#define GENERIC_INDEX_STATS_REFRESH_H

#include "generic_index_candidates.h"

#include <stddef.h>

bool tinydb_generic_index_refresh_statistics(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    char* message,
    size_t message_size);

#endif /* GENERIC_INDEX_STATS_REFRESH_H */
