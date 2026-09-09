#ifndef TINYDB_SCHEMA_CATALOG_AUTHORITATIVE_STATE_H
#define TINYDB_SCHEMA_CATALOG_AUTHORITATIVE_STATE_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "schema_catalog_shape_codec.h"
#include "schema_catalog_v3_store.h"

/*
 * Reconstruct the catalog identity/generation state that is authoritative for
 * database-open recovery.
 *
 * multitable_catalog_load() already validates and publishes table->catalog.  A
 * V3 catalog additionally persists stable table ids and schema generations in
 * the same checksummed envelope.  Reopen recovery must consume those persisted
 * values rather than bootstrap generation 1 again.  Legacy/no-sidecar catalogs
 * do not have identity metadata, so their deterministic slot-based bootstrap is
 * still the only valid state until the next V3 save.
 *
 * The helper deliberately cross-checks the schema shape decoded from the V3
 * envelope against the catalog that the production loader actually published.
 * Comparing canonical encoded shape bytes avoids relying on C struct padding.
 */

#define TINYDB_SCHEMA_CATALOG_AUTHORITATIVE_PATH_MAX 768u

static inline bool tinydb_schema_catalog_authoritative_build_path(
    const char* database_filename,
    char* path,
    size_t path_capacity) {
    int written;
    if (database_filename == NULL || database_filename[0] == '\0' ||
        path == NULL || path_capacity == 0u) {
        return false;
    }
    written = snprintf(path, path_capacity, "%s.schema", database_filename);
    return written >= 0 && (size_t)written < path_capacity;
}

static inline bool tinydb_schema_catalog_authoritative_file_is_v3(
    const char* path,
    bool* exists_out,
    bool* is_v3_out) {
    FILE* file;
    unsigned char prefix[8];
    size_t count;
    bool io_error;

    if (exists_out != NULL) *exists_out = false;
    if (is_v3_out != NULL) *is_v3_out = false;
    if (path == NULL || exists_out == NULL || is_v3_out == NULL) return false;

    file = fopen(path, "rb");
    if (file == NULL) return errno == ENOENT;
    *exists_out = true;
    count = fread(prefix, 1u, sizeof(prefix), file);
    io_error = ferror(file) != 0;
    if (fclose(file) != 0) io_error = true;
    if (io_error) return false;

    if (count == sizeof(prefix) &&
        tinydb_schema_catalog_v3_get_u32(prefix) ==
            TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAGIC &&
        tinydb_schema_catalog_v3_get_u32(prefix + 4u) ==
            TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_VERSION) {
        *is_v3_out = true;
    }
    return true;
}

static inline bool tinydb_schema_catalog_authoritative_shapes_equal(
    const Catalog* left,
    const Catalog* right) {
    unsigned char left_shape[TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE];
    unsigned char right_shape[TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE];
    size_t left_size = 0u;
    size_t right_size = 0u;

    if (!tinydb_schema_catalog_shape_encode(
            left, left_shape, sizeof(left_shape), &left_size) ||
        !tinydb_schema_catalog_shape_encode(
            right, right_shape, sizeof(right_shape), &right_size)) {
        return false;
    }
    return left_size == right_size && memcmp(left_shape, right_shape, left_size) == 0;
}

static inline bool tinydb_schema_catalog_load_authoritative_generation(
    const Table* table,
    const char* database_filename,
    TinyDBSchemaCatalogGenerationSnapshot* snapshot_out) {
    char path[TINYDB_SCHEMA_CATALOG_AUTHORITATIVE_PATH_MAX];
    bool exists = false;
    bool is_v3 = false;
    unsigned char envelope[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    size_t envelope_size = 0u;
    TinyDBSchemaCatalogV3EnvelopeView view;
    Catalog decoded_catalog;
    TinyDBSchemaCatalogGenerationSnapshot decoded_snapshot;

    tinydb_schema_catalog_generation_zero(snapshot_out);
    if (table == NULL || table->pager == NULL || snapshot_out == NULL ||
        !tinydb_schema_catalog_authoritative_build_path(
            database_filename, path, sizeof(path)) ||
        !tinydb_schema_catalog_authoritative_file_is_v3(
            path, &exists, &is_v3)) {
        return false;
    }

    if (!exists || !is_v3) {
        return tinydb_schema_catalog_generation_bootstrap_legacy(
            &table->catalog, snapshot_out);
    }

    if (tinydb_schema_catalog_v3_store_read(
            path,
            false,
            envelope,
            sizeof(envelope),
            &envelope_size) != TINYDB_SCHEMA_CATALOG_V3_STORE_READ_OK) {
        return false;
    }

    memset(&decoded_catalog, 0, sizeof(decoded_catalog));
    tinydb_schema_catalog_generation_zero(&decoded_snapshot);
    tinydb_schema_catalog_v3_envelope_zero_view(&view);
    if (tinydb_schema_catalog_v3_envelope_decode(
            envelope, envelope_size, &view) !=
            TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK ||
        !tinydb_schema_catalog_shape_decode(
            view.shape, view.shape_size, &decoded_catalog) ||
        tinydb_schema_catalog_v3_decode(
            &decoded_catalog,
            view.identity,
            view.identity_size,
            &decoded_snapshot) != TINYDB_SCHEMA_CATALOG_V3_DECODE_OK ||
        !tinydb_schema_catalog_authoritative_shapes_equal(
            &table->catalog, &decoded_catalog) ||
        !tinydb_schema_catalog_generation_is_valid(
            &table->catalog, &decoded_snapshot)) {
        return false;
    }

    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (table->catalog.schemas[i].root_page_num >= table->pager->num_pages) {
            return false;
        }
    }

    *snapshot_out = decoded_snapshot;
    return true;
}

#endif /* TINYDB_SCHEMA_CATALOG_AUTHORITATIVE_STATE_H */
