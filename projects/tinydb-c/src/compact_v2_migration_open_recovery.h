#ifndef COMPACT_V2_MIGRATION_OPEN_RECOVERY_H
#define COMPACT_V2_MIGRATION_OPEN_RECOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "compact_v2_migration_manifest_file.h"
#include "compact_v2_migration_pager_recovery.h"

/*
 * Database-open orchestration for interrupted fixed-V1 -> compact-V2
 * migrations.
 *
 * The manifest file remains untrusted until the bounded loader has completed
 * checksum and semantic validation.  Only a successfully decoded manifest is
 * handed to the Pager-backed strict recovery executor.  A present-but-invalid
 * sidecar therefore fails closed without opening a Pager transaction or
 * reclaiming any page.
 *
 * Production callers may additionally supply a manifest preflight callback.
 * It runs after bounded decode but before Pager recovery starts, allowing
 * multi-table catalog ownership invariants to reject a semantically valid yet
 * cross-table-dangerous manifest without changing the historical recovery
 * adapter aggregate ABI.
 *
 * The workspace is caller-owned so database open does not place the bounded
 * ~32 KiB decode buffers on a small stack.  The decoded manifest borrows the
 * workspace claim array only for the duration of this call.
 */

typedef enum TinyDBCompactV2MigrationOpenRecoveryStatus {
    TINYDB_COMPACT_V2_MIGRATION_OPEN_NO_MIGRATION = 0,
    TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERED = 1,
    TINYDB_COMPACT_V2_MIGRATION_OPEN_INVALID_MANIFEST = 2,
    TINYDB_COMPACT_V2_MIGRATION_OPEN_MANIFEST_IO_ERROR = 3,
    TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERY_FAILED = 4,
    TINYDB_COMPACT_V2_MIGRATION_OPEN_INVALID_ARGUMENT = 5
} TinyDBCompactV2MigrationOpenRecoveryStatus;

typedef struct TinyDBCompactV2MigrationOpenRecoveryWorkspace {
    TinyDBCompactV2MigrationManifest manifest;
    uint32_t claimed_pages[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS];
    unsigned char encoded[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE];
} TinyDBCompactV2MigrationOpenRecoveryWorkspace;

typedef bool (*TinyDBCompactV2MigrationManifestPreflightFn)(
    void* context,
    const TinyDBCompactV2MigrationManifest* manifest);

static inline void tinydb_compact_v2_migration_open_recovery_message(
    char* message,
    size_t message_capacity,
    const char* text) {
    if (message == NULL || message_capacity == 0u) return;
    if (text == NULL) text = "";
    (void)snprintf(message, message_capacity, "%s", text);
}

static inline TinyDBCompactV2MigrationOpenRecoveryStatus
 tinydb_compact_v2_migration_recover_open_file_with_preflight(
    const char* database_filename,
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter,
    TinyDBCompactV2MigrationOpenRecoveryWorkspace* workspace,
    TinyDBCompactV2MigrationRecoveryResult* recovery_result_out,
    void* preflight_context,
    TinyDBCompactV2MigrationManifestPreflightFn preflight,
    char* message,
    size_t message_capacity) {
    TinyDBCompactV2MigrationManifestLoadResult load_result;
    TinyDBCompactV2MigrationRecoveryResult recovered;

    if (recovery_result_out != NULL) {
        memset(recovery_result_out, 0, sizeof(*recovery_result_out));
    }
    if (workspace != NULL) memset(workspace, 0, sizeof(*workspace));
    tinydb_compact_v2_migration_open_recovery_message(message, message_capacity, "");

    if (database_filename == NULL || database_filename[0] == '\0' ||
        adapter == NULL || workspace == NULL || recovery_result_out == NULL ||
        !tinydb_compact_v2_migration_pager_recovery_adapter_is_valid(adapter)) {
        tinydb_compact_v2_migration_open_recovery_message(
            message, message_capacity, "invalid compact V2 open-recovery arguments");
        return TINYDB_COMPACT_V2_MIGRATION_OPEN_INVALID_ARGUMENT;
    }

    load_result = tinydb_compact_v2_migration_manifest_load_file(
        database_filename,
        &workspace->manifest,
        workspace->claimed_pages,
        TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS,
        workspace->encoded,
        sizeof(workspace->encoded),
        message,
        message_capacity);

    if (load_result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_ABSENT) {
        return TINYDB_COMPACT_V2_MIGRATION_OPEN_NO_MIGRATION;
    }
    if (load_result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_INVALID) {
        return TINYDB_COMPACT_V2_MIGRATION_OPEN_INVALID_MANIFEST;
    }
    if (load_result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_IO_ERROR) {
        return TINYDB_COMPACT_V2_MIGRATION_OPEN_MANIFEST_IO_ERROR;
    }
    if (load_result != TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_OK) {
        tinydb_compact_v2_migration_open_recovery_message(
            message, message_capacity, "unknown migration manifest load result");
        return TINYDB_COMPACT_V2_MIGRATION_OPEN_MANIFEST_IO_ERROR;
    }

    if (preflight != NULL && !preflight(preflight_context, &workspace->manifest)) {
        memset(recovery_result_out, 0, sizeof(*recovery_result_out));
        tinydb_compact_v2_migration_open_recovery_message(
            message, message_capacity, "compact V2 migration manifest violates catalog ownership");
        return TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERY_FAILED;
    }

    memset(&recovered, 0, sizeof(recovered));
    if (!tinydb_compact_v2_migration_pager_recover_reopen(
            &workspace->manifest, adapter, &recovered)) {
        memset(recovery_result_out, 0, sizeof(*recovery_result_out));
        tinydb_compact_v2_migration_open_recovery_message(
            message, message_capacity, "compact V2 reopen recovery failed");
        return TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERY_FAILED;
    }

    *recovery_result_out = recovered;
    tinydb_compact_v2_migration_open_recovery_message(
        message, message_capacity, "compact V2 migration recovered");
    return TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERED;
}

static inline TinyDBCompactV2MigrationOpenRecoveryStatus
 tinydb_compact_v2_migration_recover_open_file(
    const char* database_filename,
    TinyDBCompactV2MigrationPagerRecoveryAdapter* adapter,
    TinyDBCompactV2MigrationOpenRecoveryWorkspace* workspace,
    TinyDBCompactV2MigrationRecoveryResult* recovery_result_out,
    char* message,
    size_t message_capacity) {
    return tinydb_compact_v2_migration_recover_open_file_with_preflight(
        database_filename,
        adapter,
        workspace,
        recovery_result_out,
        NULL,
        NULL,
        message,
        message_capacity);
}

#endif
