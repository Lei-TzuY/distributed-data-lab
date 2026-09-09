#ifndef SCHEMA_REPACK_MIGRATION_REOPEN_H
#define SCHEMA_REPACK_MIGRATION_REOPEN_H

#include "compact_v2_migration_manifest_file.h"
#include "schema_repack_migration_recovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * File-backed reopen seam for schema-repack crash window A.
 *
 * This helper owns manifest discovery/decoding and deliberately delegates all
 * Pager/catalog mutations to the recovery callbacks.  ABSENT is a successful
 * no-op.  A present but corrupt/unsupported manifest fails closed before any
 * callback can observe page identities.  A valid manifest is accepted only by
 * tinydb_schema_repack_recover_durable_unpublished(), which in turn requires
 * the old root/generation to still be authoritative.
 *
 * Keeping this boundary independent of db_open() lets the table lifecycle wire
 * real Pager reclaim/sync operations later without duplicating the bounded
 * sidecar parser or weakening its fail-closed rules.
 */
typedef enum TinyDBSchemaRepackReopenStatus {
    TINYDB_SCHEMA_REPACK_REOPEN_NO_MANIFEST = 0,
    TINYDB_SCHEMA_REPACK_REOPEN_RECOVERED = 1,
    TINYDB_SCHEMA_REPACK_REOPEN_INVALID_MANIFEST = 2,
    TINYDB_SCHEMA_REPACK_REOPEN_IO_ERROR = 3,
    TINYDB_SCHEMA_REPACK_REOPEN_RECOVERY_FAILED = 4
} TinyDBSchemaRepackReopenStatus;

typedef struct TinyDBSchemaRepackReopenResult {
    TinyDBSchemaRepackReopenStatus status;
    uint32_t authoritative_root_page_num;
    uint64_t authoritative_schema_generation;
    uint32_t reclaimed_page_count;
    bool ready;
} TinyDBSchemaRepackReopenResult;

static inline TinyDBSchemaRepackReopenStatus
 tinydb_schema_repack_recover_file_on_reopen(
    const char* database_filename,
    const TinyDBCompactV2MigrationRecoveryOps* ops,
    TinyDBSchemaRepackReopenResult* result_out,
    char* message,
    size_t message_capacity) {
    TinyDBCompactV2MigrationManifest manifest;
    TinyDBSchemaRepackPreCatalogRecoveryResult recovery;
    uint32_t claimed_pages[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS];
    unsigned char encoded[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE];
    TinyDBCompactV2MigrationManifestLoadResult load_result;

    if (result_out != NULL) memset(result_out, 0, sizeof(*result_out));
    tinydb_compact_v2_migration_manifest_file_message(message, message_capacity, "");
    if (database_filename == NULL || database_filename[0] == '\0' ||
        ops == NULL || result_out == NULL) {
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "invalid schema repack reopen arguments");
        if (result_out != NULL) result_out->status = TINYDB_SCHEMA_REPACK_REOPEN_IO_ERROR;
        return TINYDB_SCHEMA_REPACK_REOPEN_IO_ERROR;
    }

    load_result = tinydb_compact_v2_migration_manifest_load_file(
        database_filename,
        &manifest,
        claimed_pages,
        TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS,
        encoded,
        sizeof(encoded),
        message,
        message_capacity);

    if (load_result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_ABSENT) {
        result_out->status = TINYDB_SCHEMA_REPACK_REOPEN_NO_MANIFEST;
        result_out->ready = true;
        return result_out->status;
    }
    if (load_result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_INVALID) {
        result_out->status = TINYDB_SCHEMA_REPACK_REOPEN_INVALID_MANIFEST;
        return result_out->status;
    }
    if (load_result != TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_OK) {
        result_out->status = TINYDB_SCHEMA_REPACK_REOPEN_IO_ERROR;
        return result_out->status;
    }

    if (!tinydb_schema_repack_recover_durable_unpublished(
            &manifest, ops, &recovery)) {
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity,
            "schema repack pre-catalog recovery failed");
        result_out->status = TINYDB_SCHEMA_REPACK_REOPEN_RECOVERY_FAILED;
        return result_out->status;
    }

    result_out->status = TINYDB_SCHEMA_REPACK_REOPEN_RECOVERED;
    result_out->authoritative_root_page_num = recovery.authoritative_root_page_num;
    result_out->authoritative_schema_generation = recovery.authoritative_schema_generation;
    result_out->reclaimed_page_count = recovery.reclaimed_page_count;
    result_out->ready = true;
    return result_out->status;
}

#endif /* SCHEMA_REPACK_MIGRATION_REOPEN_H */
