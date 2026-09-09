#ifndef GENERIC_INDEX_COST_H
#define GENERIC_INDEX_COST_H

#include <stdint.h>

#define TINYDB_GENERIC_COST_SCAN_ROW 3u
#define TINYDB_GENERIC_COST_INDEX_ENTRY 1u
#define TINYDB_GENERIC_COST_RANDOM_FETCH 4u

static inline uint64_t tinydb_generic_scan_cost(uint32_t table_rows) {
    return (uint64_t)table_rows * TINYDB_GENERIC_COST_SCAN_ROW;
}

static inline uint64_t tinydb_generic_anchor_cost(uint32_t candidate_rows) {
    return (uint64_t)candidate_rows *
           (TINYDB_GENERIC_COST_INDEX_ENTRY + TINYDB_GENERIC_COST_RANDOM_FETCH);
}

static inline uint64_t tinydb_generic_intersection_cost(
    const uint32_t* source_rows,
    uint32_t source_count,
    uint32_t table_rows,
    uint32_t* estimated_rows) {
    uint64_t estimate = table_rows;
    uint64_t cost = 0;

    if (estimated_rows != 0) *estimated_rows = 0;
    if (source_rows == 0 || source_count == 0 || table_rows == 0) return 0;

    for (uint32_t i = 0; i < source_count; i++) {
        cost += (uint64_t)source_rows[i] * TINYDB_GENERIC_COST_INDEX_ENTRY;
        estimate = (estimate * source_rows[i]) / table_rows;
    }
    if (estimate > UINT32_MAX) estimate = UINT32_MAX;
    if (estimated_rows != 0) *estimated_rows = (uint32_t)estimate;
    cost += estimate * TINYDB_GENERIC_COST_RANDOM_FETCH;
    return cost;
}

#endif /* GENERIC_INDEX_COST_H */
