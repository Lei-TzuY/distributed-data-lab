#include "engine.h"
#include "compact_v2_migration_open_adapter.h"
#include "compact_v2_migration_open_recovery.h"
#include "compact_v2_migration_full_page_preflight.h"
#include "schema_catalog_authoritative_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TinyDB* tinydb_open(const char* filename) {
    TinyDBCompactV2MigrationOpenAdapterContext recovery_context;
    TinyDBCompactV2MigrationPagerRecoveryAdapter recovery_adapter;
    TinyDBCompactV2MigrationOpenRecoveryWorkspace* recovery_workspace = NULL;
    TinyDBCompactV2MigrationRecoveryResult recovery_result;
    TinyDBCompactV2MigrationOpenRecoveryStatus recovery_status;
    char recovery_message[TINYDB_ENGINE_MESSAGE_MAX];

    if (filename == NULL || filename[0] == '\0' ||
        strlen(filename) >= TINYDB_ENGINE_FILENAME_MAX) {
        return NULL;
    }

    TinyDB* database = (TinyDB*)calloc(1, sizeof(*database));
    if (database == NULL) return NULL;

    snprintf(database->filename, sizeof(database->filename), "%s", filename);
    database->table = db_open(filename);
    if (database->table == NULL) {
        free(database);
        return NULL;
    }

    if (!multitable_catalog_load(database->table, database->filename)) {
        printf("Error: schema catalog could not be loaded safely.\n");
        db_close(database->table);
        database->table = NULL;
        free(database);
        return NULL;
    }

    TinyDBSchemaCatalogGenerationSnapshot authoritative_generation;
    if (!tinydb_schema_catalog_load_authoritative_generation(
            database->table,
            database->filename,
            &authoritative_generation)) {
        printf("Error: authoritative schema generation state could not be loaded safely.\n");
        db_close(database->table);
        database->table = NULL;
        free(database);
        return NULL;
    }

    if (!tinydb_compact_v2_migration_open_adapter_init(
            &recovery_context,
            database->filename,
            &database->table->catalog,
            &authoritative_generation,
            database->table->pager) ||
        !tinydb_compact_v2_migration_open_adapter_build(
            &recovery_context,
            &recovery_adapter)) {
        printf("Error: compact V2 recovery adapter could not be initialized safely.\n");
        db_close(database->table);
        database->table = NULL;
        free(database);
        return NULL;
    }

    recovery_workspace = (TinyDBCompactV2MigrationOpenRecoveryWorkspace*)
        calloc(1, sizeof(*recovery_workspace));
    if (recovery_workspace == NULL) {
        printf("Error: compact V2 recovery workspace could not be allocated.\n");
        db_close(database->table);
        database->table = NULL;
        free(database);
        return NULL;
    }

    memset(&recovery_result, 0, sizeof(recovery_result));
    memset(recovery_message, 0, sizeof(recovery_message));
    recovery_status = tinydb_compact_v2_migration_recover_open_file_with_preflight(
        database->filename,
        &recovery_adapter,
        recovery_workspace,
        &recovery_result,
        &recovery_context,
        tinydb_compact_v2_migration_open_adapter_manifest_pages_are_safe,
        recovery_message,
        sizeof(recovery_message));
    free(recovery_workspace);
    recovery_workspace = NULL;

    if (recovery_status != TINYDB_COMPACT_V2_MIGRATION_OPEN_NO_MIGRATION &&
        recovery_status != TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERED) {
        printf("Error: compact V2 migration recovery failed safely: %s.\n",
               recovery_message[0] != '\0' ? recovery_message : "unknown recovery error");
        db_close(database->table);
        database->table = NULL;
        free(database);
        return NULL;
    }

    if (recovery_status == TINYDB_COMPACT_V2_MIGRATION_OPEN_RECOVERED &&
        !tinydb_compact_v2_migration_validate_catalog_live_pages(
            &database->table->catalog,
            database->table->pager)) {
        printf("Error: compact V2 recovery left authoritative catalog trees invalid.\n");
        db_close(database->table);
        database->table = NULL;
        free(database);
        return NULL;
    }

    return database;
}
