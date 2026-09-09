#ifndef TINYDB_COMPACT_V2_MIGRATION_CATALOG_STATE_H
#define TINYDB_COMPACT_V2_MIGRATION_CATALOG_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "pager.h"
#include "schema_catalog_generation.h"

/*
 * Immutable authoritative catalog view used by compact-V2 reopen recovery.
 *
 * A migration manifest identifies a table by stable table_id and compares an
 * old/new schema generation before deciding whether to reclaim staging pages
 * or the retired tree.  This adapter is deliberately small: it only exposes a
 * catalog state after the decoded V3 generation snapshot has been validated
 * against the decoded schema catalog, and it additionally rejects roots that
 * are outside the currently opened Pager file.
 *
 * The object borrows catalog/snapshot/pager.  Callers must keep all three
 * alive and immutable for the duration of recovery.
 */
typedef struct TinyDBCompactV2MigrationCatalogState {
    const Catalog* catalog;
    const TinyDBSchemaCatalogGenerationSnapshot* snapshot;
    const Pager* pager;
} TinyDBCompactV2MigrationCatalogState;

static inline bool tinydb_compact_v2_migration_catalog_state_is_valid(
    const TinyDBCompactV2MigrationCatalogState* state) {
    if (state == NULL || state->catalog == NULL || state->snapshot == NULL ||
        state->pager == NULL ||
        !tinydb_schema_catalog_generation_is_valid(state->catalog,
                                                   state->snapshot)) {
        return false;
    }

    for (uint32_t i = 0u; i < state->catalog->num_tables; i++) {
        if (state->catalog->schemas[i].root_page_num >= state->pager->num_pages) {
            return false;
        }
    }
    return true;
}

static inline bool tinydb_compact_v2_migration_catalog_state_read(
    void* context,
    uint32_t table_id,
    uint32_t* root_page_num_out,
    uint64_t* schema_generation_out) {
    TinyDBCompactV2MigrationCatalogState* state =
        (TinyDBCompactV2MigrationCatalogState*)context;

    if (root_page_num_out != NULL) *root_page_num_out = 0u;
    if (schema_generation_out != NULL) *schema_generation_out = 0u;
    if (root_page_num_out == NULL || schema_generation_out == NULL ||
        !tinydb_compact_v2_migration_catalog_state_is_valid(state)) {
        return false;
    }

    if (!tinydb_schema_catalog_generation_authoritative(
            state->catalog,
            state->snapshot,
            table_id,
            root_page_num_out,
            schema_generation_out)) {
        *root_page_num_out = 0u;
        *schema_generation_out = 0u;
        return false;
    }

    if (*root_page_num_out >= state->pager->num_pages) {
        *root_page_num_out = 0u;
        *schema_generation_out = 0u;
        return false;
    }
    return true;
}

#endif /* TINYDB_COMPACT_V2_MIGRATION_CATALOG_STATE_H */
