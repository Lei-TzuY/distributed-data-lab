#include "engine.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

static bool execute_ok(TinyDB* database, const char* sql) {
    TinyDBSqlResult result;
    return tinydb_execute_sql(database, sql, &result) == TINYDB_SQL_SUCCESS;
}

static bool file_nonempty(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0 && ftell(file) > 0;
    fclose(file);
    return ok;
}

static bool sidecar_exists(const char* database_filename, const char* suffix) {
    char path[TINYDB_ENGINE_FILENAME_MAX + 16u];
    int written = snprintf(path, sizeof(path), "%s%s", database_filename, suffix);
    if (written < 0 || (size_t)written >= sizeof(path)) return false;
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static bool wal_nonempty(const char* database_filename) {
    char path[TINYDB_ENGINE_FILENAME_MAX + 16u];
    int written = snprintf(path, sizeof(path), "%s.wal", database_filename);
    if (written < 0 || (size_t)written >= sizeof(path)) return false;
    return file_nonempty(path);
}

static int fail(TinyDB* database, const char* message) {
    fprintf(stderr, "FREE_PAGE_RECOVERY_CRASH_PROBE_FAIL: %s\n", message);
    if (database != NULL) tinydb_close(database);
    return EXIT_FAILURE;
}

static void hard_exit_success(void) {
    /* Deliberately bypass tinydb_close(), checkpointing, and stdio cleanup.
     * Every preceding statement has already returned success from its
     * autocommit path, so the next open must recover both database and
     * allocator state from durable sidecars/WAL rather than normal close. */
    _exit(0);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: free_page_recovery_crash_probe DATABASE\n");
        return 2;
    }

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) return fail(NULL, "unable to open database");

    if (!execute_ok(database,
                    "CREATE TABLE metrics (id INT, value INT, tag VARCHAR);")) {
        return fail(database, "unable to create metrics table");
    }

    char sql[256];
    for (uint32_t id = 1u; id <= 60u; id++) {
        int written = snprintf(sql,
                               sizeof(sql),
                               "INSERT INTO metrics VALUES (%u, %u, 'tag-%u');",
                               id,
                               id * 10u,
                               id);
        if (written < 0 || (size_t)written >= sizeof(sql) ||
            !execute_ok(database, sql)) {
            return fail(database, "unable to insert metrics row");
        }
    }

    for (uint32_t id = 1u; id <= 55u; id++) {
        int written = snprintf(sql,
                               sizeof(sql),
                               "DELETE FROM metrics WHERE id = %u;",
                               id);
        if (written < 0 || (size_t)written >= sizeof(sql) ||
            !execute_ok(database, sql)) {
            return fail(database, "unable to delete metrics row");
        }
    }

    if (!sidecar_exists(argv[1], ".free")) {
        return fail(database, "free-page sidecar was not durably published");
    }
    if (!wal_nonempty(argv[1])) {
        return fail(database, "main WAL is empty after committed mutations");
    }

    hard_exit_success();
    return 0;
}
