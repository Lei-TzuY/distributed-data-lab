#ifndef COMPACT_V2_MIGRATION_MANIFEST_FILE_H
#define COMPACT_V2_MIGRATION_MANIFEST_FILE_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compact_v2_migration_manifest.h"

#define TINYDB_COMPACT_V2_MIGRATION_MANIFEST_SUFFIX ".compact-v2-migration"
#define TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX ((size_t)1024u)
#define TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE \
    (TINYDB_COMPACT_V2_MIGRATION_MANIFEST_FIXED_SIZE + \
     ((size_t)TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS * sizeof(uint32_t)) + \
     TINYDB_COMPACT_V2_MIGRATION_MANIFEST_CHECKSUM_SIZE)

typedef enum TinyDBCompactV2MigrationManifestLoadResult {
    TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_ABSENT = 0,
    TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_OK = 1,
    TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_INVALID = 2,
    TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_IO_ERROR = 3
} TinyDBCompactV2MigrationManifestLoadResult;

static inline void tinydb_compact_v2_migration_manifest_file_message(
    char* message,
    size_t message_capacity,
    const char* text) {
    if (message == NULL || message_capacity == 0u) return;
    if (text == NULL) text = "";
    (void)snprintf(message, message_capacity, "%s", text);
}

static inline bool tinydb_compact_v2_migration_manifest_file_path(
    const char* database_filename,
    char* path_out,
    size_t path_capacity) {
    size_t database_length;
    size_t suffix_length;
    if (path_out != NULL && path_capacity != 0u) path_out[0] = '\0';
    if (database_filename == NULL || database_filename[0] == '\0' ||
        path_out == NULL || path_capacity == 0u) {
        return false;
    }
    database_length = strlen(database_filename);
    suffix_length = strlen(TINYDB_COMPACT_V2_MIGRATION_MANIFEST_SUFFIX);
    if (database_length > SIZE_MAX - suffix_length - 1u ||
        database_length + suffix_length + 1u > path_capacity) {
        return false;
    }
    memcpy(path_out, database_filename, database_length);
    memcpy(path_out + database_length,
           TINYDB_COMPACT_V2_MIGRATION_MANIFEST_SUFFIX,
           suffix_length + 1u);
    return true;
}

/*
 * Load the active compact-V2 migration sidecar without trusting any of its
 * contents until the complete bounded file has been read and decoded.
 *
 * ABSENT is the only non-error no-op result.  A present but truncated,
 * oversized, checksummed-corrupt, or semantically invalid sidecar returns
 * INVALID so database-open recovery can fail closed rather than silently
 * ignoring evidence of an interrupted migration.
 *
 * The decoded manifest borrows claimed_pages_out.  Callers must keep that
 * array alive for as long as they use manifest_out.
 */
static inline TinyDBCompactV2MigrationManifestLoadResult
 tinydb_compact_v2_migration_manifest_load_file(
    const char* database_filename,
    TinyDBCompactV2MigrationManifest* manifest_out,
    uint32_t* claimed_pages_out,
    uint32_t claimed_pages_capacity,
    unsigned char* encoded_scratch,
    size_t encoded_scratch_capacity,
    char* message,
    size_t message_capacity) {
    char path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    FILE* file = NULL;
    long length_long;
    size_t length;
    size_t bytes_read;

    if (manifest_out != NULL) memset(manifest_out, 0, sizeof(*manifest_out));
    tinydb_compact_v2_migration_manifest_file_message(message, message_capacity, "");
    if (manifest_out == NULL || claimed_pages_out == NULL ||
        claimed_pages_capacity == 0u || encoded_scratch == NULL ||
        encoded_scratch_capacity == 0u ||
        !tinydb_compact_v2_migration_manifest_file_path(
            database_filename, path, sizeof(path))) {
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "invalid migration manifest load arguments");
        return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_IO_ERROR;
    }

    errno = 0;
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_ABSENT;
        }
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "migration manifest could not be opened");
        return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_IO_ERROR;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "migration manifest length could not be read");
        return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_IO_ERROR;
    }
    length_long = ftell(file);
    if (length_long < 0L) {
        fclose(file);
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "migration manifest length could not be read");
        return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_IO_ERROR;
    }
    length = (size_t)length_long;
    if (length < TINYDB_COMPACT_V2_MIGRATION_MANIFEST_FIXED_SIZE +
                     TINYDB_COMPACT_V2_MIGRATION_MANIFEST_CHECKSUM_SIZE ||
        length > TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE ||
        length > encoded_scratch_capacity) {
        fclose(file);
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "migration manifest has invalid bounded length");
        return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_INVALID;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "migration manifest could not be rewound");
        return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_IO_ERROR;
    }

    bytes_read = fread(encoded_scratch, 1u, length, file);
    if (bytes_read != length || ferror(file)) {
        fclose(file);
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "migration manifest could not be read completely");
        return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_IO_ERROR;
    }
    if (fclose(file) != 0) {
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "migration manifest close failed");
        return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_IO_ERROR;
    }

    if (!tinydb_compact_v2_migration_manifest_decode(
            encoded_scratch,
            length,
            manifest_out,
            claimed_pages_out,
            claimed_pages_capacity)) {
        memset(manifest_out, 0, sizeof(*manifest_out));
        tinydb_compact_v2_migration_manifest_file_message(
            message, message_capacity, "migration manifest is corrupt or inconsistent");
        return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_INVALID;
    }

    return TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_OK;
}

#endif