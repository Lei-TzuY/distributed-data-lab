#ifndef TINYDB_SCHEMA_REPACK_CATALOG_V3_PUBLISH_H
#define TINYDB_SCHEMA_REPACK_CATALOG_V3_PUBLISH_H

#include "schema_catalog_authoritative_state.h"
#include "schema_repack_table_catalog_publish.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Production V3 catalog adapter for schema-repack publication.
 *
 * A schema-repack changes both the physical row interpretation and the tree
 * root. Publishing only the new root would make reopen decode the new payloads
 * with the old offsets/sizes. The durable unit here is therefore one V3
 * envelope containing:
 *
 *   destination TableSchema + staged root + advanced schema generation.
 *
 * Before building that envelope, the adapter reloads the authoritative
 * root/generation state and requires an exact old-state match. The synced V3
 * WAL commit marker is the durable publication boundary; after it succeeds the
 * in-memory Catalog is advanced even if the later main-file copy is left for
 * reopen recovery.
 *
 * TinyDB currently has a single-writer catalog path. This adapter preserves
 * that model; it provides optimistic stale-state rejection against the durable
 * catalog but is not a cross-process file-locking primitive.
 */
typedef struct TinyDBSchemaRepackCatalogV3PublishContext {
    Table* table;
    const char* database_filename;
    const TableSchema* destination_schema;
} TinyDBSchemaRepackCatalogV3PublishContext;

static inline bool tinydb_schema_repack_catalog_v3_build_paths(
    const char* database_filename,
    char* main_path,
    size_t main_capacity,
    char* wal_path,
    size_t wal_capacity) {
    int main_written;
    int wal_written;
    if (database_filename == NULL || database_filename[0] == '\0' ||
        main_path == NULL || wal_path == NULL ||
        main_capacity == 0u || wal_capacity == 0u) {
        return false;
    }
    main_written = snprintf(main_path, main_capacity, "%s.schema", database_filename);
    wal_written = snprintf(wal_path, wal_capacity, "%s.schema.wal", database_filename);
    return main_written >= 0 && wal_written >= 0 &&
           (size_t)main_written < main_capacity &&
           (size_t)wal_written < wal_capacity;
}

static inline bool tinydb_schema_repack_catalog_v3_publish_durable(
    void* opaque_context,
    uint32_t table_id,
    uint32_t expected_old_root_page_num,
    uint64_t expected_old_schema_generation,
    uint32_t new_root_page_num,
    uint64_t new_schema_generation) {
    TinyDBSchemaRepackCatalogV3PublishContext* context =
        (TinyDBSchemaRepackCatalogV3PublishContext*)opaque_context;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    TinyDBSchemaCatalogGenerationSnapshot candidate_snapshot;
    Catalog candidate_catalog;
    uint32_t slot = UINT32_MAX;
    unsigned char shape[TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE];
    unsigned char identity[TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE];
    unsigned char envelope[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE];
    size_t shape_size = 0u;
    size_t identity_size = 0u;
    size_t envelope_size = 0u;
    char main_path[TINYDB_SCHEMA_CATALOG_AUTHORITATIVE_PATH_MAX];
    char wal_path[TINYDB_SCHEMA_CATALOG_AUTHORITATIVE_PATH_MAX];
    TinyDBSchemaCatalogV3StorePublishResult publish_result;

    tinydb_schema_catalog_generation_zero(&snapshot);
    tinydb_schema_catalog_generation_zero(&candidate_snapshot);
    tinydb_schema_catalog_v3_store_publish_result_zero(&publish_result);
    memset(&candidate_catalog, 0, sizeof(candidate_catalog));

    if (context == NULL || context->table == NULL ||
        context->table->pager == NULL || context->database_filename == NULL ||
        context->destination_schema == NULL || table_id == 0u ||
        expected_old_root_page_num == 0u || new_root_page_num == 0u ||
        expected_old_schema_generation == 0u ||
        expected_old_schema_generation == UINT64_MAX ||
        new_schema_generation != expected_old_schema_generation + UINT64_C(1) ||
        !tinydb_schema_catalog_load_authoritative_generation(
            context->table, context->database_filename, &snapshot)) {
        return false;
    }

    for (uint32_t i = 0u; i < snapshot.num_tables; i++) {
        const TinyDBSchemaCatalogGenerationEntry* entry = &snapshot.entries[i];
        if (entry->table_id != table_id) continue;
        if (entry->root_page_num != expected_old_root_page_num ||
            entry->schema_generation != expected_old_schema_generation ||
            i >= context->table->catalog.num_tables ||
            context->table->catalog.schemas[i].root_page_num !=
                expected_old_root_page_num ||
            strncmp(context->destination_schema->name,
                    context->table->catalog.schemas[i].name,
                    MAX_NAME_SIZE) != 0) {
            return false;
        }
        slot = i;
        break;
    }
    if (slot == UINT32_MAX) return false;

    candidate_catalog = context->table->catalog;
    candidate_snapshot = snapshot;
    candidate_catalog.schemas[slot] = *context->destination_schema;
    candidate_catalog.schemas[slot].root_page_num = new_root_page_num;
    candidate_snapshot.entries[slot].root_page_num = new_root_page_num;
    candidate_snapshot.entries[slot].schema_generation = new_schema_generation;

    if (!tinydb_schema_catalog_shape_valid(&candidate_catalog) ||
        !tinydb_schema_catalog_generation_is_valid(&candidate_catalog,
                                                    &candidate_snapshot) ||
        !tinydb_schema_catalog_shape_encode(&candidate_catalog,
                                            shape,
                                            sizeof(shape),
                                            &shape_size) ||
        !tinydb_schema_catalog_v3_encode(&candidate_catalog,
                                         &candidate_snapshot,
                                         identity,
                                         sizeof(identity),
                                         &identity_size) ||
        !tinydb_schema_catalog_v3_envelope_encode(shape,
                                                   shape_size,
                                                   identity,
                                                   identity_size,
                                                   envelope,
                                                   sizeof(envelope),
                                                   &envelope_size) ||
        !tinydb_schema_repack_catalog_v3_build_paths(
            context->database_filename,
            main_path,
            sizeof(main_path),
            wal_path,
            sizeof(wal_path)) ||
        !tinydb_schema_catalog_v3_store_publish_detailed(main_path,
                                                         wal_path,
                                                         envelope,
                                                         envelope_size,
                                                         &publish_result) ||
        !publish_result.wal_committed_durable) {
        return false;
    }

    /* The WAL commit is authoritative even if main copy/cleanup is pending. */
    context->table->catalog = candidate_catalog;
    return true;
}

static inline bool tinydb_schema_repack_catalog_v3_publish_ops_init(
    TinyDBSchemaRepackCatalogV3PublishContext* context,
    TinyDBSchemaRepackCatalogPublishOps* ops) {
    if (ops != NULL) memset(ops, 0, sizeof(*ops));
    if (context == NULL || context->table == NULL ||
        context->database_filename == NULL ||
        context->destination_schema == NULL || ops == NULL) {
        return false;
    }
    ops->context = context;
    ops->publish_catalog_durable =
        tinydb_schema_repack_catalog_v3_publish_durable;
    return true;
}

#endif /* TINYDB_SCHEMA_REPACK_CATALOG_V3_PUBLISH_H */
