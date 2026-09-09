#ifndef TABLE_STATS_TRY_H
#define TABLE_STATS_TRY_H

#include "table.h"

/*
 * Additive non-fatal counterpart to the historical void db_get_stats().
 * Output is zeroed before work and published only after every page needed by
 * the statistics pass can be acquired and released successfully.
 */
bool db_try_get_stats(Table* table,
                      TableStats* stats,
                      char* message,
                      size_t message_size);

#endif /* TABLE_STATS_TRY_H */
