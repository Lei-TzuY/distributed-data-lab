#ifndef TINYDB_SCHEMA_CATALOG_GENERATION_H
#define TINYDB_SCHEMA_CATALOG_GENERATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "table.h"

/*
 * Stable migration identity/generation contract for schema catalogs.
 *
 * Existing schema catalog V1/V2 payloads do not persist these fields yet, so
 * this helper deliberately separates the logical contract from the on-disk
 * encoding.  A catalog slot is assigned a non-zero table id once and keeps it
 * across rename/root replacement; schema generation starts at 1 and advances
 * only when a replacement root/schema is atomically published.
 *
 * Callers must persist the identities/generations in the catalog before using
 * them as authoritative reopen state.  The helper is intentionally fail-closed
 * and does not infer generation from a page number or table name.
 */

#define TINYDB_SCHEMA_GENERATION_INITIAL UINT64_C(1)

typedef struct TinyDBSchemaCatalogGenerationEntry {
    uint32_t table_id;
    uint32_t root_page_num;
    uint64_t schema_generation;
} TinyDBSchemaCatalogGenerationEntry;

typedef struct TinyDBSchemaCatalogGenerationSnapshot {
    uint32_t num_tables;
    TinyDBSchemaCatalogGenerationEntry entries[MAX_TABLES];
} TinyDBSchemaCatalogGenerationSnapshot;

static inline void tinydb_schema_catalog_generation_zero(
    TinyDBSchemaCatalogGenerationSnapshot* snapshot) {
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
}

static inline bool tinydb_schema_catalog_generation_is_valid(
    const Catalog* catalog,
    const TinyDBSchemaCatalogGenerationSnapshot* snapshot) {
    if (catalog == NULL || snapshot == NULL ||
        catalog->num_tables == 0u || catalog->num_tables > MAX_TABLES ||
        snapshot->num_tables != catalog->num_tables) {
        return false;
    }

    for (uint32_t i = 0u; i < snapshot->num_tables; i++) {
        const TinyDBSchemaCatalogGenerationEntry* entry = &snapshot->entries[i];
        if (entry->table_id == 0u || entry->schema_generation == 0u ||
            entry->root_page_num != catalog->schemas[i].root_page_num) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (entry->table_id == snapshot->entries[j].table_id) return false;
        }
    }
    return true;
}

static inline bool tinydb_schema_catalog_generation_bootstrap_legacy(
    const Catalog* catalog,
    TinyDBSchemaCatalogGenerationSnapshot* snapshot) {
    tinydb_schema_catalog_generation_zero(snapshot);
    if (catalog == NULL || snapshot == NULL || catalog->num_tables == 0u ||
        catalog->num_tables > MAX_TABLES) {
        return false;
    }

    snapshot->num_tables = catalog->num_tables;
    for (uint32_t i = 0u; i < catalog->num_tables; i++) {
        /* Catalog order is durable in existing V1/V2 snapshots.  Using the
         * one-based slot as the migration id gives legacy catalogs a stable,
         * deterministic bootstrap without depending on mutable table names. */
        snapshot->entries[i].table_id = i + 1u;
        snapshot->entries[i].root_page_num = catalog->schemas[i].root_page_num;
        snapshot->entries[i].schema_generation = TINYDB_SCHEMA_GENERATION_INITIAL;
    }
    return tinydb_schema_catalog_generation_is_valid(catalog, snapshot);
}

static inline const TinyDBSchemaCatalogGenerationEntry*
tinydb_schema_catalog_generation_find(
    const TinyDBSchemaCatalogGenerationSnapshot* snapshot,
    uint32_t table_id) {
    if (snapshot == NULL || table_id == 0u || snapshot->num_tables > MAX_TABLES) {
        return NULL;
    }
    for (uint32_t i = 0u; i < snapshot->num_tables; i++) {
        if (snapshot->entries[i].table_id == table_id) return &snapshot->entries[i];
    }
    return NULL;
}

static inline bool tinydb_schema_catalog_generation_authoritative(
    const Catalog* catalog,
    const TinyDBSchemaCatalogGenerationSnapshot* snapshot,
    uint32_t table_id,
    uint32_t* root_page_num_out,
    uint64_t* schema_generation_out) {
    if (root_page_num_out != NULL) *root_page_num_out = 0u;
    if (schema_generation_out != NULL) *schema_generation_out = 0u;
    if (root_page_num_out == NULL || schema_generation_out == NULL ||
        !tinydb_schema_catalog_generation_is_valid(catalog, snapshot)) {
        return false;
    }

    const TinyDBSchemaCatalogGenerationEntry* entry =
        tinydb_schema_catalog_generation_find(snapshot, table_id);
    if (entry == NULL) return false;
    *root_page_num_out = entry->root_page_num;
    *schema_generation_out = entry->schema_generation;
    return true;
}

static inline bool tinydb_schema_catalog_generation_publish_replacement(
    Catalog* catalog,
    TinyDBSchemaCatalogGenerationSnapshot* snapshot,
    uint32_t table_id,
    uint32_t expected_old_root_page_num,
    uint64_t expected_old_generation,
    uint32_t new_root_page_num) {
    if (!tinydb_schema_catalog_generation_is_valid(catalog, snapshot) ||
        table_id == 0u || new_root_page_num == expected_old_root_page_num ||
        expected_old_generation == 0u || expected_old_generation == UINT64_MAX) {
        return false;
    }

    for (uint32_t i = 0u; i < snapshot->num_tables; i++) {
        TinyDBSchemaCatalogGenerationEntry* entry = &snapshot->entries[i];
        if (entry->table_id != table_id) continue;
        if (entry->root_page_num != expected_old_root_page_num ||
            entry->schema_generation != expected_old_generation ||
            catalog->schemas[i].root_page_num != expected_old_root_page_num) {
            return false;
        }
        /* Publish root and generation together in memory.  Persistence code
         * must serialize both in the same catalog durability boundary. */
        catalog->schemas[i].root_page_num = new_root_page_num;
        entry->root_page_num = new_root_page_num;
        entry->schema_generation = expected_old_generation + UINT64_C(1);
        return true;
    }
    return false;
}

#endif /* TINYDB_SCHEMA_CATALOG_GENERATION_H */
