#include "diagnostics.h"
#include "engine.h"

#include <stdio.h>
#include <string.h>

static int find_schema_index(Table* table, const char* name) {
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int fail(const char* message) {
    fprintf(stderr, "PAGE_OWNERSHIP_FAIL: %s\n", message);
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

int main(int argc, char** argv) {
    if (argc != 2) return fail("usage: tinydb_page_ownership_probe DATABASE");

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) return fail("unable to open database");

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
        tinydb_close(database);
        return fail("unable to seed multi-root database");
    }

    char insert_sql[256];
    for (uint32_t row_id = 2u; row_id <= 260u; row_id++) {
        snprintf(insert_sql,
                 sizeof(insert_sql),
                 "INSERT INTO archive VALUES (%u, 'archive-%u', 'a%u@example.com');",
                 row_id,
                 row_id,
                 row_id);
        if (tinydb_execute_sql(database, insert_sql, &result) != TINYDB_SQL_SUCCESS) {
            tinydb_close(database);
            return fail("unable to grow archive beyond the buffer pool");
        }
    }

    if (tinydb_execute_sql(database, "PRAGMA user_version = 77;", &result) !=
        TINYDB_SQL_SUCCESS) {
        tinydb_close(database);
        return fail("unable to seed user_version before pin-pressure test");
    }

    Table* table = tinydb_table(database);
    pager_checkpoint(table->pager);
    if (table->pager->num_pages <= MAX_BUFFER_POOL_SIZE) {
        tinydb_close(database);
        return fail("diagnostic pin-pressure fixture did not exceed the buffer pool");
    }

    TinyDBPageOwnershipStats stats;
    TinyDBTreeStats tree_stats;
    TinyDBDatabaseTreeStats database_tree_stats;
    TinyDBCatalogTreeStatsSnapshot catalog_stats_snapshot;
    TinyDBCatalogTreeCheck catalog_check;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        stats.orphan_pages != 0 || stats.shared_pages != 0) {
        tinydb_close(database);
        return fail("clean database did not pass ownership check");
    }
    if (!tinydb_check_database(table, &stats, message, sizeof(message))) {
        tinydb_close(database);
        return fail("clean database did not pass whole-database diagnostics");
    }

    int archive_for_inspection = find_schema_index(table, "archive");
    if (archive_for_inspection < 0) {
        tinydb_close(database);
        return fail("archive schema missing before inspection pressure test");
    }
    uint32_t archive_inspection_root =
        table->catalog.schemas[archive_for_inspection].root_page_num;

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
            tinydb_close(database);
            return fail("unable to pin the complete buffer pool fixture");
        }
        owner_count++;
    }

    if (tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        strstr(message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("page ownership did not fail non-fatally under full pin pressure");
    }
    if (tinydb_get_tree_stats_diagnostic(table,
                                         "users",
                                         &tree_stats,
                                         message,
                                         sizeof(message)) ||
        strstr(message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("tree stats did not preserve full-pool backpressure detail");
    }
    memset(&catalog_stats_snapshot, 0xA5, sizeof(catalog_stats_snapshot));
    if (tinydb_get_catalog_tree_stats_snapshot(table,
                                               &catalog_stats_snapshot,
                                               message,
                                               sizeof(message)) ||
        strstr(message, "buffer pool busy") == NULL ||
        catalog_stats_snapshot.aggregate.table_count != 0u ||
        catalog_stats_snapshot.aggregate.total_rows != 0u ||
        catalog_stats_snapshot.aggregate.leaf_pages != 0u ||
        catalog_stats_snapshot.aggregate.internal_pages != 0u ||
        catalog_stats_snapshot.aggregate.max_height != 0u ||
        catalog_stats_snapshot.table_stats[0].total_rows != 0u ||
        catalog_stats_snapshot.table_stats[1].total_rows != 0u) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("catalog stats snapshot published partial results under full pin pressure");
    }
    memset(&database_tree_stats, 0xA5, sizeof(database_tree_stats));
    if (tinydb_get_database_tree_stats(table,
                                       &database_tree_stats,
                                       message,
                                       sizeof(message)) ||
        strstr(message, "buffer pool busy") == NULL ||
        database_tree_stats.table_count != 0u ||
        database_tree_stats.total_rows != 0u ||
        database_tree_stats.leaf_pages != 0u ||
        database_tree_stats.internal_pages != 0u ||
        database_tree_stats.max_height != 0u) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("catalog stats published partial totals under full pin pressure");
    }
    memset(&catalog_check, 0xA5, sizeof(catalog_check));
    if (tinydb_check_catalog_trees(table,
                                   &catalog_check,
                                   message,
                                   sizeof(message)) ||
        strstr(message, "buffer pool busy") == NULL ||
        catalog_check.table_count != 0u ||
        catalog_check.ownership.total_pages != 0u ||
        catalog_check.ownership.owned_pages != 0u) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("catalog check published partial results under full pin pressure");
    }
    if (tinydb_check_table_tree(table, "users", message, sizeof(message)) ||
        strstr(message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("table diagnostics did not fail non-fatally under full pin pressure");
    }
    if (tinydb_check_database(table, &stats, message, sizeof(message)) ||
        strstr(message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("whole-database diagnostics did not fail non-fatally under full pin pressure");
    }
    if (tinydb_execute_sql(database, "PRAGMA table_info(archive);", &result) !=
            TINYDB_SQL_SUCCESS ||
        tinydb_execute_sql(database, "PRAGMA index_list(archive);", &result) !=
            TINYDB_SQL_SUCCESS) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("catalog-only PRAGMAs unexpectedly required a free buffer frame");
    }
    if (tinydb_execute_sql(database, "PRAGMA integrity_check;", &result) !=
            TINYDB_SQL_EXECUTE_ERROR ||
        strstr(result.message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("PRAGMA integrity_check did not preserve non-fatal buffer-pool backpressure");
    }
    if (tinydb_print_page_nonfatal(table, 0u)) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("page inspection unexpectedly succeeded with every frame pinned");
    }
    if (tinydb_print_tree_nonfatal(table->pager,
                                   archive_inspection_root,
                                   0u)) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("tree inspection unexpectedly completed with every frame pinned");
    }
    if (tinydb_execute_sql(database, "PRAGMA user_version;", &result) !=
            TINYDB_SQL_EXECUTE_ERROR ||
        strstr(result.message, "buffer pool busy") == NULL) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("PRAGMA user_version did not return non-fatal buffer-pool backpressure");
    }

    if (!pager_release_page_handle(&owners[MAX_BUFFER_POOL_SIZE - 1u])) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("unable to free one frame for diagnostic progress");
    }
    if (!tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        !tinydb_get_tree_stats_diagnostic(table,
                                          "users",
                                          &tree_stats,
                                          message,
                                          sizeof(message)) ||
        !tinydb_get_catalog_tree_stats_snapshot(table,
                                                &catalog_stats_snapshot,
                                                message,
                                                sizeof(message)) ||
        !tinydb_get_database_tree_stats(table,
                                        &database_tree_stats,
                                        message,
                                        sizeof(message)) ||
        !tinydb_check_catalog_trees(table,
                                    &catalog_check,
                                    message,
                                    sizeof(message)) ||
        !tinydb_check_table_tree(table, "users", message, sizeof(message)) ||
        !tinydb_check_database(table, &stats, message, sizeof(message))) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("diagnostics require more than one free frame");
    }
    if (tree_stats.total_rows != 1u) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("tree stats changed while retrying after backpressure");
    }
    if (catalog_stats_snapshot.aggregate.table_count != 2u ||
        catalog_stats_snapshot.aggregate.total_rows != 261u ||
        catalog_stats_snapshot.aggregate.leaf_pages == 0u ||
        catalog_stats_snapshot.aggregate.max_height == 0u ||
        catalog_stats_snapshot.table_stats[0].total_rows != 1u ||
        catalog_stats_snapshot.table_stats[1].total_rows != 260u) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("catalog stats snapshot did not recover complete results with one free frame");
    }
    if (database_tree_stats.table_count != 2u ||
        database_tree_stats.total_rows != 261u ||
        database_tree_stats.leaf_pages == 0u ||
        database_tree_stats.max_height == 0u) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("catalog stats did not recover complete totals with one free frame");
    }
    if (catalog_check.table_count != 2u ||
        catalog_check.table_stats[0].total_rows != 1u ||
        catalog_check.table_stats[1].total_rows != 260u ||
        catalog_check.ownership.orphan_pages != 0u ||
        catalog_check.ownership.shared_pages != 0u ||
        catalog_check.ownership.owned_pages == 0u) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("catalog check did not recover complete results with one free frame");
    }
    if (tinydb_execute_sql(database, "PRAGMA integrity_check;", &result) !=
        TINYDB_SQL_SUCCESS) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("PRAGMA integrity_check did not recover with one free frame");
    }
    if (!tinydb_print_page_nonfatal(table, 0u)) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("page inspection did not recover with one free frame");
    }
    if (!tinydb_print_tree_nonfatal(table->pager,
                                    archive_inspection_root,
                                    0u)) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("tree inspection requires more than one free frame");
    }
    if (tinydb_execute_sql(database, "PRAGMA user_version;", &result) !=
            TINYDB_SQL_SUCCESS ||
        db_get_user_version(table) != 77u) {
        (void)release_handles(owners, owner_count);
        tinydb_close(database);
        return fail("PRAGMA user_version did not recover with one free frame");
    }

    if (!release_handles(owners, owner_count)) {
        tinydb_close(database);
        return fail("unable to release diagnostic pin-pressure owners");
    }
    if (!tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        !tinydb_check_table_tree(table, "users", message, sizeof(message)) ||
        !tinydb_check_database(table, &stats, message, sizeof(message))) {
        tinydb_close(database);
        return fail("diagnostics did not recover after pin release");
    }

    uint32_t orphan_page = get_unused_page_num(table->pager);
    (void)get_page(table->pager, orphan_page);
    if (tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        stats.orphan_pages != 1 || strstr(message, "unreachable") == NULL) {
        tinydb_close(database);
        return fail("orphan page was not detected");
    }
    if (tinydb_check_database(table, &stats, message, sizeof(message)) ||
        stats.orphan_pages != 1 || strstr(message, "page ownership:") == NULL) {
        tinydb_close(database);
        return fail("whole-database diagnostics accepted orphan page");
    }
    if (tinydb_execute_sql(database, "PRAGMA integrity_check;", &result) == TINYDB_SQL_SUCCESS) {
        tinydb_close(database);
        return fail("PRAGMA integrity_check accepted orphan page");
    }

    pager_free_page(table->pager, orphan_page);
    if (!tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        !tinydb_check_database(table, &stats, message, sizeof(message))) {
        tinydb_close(database);
        return fail("freeing orphan page did not restore ownership validity");
    }

    int users_index = find_schema_index(table, "users");
    int archive_index = find_schema_index(table, "archive");
    if (users_index < 0 || archive_index < 0) {
        tinydb_close(database);
        return fail("catalog schemas missing");
    }

    uint32_t archive_root = table->catalog.schemas[archive_index].root_page_num;
    table->catalog.schemas[archive_index].root_page_num =
        table->catalog.schemas[users_index].root_page_num;

    if (tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        stats.shared_pages == 0 || strstr(message, "referenced by both") == NULL) {
        tinydb_close(database);
        return fail("shared root/page was not detected");
    }
    if (tinydb_check_database(table, &stats, message, sizeof(message)) ||
        strstr(message, "share root page") == NULL) {
        tinydb_close(database);
        return fail("whole-database diagnostics accepted duplicate table roots");
    }
    if (tinydb_execute_sql(database, "PRAGMA integrity_check;", &result) == TINYDB_SQL_SUCCESS) {
        tinydb_close(database);
        return fail("PRAGMA integrity_check accepted shared root/page");
    }

    table->catalog.schemas[archive_index].root_page_num = archive_root;
    if (!tinydb_check_page_ownership(table, &stats, message, sizeof(message)) ||
        !tinydb_check_database(table, &stats, message, sizeof(message))) {
        tinydb_close(database);
        return fail("restoring root did not restore ownership validity");
    }

    tinydb_close(database);
    printf("PAGE_OWNERSHIP_OK diagnostic_pin_pressure=yes direct_ownership_busy=yes tree_stats_busy=yes catalog_snapshot_busy=yes catalog_stats_busy=yes catalog_check_busy=yes table_check_busy=yes one_free_frame_success=yes tree_stats_one_free_frame_success=yes catalog_snapshot_one_free_frame_success=yes catalog_stats_one_free_frame_success=yes catalog_check_one_free_frame_success=yes catalog_pragmas_full_pool_success=yes page_inspect_busy=yes tree_inspect_busy=yes inspection_one_free_frame_success=yes integrity_pragma_busy=yes integrity_pragma_one_free_frame_success=yes user_version_busy=yes user_version_one_free_frame_success=yes\n");
    return 0;
}
