#include "engine.h"
#include "table_stats_try.h"

#include <stdio.h>
#include <string.h>

static int fail(TinyDB* database, const char* message) {
    if (database != NULL) tinydb_close(database);
    fprintf(stderr, "INTEGRITY_API_PIN_FAIL: %s\n", message);
    return 1;
}

static bool release_handles(PagerPageHandle* handles, uint32_t count) {
    bool ok = true;
    for (uint32_t i = 0u; i < count; i++) {
        if (handles[i].pinned && !pager_release_page_handle(&handles[i])) {
            ok = false;
        }
    }
    return ok;
}

static bool stats_zero(const TableStats* stats) {
    return stats->total_pages == 0u &&
           stats->leaf_pages == 0u &&
           stats->internal_pages == 0u &&
           stats->free_pages == 0u &&
           stats->total_rows == 0u;
}

static bool stats_equal(const TableStats* left, const TableStats* right) {
    return left->total_pages == right->total_pages &&
           left->leaf_pages == right->leaf_pages &&
           left->internal_pages == right->internal_pages &&
           left->free_pages == right->free_pages &&
           left->total_rows == right->total_rows;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "INTEGRITY_API_PIN_FAIL: usage: tinydb_integrity_api_pin_probe DATABASE\n");
        return 1;
    }

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) return fail(NULL, "unable to open database");

    TinyDBSqlResult result;
    if (tinydb_execute_sql(database,
                           "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                           &result) != TINYDB_SQL_SUCCESS ||
        tinydb_execute_sql(database,
                           "INSERT INTO users VALUES (1, 'main', 'main@example.com');",
                           &result) != TINYDB_SQL_SUCCESS ||
        tinydb_execute_sql(database,
                           "INSERT INTO archive VALUES (1, 'copy', 'copy@example.com');",
                           &result) != TINYDB_SQL_SUCCESS) {
        return fail(database, "unable to seed multi-root database");
    }

    char sql[256];
    for (uint32_t row_id = 2u; row_id <= 260u; row_id++) {
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO archive VALUES (%u, 'archive-%u', 'a%u@example.com');",
                 row_id,
                 row_id,
                 row_id);
        if (tinydb_execute_sql(database, sql, &result) != TINYDB_SQL_SUCCESS) {
            return fail(database, "unable to grow archive beyond the buffer pool");
        }
    }

    Table* table = tinydb_table(database);
    pager_checkpoint(table->pager);
    if (table->pager->num_pages <= MAX_BUFFER_POOL_SIZE) {
        return fail(database, "fixture did not exceed the buffer pool");
    }

    PagerPageHandle owners[MAX_BUFFER_POOL_SIZE];
    memset(owners, 0, sizeof(owners));
    uint32_t owner_count = 0u;
    for (uint32_t page_num = 1u;
         page_num <= MAX_BUFFER_POOL_SIZE;
         page_num++) {
        if (!pager_pin_page_handle(table->pager,
                                   page_num,
                                   &owners[owner_count])) {
            (void)release_handles(owners, owner_count);
            return fail(database, "unable to pin complete buffer pool fixture");
        }
        owner_count++;
    }

    if (db_integrity_check(table)) {
        (void)release_handles(owners, owner_count);
        return fail(database, "db_integrity_check unexpectedly succeeded with every frame pinned");
    }

    TableStats safe_stats;
    memset(&safe_stats, 0xA5, sizeof(safe_stats));
    char stats_message[TINYDB_ENGINE_MESSAGE_MAX];
    if (db_try_get_stats(table,
                         &safe_stats,
                         stats_message,
                         sizeof(stats_message))) {
        (void)release_handles(owners, owner_count);
        return fail(database, "db_try_get_stats unexpectedly succeeded with every frame pinned");
    }
    if (!stats_zero(&safe_stats)) {
        (void)release_handles(owners, owner_count);
        return fail(database, "db_try_get_stats published partial output on BUSY");
    }
    if (strstr(stats_message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        return fail(database, "db_try_get_stats did not preserve BUSY detail");
    }

    if (!pager_release_page_handle(&owners[MAX_BUFFER_POOL_SIZE - 1u])) {
        (void)release_handles(owners, owner_count);
        return fail(database, "unable to free one frame");
    }
    if (!db_integrity_check(table)) {
        (void)release_handles(owners, owner_count);
        return fail(database, "db_integrity_check requires more than one free frame");
    }
    if (!db_try_get_stats(table,
                          &safe_stats,
                          stats_message,
                          sizeof(stats_message))) {
        (void)release_handles(owners, owner_count);
        return fail(database, "db_try_get_stats requires more than one free frame");
    }
    if (safe_stats.total_pages != table->pager->num_pages ||
        safe_stats.free_pages != table->pager->free_page_count ||
        safe_stats.total_rows != 1u ||
        safe_stats.leaf_pages != 1u ||
        safe_stats.internal_pages == 0u) {
        (void)release_handles(owners, owner_count);
        return fail(database, "db_try_get_stats returned unexpected routed/global statistics");
    }

    if (!release_handles(owners, owner_count)) {
        return fail(database, "unable to release pin-pressure owners");
    }

    TableStats legacy_stats;
    db_get_stats(table, &legacy_stats);
    if (!stats_equal(&safe_stats, &legacy_stats)) {
        return fail(database, "db_try_get_stats does not match legacy db_get_stats after pressure clears");
    }

    TableStats no_message_stats;
    if (!db_try_get_stats(table, &no_message_stats, NULL, 0u) ||
        !stats_equal(&no_message_stats, &legacy_stats)) {
        return fail(database, "db_try_get_stats failed without a diagnostic buffer");
    }

    uint32_t orphan_page = get_unused_page_num(table->pager);
    (void)get_page(table->pager, orphan_page);
    if (db_integrity_check(table)) {
        return fail(database, "db_integrity_check accepted an allocated orphan page");
    }

    pager_free_page(table->pager, orphan_page);
    if (!db_integrity_check(table)) {
        return fail(database, "db_integrity_check did not recover after freeing orphan page");
    }

    tinydb_close(database);
    printf("INTEGRITY_API_PIN_OK busy_nonfatal=yes one_free_frame_success=yes ownership_fail_closed=yes recovery=yes stats_busy_nonfatal=yes stats_zero_publish=yes stats_one_free_frame_success=yes stats_legacy_match=yes stats_optional_message=yes\n");
    return 0;
}
