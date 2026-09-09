#ifndef GENERIC_INDEX_EPOCH_H
#define GENERIC_INDEX_EPOCH_H

#include "table.h"

#include <stdint.h>

bool tinydb_generic_index_epoch_current(Table* table, uint64_t* epoch);
bool tinydb_generic_index_epoch_before_mutation(Table* table,
                                                const TableSchema* schema);

/* generic_index_candidates.c includes its public candidate header immediately
 * before this header.  Inject the payload compatibility seam only at that
 * implementation boundary so wide candidate snapshot rebuilds do not leak
 * TinyDBRecord's fixed ROW_SIZE limit into unrelated query code. */
#ifdef GENERIC_INDEX_CANDIDATES_H
#include "generic_index_payload_scan_shim.h"
#endif

#endif /* GENERIC_INDEX_EPOCH_H */
