#include "diagnostics.h"
#include "engine.h"
#include "compact_v2_migration_live_page_guard.h"
#include "schema_repack_table_scan.h"

static bool execute_ok(TinyDB* database, const char* sql) {
    TinyDBSqlResult result;
    TinyDBSqlStatus status = tinydb_execute_sql(database, sql, &result);
    if (status != TINYDB_SQL_SUCCESS) {
        fprintf(stderr,
                "SQL failed: %s\nstatus=%s message=%s execute=%d route=%d\n",
                sql,
                tinydb_sql_status_string(status),
                result.message,
                (int)result.execute_result,
                (int)result.route_result);
        return false;
    }
    return true;
}

static bool verify_private_repack_rows(
    const TinyDBCompactV2StagingLeafChain* leaves,
    const TableSchema* schema,
    uint32_t expected_rows) {
    uint32_t expected_key = 1u;
    for (uint32_t page_index = 0u; page_index < leaves->page_count; page_index++) {
        const unsigned char* page =
            tinydb_compact_v2_staging_page_const(leaves, page_index);
        uint32_t count = 0u;
        if (page == NULL || !tinydb_leaf_page_count(page, PAGE_SIZE, &count)) {
            return false;
        }
        for (uint32_t cell = 0u; cell < count; cell++) {
            uint32_t key = 0u;
            const void* value = NULL;
            uint32_t value_length = 0u;
            TinyDBRecordPayload payload;
            char expected_title[32];
            char expected_body[32];
            if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, cell, &key) ||
                !tinydb_leaf_page_value_at(page, PAGE_SIZE, cell,
                                           &value, &value_length) ||
                key != expected_key ||
                !tinydb_row_envelope_decode_compact_v2(
                    schema,
                    (const unsigned char*)value,
                    value_length,
                    &payload)) {
                return false;
            }
            snprintf(expected_title, sizeof(expected_title), "source-title-%u", key);
            snprintf(expected_body, sizeof(expected_body), "source-body-%u", key);
            if (strcmp((const char*)payload.bytes + schema->columns[1].offset,
                       expected_title) != 0 ||
                strcmp((const char*)payload.bytes + schema->columns[2].offset,
                       expected_body) != 0) {
                return false;
            }
            expected_key++;
        }
    }
    return expected_key == expected_rows + 1u;
}

static bool repack_real_authoritative_table(TinyDB* database) {
    if (!execute_ok(database,
                    "CREATE TABLE repack_source (id INT, title VARCHAR(31), body VARCHAR(63));")) {
        return false;
    }

    char sql[512];
    for (uint32_t id = 1u; id <= 120u; id++) {
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO repack_source VALUES (%u, 'source-title-%u', 'source-body-%u');",
                 id,
                 id,
                 id);
        if (!execute_ok(database, sql)) return false;
    }

    Table* table = tinydb_table(database);
    const TableSchema* source = tinydb_find_table_schema(table, "repack_source");
    TinyDBTreeStats source_stats;
    if (source == NULL || source->row_size != 100u ||
        source->columns[1].size != 32u || source->columns[2].size != 64u ||
        !tinydb_get_tree_stats(table, "repack_source", &source_stats) ||
        source_stats.total_rows != 120u || source_stats.leaf_pages < 2u ||
        source_stats.internal_pages < 1u) {
        fprintf(stderr, "real repack source did not form the expected multi-leaf V2 tree\n");
        return false;
    }

    TableSchema destination = *source;
    destination.columns[1].size = 128u;
    destination.columns[2].offset = 132u;
    destination.columns[2].size = 256u;
    destination.row_size = 388u;

    enum { PRIVATE_PAGE_CAPACITY = 32 };
    unsigned char leaf_images[PRIVATE_PAGE_CAPACITY * PAGE_SIZE];
    unsigned char internal_images[PRIVATE_PAGE_CAPACITY * PAGE_SIZE];
    uint32_t leaf_page_numbers[PRIVATE_PAGE_CAPACITY];
    uint32_t internal_page_numbers[PRIVATE_PAGE_CAPACITY];
    for (uint32_t i = 0u; i < PRIVATE_PAGE_CAPACITY; i++) {
        leaf_page_numbers[i] = 1001u + i;
        internal_page_numbers[i] = 2001u + i;
    }
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));

    TinyDBCompactV2StagingLeafChain leaves;
    TinyDBSchemaRepackStaging staging;
    TinyDBCompactV2StagingHierarchy hierarchy;
    TinyDBSchemaRepackStagingTreeResult result;
    char message[192];
    if (!tinydb_compact_v2_staging_leaf_chain_init(
            &leaves,
            leaf_images,
            leaf_page_numbers,
            PRIVATE_PAGE_CAPACITY) ||
        !tinydb_schema_repack_stage_table_scan(
            table,
            source,
            &destination,
            &leaves,
            &staging,
            &hierarchy,
            internal_images,
            internal_page_numbers,
            PRIVATE_PAGE_CAPACITY,
            &result,
            message,
            sizeof(message))) {
        fprintf(stderr, "real authoritative repack scan failed: %s\n", message);
        return false;
    }

    if (!result.ready || result.row_count != 120u ||
        result.leaf_page_count < 2u || result.internal_page_count < 1u ||
        result.root_page_num != hierarchy.root_page_num ||
        staging.rows_staged != 120u || leaves.row_count != 120u ||
        !verify_private_repack_rows(&leaves, &destination, 120u)) {
        fprintf(stderr, "real authoritative repack result failed validation\n");
        return false;
    }

    printf("REAL_REPACK_SCAN_OK source_root=%u source_rows=%u private_root=%u private_leaves=%u row_size=%u\n",
           source_stats.root_page_num,
           source_stats.total_rows,
           result.root_page_num,
           result.leaf_page_count,
           destination.row_size);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: tinydb_engine_api_probe <database>\n");
        return 2;
    }

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) {
        fprintf(stderr, "unable to open database\n");
        return 1;
    }

    if (!execute_ok(database,
                    "CREATE TABLE archive (id INT, title VARCHAR(255), body VARCHAR(255));")) {
        tinydb_close(database);
        return 1;
    }
    if (!execute_ok(database,
                    "INSERT INTO users VALUES (7, 'main-seven', 'main7@example.com');")) {
        tinydb_close(database);
        return 1;
    }

    char sql[768];
    for (uint32_t id = 1; id <= 80; id++) {
        snprintf(sql,
                 sizeof(sql),
                 "INSERT INTO archive VALUES (%u, 'archive-title-%u', 'archive-body-%u');",
                 id,
                 id,
                 id);
        if (!execute_ok(database, sql)) {
            tinydb_close(database);
            return 1;
        }
    }

    if (!repack_real_authoritative_table(database)) {
        tinydb_close(database);
        return 1;
    }

    Table* table = tinydb_table(database);
    const TableSchema* users_schema = tinydb_find_table_schema(table, "users");
    const TableSchema* archive_schema = tinydb_find_table_schema(table, "archive");
    if (users_schema == NULL || archive_schema == NULL ||
        users_schema->root_page_num == archive_schema->root_page_num) {
        fprintf(stderr, "catalog roots are not independent\n");
        tinydb_close(database);
        return 1;
    }
    if (archive_schema->num_columns != 3 || archive_schema->row_size != 516 ||
        archive_schema->columns[0].offset != 0 ||
        archive_schema->columns[1].offset != 4 ||
        archive_schema->columns[1].size != 256 ||
        archive_schema->columns[2].offset != 260 ||
        archive_schema->columns[2].size != 256) {
        fprintf(stderr,
                "wide archive schema layout is wrong: columns=%u row_size=%u\n",
                archive_schema->num_columns,
                archive_schema->row_size);
        tinydb_close(database);
        return 1;
    }

    uint32_t archive_root = archive_schema->root_page_num;
    TinyDBTreeStats users_stats;
    TinyDBTreeStats archive_stats;
    if (!tinydb_get_tree_stats(table, "users", &users_stats) ||
        !tinydb_get_tree_stats(table, "archive", &archive_stats)) {
        fprintf(stderr, "unable to read tree stats\n");
        tinydb_close(database);
        return 1;
    }
    if (users_stats.total_rows != 1 ||
        archive_stats.total_rows != 80 ||
        archive_stats.leaf_pages < 2 ||
        archive_stats.internal_pages < 1 ||
        archive_stats.height < 2) {
        fprintf(stderr,
                "unexpected stats users=%u archive=%u leaf=%u internal=%u height=%u\n",
                users_stats.total_rows,
                archive_stats.total_rows,
                archive_stats.leaf_pages,
                archive_stats.internal_pages,
                archive_stats.height);
        tinydb_close(database);
        return 1;
    }

    char diagnostic[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_table_tree(table, "users", diagnostic, sizeof(diagnostic)) ||
        !tinydb_check_table_tree(table, "archive", diagnostic, sizeof(diagnostic)) ||
        !tinydb_check_table_tree(table, "repack_source", diagnostic, sizeof(diagnostic))) {
        fprintf(stderr, "tree integrity failed: %s\n", diagnostic);
        tinydb_close(database);
        return 1;
    }

    tinydb_close(database);
    database = tinydb_open(argv[1]);
    if (database == NULL) {
        fprintf(stderr, "unable to reopen database\n");
        return 1;
    }

    table = tinydb_table(database);
    users_schema = tinydb_find_table_schema(table, "users");
    archive_schema = tinydb_find_table_schema(table, "archive");
    const TableSchema* repack_source_schema =
        tinydb_find_table_schema(table, "repack_source");
    if (users_schema == NULL || archive_schema == NULL || repack_source_schema == NULL ||
        archive_schema->root_page_num != archive_root) {
        fprintf(stderr, "catalog schemas or archive root did not persist across reopen\n");
        tinydb_close(database);
        return 1;
    }
    if (archive_schema->num_columns != 3 || archive_schema->row_size != 516 ||
        archive_schema->columns[1].size != 256 || archive_schema->columns[2].size != 256 ||
        repack_source_schema->row_size != 100u) {
        fprintf(stderr, "wide or repack-source schema layout did not persist across reopen\n");
        tinydb_close(database);
        return 1;
    }
    if (!tinydb_get_tree_stats(table, "archive", &archive_stats) ||
        archive_stats.total_rows != 80) {
        fprintf(stderr, "archive rows did not persist across reopen\n");
        tinydb_close(database);
        return 1;
    }
    if (!tinydb_get_tree_stats(table, "users", &users_stats) ||
        users_stats.total_rows != 1) {
        fprintf(stderr, "users rows changed across reopen\n");
        tinydb_close(database);
        return 1;
    }

    TinyDBTreeStats repack_source_stats;
    if (!tinydb_get_tree_stats(table, "repack_source", &repack_source_stats) ||
        repack_source_stats.total_rows != 120u || repack_source_stats.leaf_pages < 2u) {
        fprintf(stderr, "repack source rows changed across reopen\n");
        tinydb_close(database);
        return 1;
    }

    TinyDBSqlResult archive_query;
    if (tinydb_execute_sql(database,
                           "SELECT * FROM archive WHERE id = 7;",
                           &archive_query) != TINYDB_SQL_SUCCESS) {
        fprintf(stderr, "wide archive query failed after reopen\n");
        tinydb_close(database);
        return 1;
    }

    if (!tinydb_compact_v2_migration_validate_catalog_live_pages(
            &table->catalog, table->pager)) {
        fprintf(stderr, "healthy reopened catalog failed live-page validation\n");
        tinydb_close(database);
        return 1;
    }

    if (table->catalog.num_tables >= MAX_TABLES) {
        fprintf(stderr, "catalog has no room for validation regression fixture\n");
        tinydb_close(database);
        return 1;
    }
    Catalog duplicate_root_catalog = table->catalog;
    duplicate_root_catalog.schemas[duplicate_root_catalog.num_tables] =
        duplicate_root_catalog.schemas[1];
    snprintf(duplicate_root_catalog.schemas[duplicate_root_catalog.num_tables].name,
             sizeof(duplicate_root_catalog.schemas[duplicate_root_catalog.num_tables].name),
             "duplicate_root_fixture");
    duplicate_root_catalog.num_tables++;
    if (tinydb_compact_v2_migration_validate_catalog_live_pages(
            &duplicate_root_catalog, table->pager)) {
        fprintf(stderr, "duplicate live-tree ownership was accepted\n");
        tinydb_close(database);
        return 1;
    }

    printf("ENGINE_API_OK users_root=%u archive_root=%u archive_rows=%u archive_height=%u\n",
           users_stats.root_page_num,
           archive_stats.root_page_num,
           archive_stats.total_rows,
           archive_stats.height);
    printf("WIDE_SCHEMA_REOPEN_OK row_size=%u title_size=%u body_size=%u\n",
           archive_schema->row_size,
           archive_schema->columns[1].size,
           archive_schema->columns[2].size);
    printf("POST_RECOVERY_LIVE_PAGE_VALIDATION_OK\n");
    tinydb_close(database);
    return 0;
}
