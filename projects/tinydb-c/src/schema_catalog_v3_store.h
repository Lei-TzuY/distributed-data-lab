#ifndef TINYDB_SCHEMA_CATALOG_V3_STORE_H
#define TINYDB_SCHEMA_CATALOG_V3_STORE_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "schema_catalog_v3_envelope.h"

/*
 * Durable file publication for the V3 schema catalog envelope.
 *
 * This is intentionally separate from the legacy V1/V2 reader so the existing
 * on-disk ABI stays untouched while the V3 production path is brought up.
 * The WAL contains one complete checksummed V3 envelope followed by a commit
 * marker. Recovery only publishes a WAL after both the outer envelope and the
 * marker validate. Main-file publication writes the same envelope without the
 * marker and syncs it before the WAL may be removed.
 *
 * The durable commit point is the synced WAL commit marker, not the later
 * best-effort main-file copy. Once that point is crossed, reopen recovery will
 * publish the envelope even if copying the main file or removing the WAL fails.
 */

#define TINYDB_SCHEMA_CATALOG_V3_WAL_COMMIT_MAGIC UINT32_C(0x33435754) /* TWC3 */
#define TINYDB_SCHEMA_CATALOG_V3_COMMIT_SIZE 4u

typedef enum TinyDBSchemaCatalogV3StoreReadResult {
    TINYDB_SCHEMA_CATALOG_V3_STORE_READ_OK = 0,
    TINYDB_SCHEMA_CATALOG_V3_STORE_READ_ABSENT,
    TINYDB_SCHEMA_CATALOG_V3_STORE_READ_INVALID,
    TINYDB_SCHEMA_CATALOG_V3_STORE_READ_IO_ERROR
} TinyDBSchemaCatalogV3StoreReadResult;

typedef struct TinyDBSchemaCatalogV3StorePublishResult {
    bool wal_committed_durable;
    bool main_published_durable;
    bool cleanup_complete;
} TinyDBSchemaCatalogV3StorePublishResult;

static inline void tinydb_schema_catalog_v3_store_publish_result_zero(
    TinyDBSchemaCatalogV3StorePublishResult* result) {
    if (result != NULL) memset(result, 0, sizeof(*result));
}

static inline bool tinydb_schema_catalog_v3_store_sync(FILE* file) {
    if (file == NULL || fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static inline TinyDBSchemaCatalogV3StoreReadResult
 tinydb_schema_catalog_v3_store_read(const char* path,
                                     bool require_commit_marker,
                                     unsigned char* envelope_out,
                                     size_t envelope_capacity,
                                     size_t* envelope_size_out) {
    if (envelope_size_out != NULL) *envelope_size_out = 0u;
    if (path == NULL || envelope_out == NULL || envelope_size_out == NULL ||
        envelope_capacity < TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE +
                            TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE) {
        return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_INVALID;
    }

    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return errno == ENOENT ? TINYDB_SCHEMA_CATALOG_V3_STORE_READ_ABSENT
                              : TINYDB_SCHEMA_CATALOG_V3_STORE_READ_IO_ERROR;
    }

    unsigned char header[TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_INVALID;
    }

    const uint32_t declared_size = tinydb_schema_catalog_v3_get_u32(header + 8u);
    if (tinydb_schema_catalog_v3_get_u32(header) !=
            TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAGIC ||
        tinydb_schema_catalog_v3_get_u32(header + 4u) !=
            TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_VERSION ||
        declared_size < TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE +
                        TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE ||
        declared_size > TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE ||
        declared_size > envelope_capacity) {
        fclose(file);
        return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_INVALID;
    }

    memcpy(envelope_out, header, sizeof(header));
    const size_t remainder = (size_t)declared_size - sizeof(header);
    if (fread(envelope_out + sizeof(header), 1, remainder, file) != remainder) {
        fclose(file);
        return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_INVALID;
    }

    TinyDBSchemaCatalogV3EnvelopeView view;
    if (tinydb_schema_catalog_v3_envelope_decode(
            envelope_out, (size_t)declared_size, &view) !=
        TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK) {
        fclose(file);
        return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_INVALID;
    }

    if (require_commit_marker) {
        unsigned char commit[TINYDB_SCHEMA_CATALOG_V3_COMMIT_SIZE];
        if (fread(commit, 1, sizeof(commit), file) != sizeof(commit) ||
            tinydb_schema_catalog_v3_get_u32(commit) !=
                TINYDB_SCHEMA_CATALOG_V3_WAL_COMMIT_MAGIC) {
            fclose(file);
            return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_INVALID;
        }
    }

    const int trailing = fgetc(file);
    if (ferror(file)) {
        fclose(file);
        return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_IO_ERROR;
    }
    if (trailing != EOF) {
        fclose(file);
        return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_INVALID;
    }
    if (fclose(file) != 0) return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_IO_ERROR;

    *envelope_size_out = (size_t)declared_size;
    return TINYDB_SCHEMA_CATALOG_V3_STORE_READ_OK;
}

static inline bool tinydb_schema_catalog_v3_store_write_file(
    const char* path,
    const unsigned char* envelope,
    size_t envelope_size,
    bool include_commit_marker) {
    if (path == NULL || envelope == NULL) return false;

    TinyDBSchemaCatalogV3EnvelopeView view;
    if (tinydb_schema_catalog_v3_envelope_decode(envelope, envelope_size, &view) !=
        TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK) {
        return false;
    }

    FILE* file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fwrite(envelope, 1, envelope_size, file) == envelope_size;
    if (ok && include_commit_marker) {
        unsigned char commit[TINYDB_SCHEMA_CATALOG_V3_COMMIT_SIZE];
        tinydb_schema_catalog_v3_put_u32(
            commit, TINYDB_SCHEMA_CATALOG_V3_WAL_COMMIT_MAGIC);
        ok = fwrite(commit, 1, sizeof(commit), file) == sizeof(commit);
    }
    if (ok) ok = tinydb_schema_catalog_v3_store_sync(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static inline bool tinydb_schema_catalog_v3_store_publish_detailed(
    const char* main_path,
    const char* wal_path,
    const unsigned char* envelope,
    size_t envelope_size,
    TinyDBSchemaCatalogV3StorePublishResult* result) {
    tinydb_schema_catalog_v3_store_publish_result_zero(result);
    if (main_path == NULL || wal_path == NULL || envelope == NULL || result == NULL) {
        return false;
    }

    /* Crossing this boundary commits the new catalog. Recovery treats a valid,
     * synced WAL as authoritative even if the main copy is still old/missing. */
    if (!tinydb_schema_catalog_v3_store_write_file(
            wal_path, envelope, envelope_size, true)) {
        return false;
    }
    result->wal_committed_durable = true;

    if (!tinydb_schema_catalog_v3_store_write_file(
            main_path, envelope, envelope_size, false)) {
        return true;
    }
    result->main_published_durable = true;

    if (remove(wal_path) == 0 || errno == ENOENT) {
        result->cleanup_complete = true;
    }
    return true;
}

static inline bool tinydb_schema_catalog_v3_store_publish(
    const char* main_path,
    const char* wal_path,
    const unsigned char* envelope,
    size_t envelope_size) {
    TinyDBSchemaCatalogV3StorePublishResult result;
    return tinydb_schema_catalog_v3_store_publish_detailed(
        main_path, wal_path, envelope, envelope_size, &result);
}

static inline bool tinydb_schema_catalog_v3_store_recover(
    const char* main_path,
    const char* wal_path,
    unsigned char* workspace,
    size_t workspace_capacity,
    bool* recovered_out) {
    if (recovered_out != NULL) *recovered_out = false;
    if (main_path == NULL || wal_path == NULL || workspace == NULL ||
        recovered_out == NULL) {
        return false;
    }

    size_t envelope_size = 0u;
    TinyDBSchemaCatalogV3StoreReadResult result =
        tinydb_schema_catalog_v3_store_read(wal_path,
                                            true,
                                            workspace,
                                            workspace_capacity,
                                            &envelope_size);
    if (result == TINYDB_SCHEMA_CATALOG_V3_STORE_READ_ABSENT) return true;
    if (result == TINYDB_SCHEMA_CATALOG_V3_STORE_READ_INVALID) {
        /* An invalid/uncommitted WAL is never authoritative. Removing it is
         * safe because the main file was the last completed publication. */
        if (remove(wal_path) != 0 && errno != ENOENT) return false;
        return true;
    }
    if (result != TINYDB_SCHEMA_CATALOG_V3_STORE_READ_OK) return false;

    if (!tinydb_schema_catalog_v3_store_write_file(
            main_path, workspace, envelope_size, false)) {
        return false;
    }
    if (remove(wal_path) != 0 && errno != ENOENT) return false;
    *recovered_out = true;
    return true;
}

#endif /* TINYDB_SCHEMA_CATALOG_V3_STORE_H */
