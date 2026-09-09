#include "engine.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

static int fail(TinyDB* database, const char* message) {
    fprintf(stderr, "COMMITTED_CRASH_PROBE_FAIL: %s\n", message);
    if (database != NULL) tinydb_close(database);
    return 1;
}

static bool parse_u32(const char* text, uint32_t* value) {
    if (text == NULL || text[0] == '\0') return false;
    char* end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool has_table(const Table* table, const char* name) {
    if (table == NULL || name == NULL) return false;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) return true;
    }
    return false;
}

static bool execute_ok(TinyDB* database, const char* sql) {
    TinyDBSqlResult result;
    return tinydb_execute_sql(database, sql, &result) == TINYDB_SQL_SUCCESS;
}

static bool wal_is_nonempty(const char* database_filename) {
    char wal_path[TINYDB_ENGINE_FILENAME_MAX + 8u];
    int written = snprintf(wal_path, sizeof(wal_path), "%s.wal", database_filename);
    if (written < 0 || (size_t)written >= sizeof(wal_path)) return false;

    FILE* file = fopen(wal_path, "rb");
    if (file == NULL) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0 && ftell(file) > 0;
    fclose(file);
    return ok;
}

static void hard_exit_success(void) {
    /* Deliberately bypass tinydb_close(), stdio flushing, and checkpointing.
     * The transaction has already returned successfully from COMMIT, so the
     * next open must reconstruct committed state from the durable WAL. */
    _exit(0);
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr,
                "usage: tinydb_committed_crash_probe DATABASE START_ID COUNT [archive]\n");
        return 2;
    }

    uint32_t start_id = 0;
    uint32_t count = 0;
    if (!parse_u32(argv[2], &start_id) || start_id == 0 ||
        !parse_u32(argv[3], &count) || count == 0 ||
        count - 1u > UINT32_MAX - start_id) {
        fprintf(stderr, "COMMITTED_CRASH_PROBE_FAIL: invalid id/count range\n");
        return 2;
    }

    bool include_archive = false;
    if (argc == 5) {
        if (strcmp(argv[4], "archive") != 0) {
            fprintf(stderr, "COMMITTED_CRASH_PROBE_FAIL: unknown mode '%s'\n", argv[4]);
            return 2;
        }
        include_archive = true;
    }

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) return fail(NULL, "unable to open database");

    if (include_archive && !has_table(tinydb_table(database), "archive")) {
        if (!execute_ok(database,
                        "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);")) {
            return fail(database, "unable to create archive table");
        }
    }

    if (!execute_ok(database, "BEGIN;")) {
        return fail(database, "unable to begin transaction");
    }

    char sql[256];
    for (uint32_t offset = 0; offset < count; offset++) {
        uint32_t id = start_id + offset;
        int written = snprintf(sql,
                               sizeof(sql),
                               "INSERT INTO users VALUES (%u, 'durable_%u', 'durable%u@crash.test');",
                               id,
                               id,
                               id);
        if (written < 0 || (size_t)written >= sizeof(sql) ||
            !execute_ok(database, sql)) {
            return fail(database, "unable to insert committed users row");
        }

        if (include_archive) {
            written = snprintf(sql,
                               sizeof(sql),
                               "INSERT INTO archive VALUES (%u, 'archive_%u', 'archive%u@crash.test');",
                               id,
                               id,
                               id);
            if (written < 0 || (size_t)written >= sizeof(sql) ||
                !execute_ok(database, sql)) {
                return fail(database, "unable to insert committed archive row");
            }
        }
    }

    if (!execute_ok(database, "COMMIT;")) {
        return fail(database, "COMMIT did not complete successfully");
    }
    if (!wal_is_nonempty(argv[1])) {
        return fail(database, "COMMIT returned without a durable non-empty WAL");
    }

    hard_exit_success();
    return 0;
}
