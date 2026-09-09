#ifndef TINYDB_SCHEMA_CATALOG_V3_RUNTIME_H
#define TINYDB_SCHEMA_CATALOG_V3_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "schema_catalog_v3_envelope.h"

/*
 * Runtime bridge between the production Catalog and the durable V3
 * identity/generation envelope.
 *
 * The production V1/V2 catalog still stores only schema shape.  Callers that
 * load one of those formats bootstrap deterministic identities once, then the
 * next successful V3 save persists this state.  A V3 reopen restores the
 * persisted identities/generations and cross-checks every root against the
 * decoded schema shape before making the runtime state authoritative.
 *
 * This object is intentionally separate from Table for now: adding it does not
 * change the historical Table/TableSchema ABI or the V1 struct-dump decoder.
 * The next integration step can embed/own one instance beside the catalog in
 * db_open without changing any on-disk V1 representation.
 */

typedef struct TinyDBSchemaCatalogV3Runtime {
    TinyDBSchemaCatalogGenerationSnapshot generation;
    bool authoritative_v3;
} TinyDBSchemaCatalogV3Runtime;

static inline void tinydb_schema_catalog_v3_runtime_zero(
    TinyDBSchemaCatalogV3Runtime* runtime) {
    if (runtime != NULL) memset(runtime, 0, sizeof(*runtime));
}

static inline bool tinydb_schema_catalog_v3_runtime_is_valid(
    const Catalog* catalog,
    const TinyDBSchemaCatalogV3Runtime* runtime) {
    return catalog != NULL && runtime != NULL &&
           tinydb_schema_catalog_generation_is_valid(catalog,
                                                      &runtime->generation);
}

static inline bool tinydb_schema_catalog_v3_runtime_bootstrap_legacy(
    const Catalog* catalog,
    TinyDBSchemaCatalogV3Runtime* runtime) {
    tinydb_schema_catalog_v3_runtime_zero(runtime);
    if (runtime == NULL ||
        !tinydb_schema_catalog_generation_bootstrap_legacy(
            catalog, &runtime->generation)) {
        return false;
    }
    runtime->authoritative_v3 = false;
    return true;
}

static inline TinyDBSchemaCatalogV3EnvelopeDecodeResult
 tinydb_schema_catalog_v3_runtime_restore(
    const Catalog* decoded_shape_catalog,
    const unsigned char* envelope,
    size_t envelope_size,
    TinyDBSchemaCatalogV3Runtime* runtime,
    TinyDBSchemaCatalogV3EnvelopeView* view_out) {
    tinydb_schema_catalog_v3_runtime_zero(runtime);
    if (runtime == NULL) {
        return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID;
    }

    TinyDBSchemaCatalogGenerationSnapshot decoded;
    tinydb_schema_catalog_generation_zero(&decoded);
    TinyDBSchemaCatalogV3EnvelopeView view;
    tinydb_schema_catalog_v3_envelope_zero_view(&view);
    TinyDBSchemaCatalogV3EnvelopeDecodeResult result =
        tinydb_schema_catalog_v3_envelope_decode_identity(
            decoded_shape_catalog,
            envelope,
            envelope_size,
            &decoded,
            &view);
    if (result != TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK) {
        return result;
    }

    runtime->generation = decoded;
    runtime->authoritative_v3 = true;
    if (view_out != NULL) *view_out = view;
    return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK;
}

static inline bool tinydb_schema_catalog_v3_runtime_encode_envelope(
    const Catalog* catalog,
    const TinyDBSchemaCatalogV3Runtime* runtime,
    const unsigned char* shape,
    size_t shape_size,
    unsigned char* identity_workspace,
    size_t identity_capacity,
    unsigned char* envelope_out,
    size_t envelope_capacity,
    size_t* envelope_size_out) {
    if (envelope_size_out != NULL) *envelope_size_out = 0u;
    if (!tinydb_schema_catalog_v3_runtime_is_valid(catalog, runtime) ||
        shape == NULL || shape_size == 0u || identity_workspace == NULL ||
        envelope_out == NULL || envelope_size_out == NULL) {
        return false;
    }

    size_t identity_size = 0u;
    if (!tinydb_schema_catalog_v3_encode(catalog,
                                         &runtime->generation,
                                         identity_workspace,
                                         identity_capacity,
                                         &identity_size)) {
        return false;
    }
    return tinydb_schema_catalog_v3_envelope_encode(shape,
                                                     shape_size,
                                                     identity_workspace,
                                                     identity_size,
                                                     envelope_out,
                                                     envelope_capacity,
                                                     envelope_size_out);
}

static inline uint32_t tinydb_schema_catalog_v3_runtime_max_table_id(
    const TinyDBSchemaCatalogV3Runtime* runtime) {
    if (runtime == NULL || runtime->generation.num_tables > MAX_TABLES) return 0u;
    uint32_t max_id = 0u;
    for (uint32_t i = 0u; i < runtime->generation.num_tables; i++) {
        if (runtime->generation.entries[i].table_id > max_id) {
            max_id = runtime->generation.entries[i].table_id;
        }
    }
    return max_id;
}

/*
 * Reconcile append-only CREATE TABLE with the runtime identity set.
 * Existing slots must retain exactly the same root identity.  This refuses to
 * silently reinterpret a reorder, drop, or root replacement as table creation;
 * those operations require an explicit publication path.  Newly appended
 * tables receive fresh monotonically increasing non-zero ids and generation 1.
 */
static inline bool tinydb_schema_catalog_v3_runtime_reconcile_appended_tables(
    const Catalog* catalog,
    TinyDBSchemaCatalogV3Runtime* runtime) {
    if (catalog == NULL || runtime == NULL || catalog->num_tables == 0u ||
        catalog->num_tables > MAX_TABLES ||
        runtime->generation.num_tables == 0u ||
        runtime->generation.num_tables > catalog->num_tables) {
        return false;
    }

    const uint32_t old_count = runtime->generation.num_tables;
    for (uint32_t i = 0u; i < old_count; i++) {
        const TinyDBSchemaCatalogGenerationEntry* entry =
            &runtime->generation.entries[i];
        if (entry->table_id == 0u || entry->schema_generation == 0u ||
            entry->root_page_num != catalog->schemas[i].root_page_num) {
            return false;
        }
    }

    uint32_t next_id = tinydb_schema_catalog_v3_runtime_max_table_id(runtime);
    if (next_id == UINT32_MAX && catalog->num_tables > old_count) return false;

    TinyDBSchemaCatalogV3Runtime candidate = *runtime;
    for (uint32_t i = old_count; i < catalog->num_tables; i++) {
        if (next_id == UINT32_MAX) return false;
        next_id++;
        candidate.generation.entries[i].table_id = next_id;
        candidate.generation.entries[i].root_page_num =
            catalog->schemas[i].root_page_num;
        candidate.generation.entries[i].schema_generation =
            TINYDB_SCHEMA_GENERATION_INITIAL;
        candidate.generation.num_tables = i + 1u;
    }

    if (!tinydb_schema_catalog_generation_is_valid(catalog,
                                                    &candidate.generation)) {
        return false;
    }
    *runtime = candidate;
    return true;
}

/*
 * Publish a schema change in memory with optimistic old-root/generation
 * matching.  Unlike the lower-level replacement helper this also supports a
 * shape-only change that deliberately keeps the same root: generation still
 * advances because the schema interpretation changed.
 */
static inline bool tinydb_schema_catalog_v3_runtime_publish_schema_change(
    Catalog* catalog,
    TinyDBSchemaCatalogV3Runtime* runtime,
    uint32_t table_id,
    uint32_t expected_old_root_page_num,
    uint64_t expected_old_generation,
    uint32_t new_root_page_num) {
    if (!tinydb_schema_catalog_v3_runtime_is_valid(catalog, runtime) ||
        table_id == 0u || expected_old_generation == 0u ||
        expected_old_generation == UINT64_MAX) {
        return false;
    }

    TinyDBSchemaCatalogV3Runtime candidate = *runtime;
    Catalog catalog_candidate = *catalog;
    for (uint32_t i = 0u; i < candidate.generation.num_tables; i++) {
        TinyDBSchemaCatalogGenerationEntry* entry =
            &candidate.generation.entries[i];
        if (entry->table_id != table_id) continue;
        if (entry->root_page_num != expected_old_root_page_num ||
            entry->schema_generation != expected_old_generation ||
            catalog_candidate.schemas[i].root_page_num !=
                expected_old_root_page_num) {
            return false;
        }

        catalog_candidate.schemas[i].root_page_num = new_root_page_num;
        entry->root_page_num = new_root_page_num;
        entry->schema_generation = expected_old_generation + UINT64_C(1);
        if (!tinydb_schema_catalog_generation_is_valid(
                &catalog_candidate, &candidate.generation)) {
            return false;
        }
        *catalog = catalog_candidate;
        *runtime = candidate;
        return true;
    }
    return false;
}

static inline bool tinydb_schema_catalog_v3_runtime_authoritative(
    const Catalog* catalog,
    const TinyDBSchemaCatalogV3Runtime* runtime,
    uint32_t table_id,
    uint32_t* root_page_num_out,
    uint64_t* schema_generation_out) {
    if (runtime == NULL) {
        if (root_page_num_out != NULL) *root_page_num_out = 0u;
        if (schema_generation_out != NULL) *schema_generation_out = 0u;
        return false;
    }
    return tinydb_schema_catalog_generation_authoritative(
        catalog,
        &runtime->generation,
        table_id,
        root_page_num_out,
        schema_generation_out);
}

#endif /* TINYDB_SCHEMA_CATALOG_V3_RUNTIME_H */