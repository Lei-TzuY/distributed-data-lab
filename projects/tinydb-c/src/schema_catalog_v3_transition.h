#ifndef TINYDB_SCHEMA_CATALOG_V3_TRANSITION_H
#define TINYDB_SCHEMA_CATALOG_V3_TRANSITION_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "schema_catalog_generation.h"

/*
 * Derive the next durable table identity/schema-generation snapshot from the
 * previously published V3 catalog and the Catalog that is about to be saved.
 *
 * This deliberately avoids changing the historical Table/TableSchema ABI.  The
 * production writer can reconstruct authoritative generation state from the
 * last durable V3 envelope, then atomically publish the current shape plus this
 * derived snapshot in one new envelope.
 *
 * Supported catalog evolution is append-only for table slots.  Existing slots
 * keep their stable table_id even across rename.  A root replacement or a
 * physical row-schema change advances schema_generation exactly once.  New
 * appended tables receive fresh monotonically increasing ids and generation 1.
 * Reorder/drop/invalid previous metadata fails closed.
 */

static inline bool tinydb_schema_catalog_v3_same_column(
    const TableColumn* a,
    const TableColumn* b) {
    return a != NULL && b != NULL &&
           strncmp(a->name, b->name, MAX_NAME_SIZE) == 0 &&
           a->type == b->type &&
           a->size == b->size &&
           a->offset == b->offset;
}

static inline bool tinydb_schema_catalog_v3_same_physical_schema(
    const TableSchema* a,
    const TableSchema* b) {
    if (a == NULL || b == NULL ||
        a->num_columns != b->num_columns ||
        a->row_size != b->row_size ||
        a->has_fk != b->has_fk ||
        a->fk_on_delete_cascade != b->fk_on_delete_cascade) {
        return false;
    }
    if (a->has_fk &&
        (strncmp(a->fk_col, b->fk_col, MAX_NAME_SIZE) != 0 ||
         strncmp(a->fk_parent_table, b->fk_parent_table, MAX_NAME_SIZE) != 0 ||
         strncmp(a->fk_parent_col, b->fk_parent_col, MAX_NAME_SIZE) != 0)) {
        return false;
    }
    for (uint32_t i = 0u; i < a->num_columns; i++) {
        if (!tinydb_schema_catalog_v3_same_column(&a->columns[i], &b->columns[i])) {
            return false;
        }
    }
    return true;
}

static inline uint32_t tinydb_schema_catalog_v3_snapshot_max_table_id(
    const TinyDBSchemaCatalogGenerationSnapshot* snapshot) {
    if (snapshot == NULL || snapshot->num_tables > MAX_TABLES) return 0u;
    uint32_t max_id = 0u;
    for (uint32_t i = 0u; i < snapshot->num_tables; i++) {
        if (snapshot->entries[i].table_id > max_id) {
            max_id = snapshot->entries[i].table_id;
        }
    }
    return max_id;
}

static inline bool tinydb_schema_catalog_v3_derive_next_snapshot(
    const Catalog* previous_catalog,
    const TinyDBSchemaCatalogGenerationSnapshot* previous_snapshot,
    const Catalog* current_catalog,
    TinyDBSchemaCatalogGenerationSnapshot* next_snapshot) {
    tinydb_schema_catalog_generation_zero(next_snapshot);
    if (previous_catalog == NULL || previous_snapshot == NULL ||
        current_catalog == NULL || next_snapshot == NULL ||
        !tinydb_schema_catalog_generation_is_valid(previous_catalog,
                                                    previous_snapshot) ||
        current_catalog->num_tables < previous_catalog->num_tables ||
        current_catalog->num_tables == 0u ||
        current_catalog->num_tables > MAX_TABLES) {
        return false;
    }

    TinyDBSchemaCatalogGenerationSnapshot candidate = *previous_snapshot;
    const uint32_t old_count = previous_catalog->num_tables;
    for (uint32_t i = 0u; i < old_count; i++) {
        TinyDBSchemaCatalogGenerationEntry* entry = &candidate.entries[i];
        if (entry->root_page_num != previous_catalog->schemas[i].root_page_num ||
            entry->schema_generation == 0u) {
            return false;
        }

        const bool changed =
            previous_catalog->schemas[i].root_page_num !=
                current_catalog->schemas[i].root_page_num ||
            !tinydb_schema_catalog_v3_same_physical_schema(
                &previous_catalog->schemas[i], &current_catalog->schemas[i]);
        if (changed) {
            if (entry->schema_generation == UINT64_MAX) return false;
            entry->schema_generation++;
        }
        entry->root_page_num = current_catalog->schemas[i].root_page_num;
    }

    uint32_t next_id = tinydb_schema_catalog_v3_snapshot_max_table_id(
        previous_snapshot);
    for (uint32_t i = old_count; i < current_catalog->num_tables; i++) {
        if (next_id == UINT32_MAX) return false;
        next_id++;
        candidate.entries[i].table_id = next_id;
        candidate.entries[i].root_page_num = current_catalog->schemas[i].root_page_num;
        candidate.entries[i].schema_generation = TINYDB_SCHEMA_GENERATION_INITIAL;
    }
    candidate.num_tables = current_catalog->num_tables;

    if (!tinydb_schema_catalog_generation_is_valid(current_catalog, &candidate)) {
        return false;
    }
    *next_snapshot = candidate;
    return true;
}

#endif /* TINYDB_SCHEMA_CATALOG_V3_TRANSITION_H */
