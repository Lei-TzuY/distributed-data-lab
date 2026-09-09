#include "vm.h"

#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static double monotonic_seconds(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}

static bool file_exists(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static uint32_t next_random(uint32_t* state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static int insert_rows(Table* table, uint32_t rows, double* elapsed) {
    Statement statement;
    memset(&statement, 0, sizeof(statement));
    statement.type = STATEMENT_BEGIN;
    if (execute_statement(&statement, table) != EXECUTE_SUCCESS) {
        return 1;
    }

    double start = monotonic_seconds();
    for (uint32_t id = 1; id <= rows; id++) {
        memset(&statement, 0, sizeof(statement));
        statement.type = STATEMENT_INSERT;
        statement.row_to_insert.id = id;
        snprintf(statement.row_to_insert.username,
                 sizeof(statement.row_to_insert.username),
                 "user_%u", id);
        snprintf(statement.row_to_insert.email,
                 sizeof(statement.row_to_insert.email),
                 "user_%u@example.com", id);

        ExecuteResult result = execute_statement(&statement, table);
        if (result != EXECUTE_SUCCESS) {
            fprintf(stderr, "insert failed at id=%u (code=%d)\n", id, (int)result);
            memset(&statement, 0, sizeof(statement));
            statement.type = STATEMENT_ROLLBACK;
            (void)execute_statement(&statement, table);
            return 1;
        }
    }

    memset(&statement, 0, sizeof(statement));
    statement.type = STATEMENT_COMMIT;
    if (execute_statement(&statement, table) != EXECUTE_SUCCESS) {
        return 1;
    }
    *elapsed = monotonic_seconds() - start;
    return 0;
}

static int benchmark_point_lookups(Table* table,
                                   uint32_t rows,
                                   uint32_t lookups,
                                   uint32_t* hits,
                                   double* elapsed) {
    uint32_t random_state = 0xC0FFEEu;
    *hits = 0;
    double start = monotonic_seconds();

    for (uint32_t i = 0; i < lookups; i++) {
        uint32_t id = (next_random(&random_state) % rows) + 1u;
        Cursor* cursor = table_find(table, id);
        void* node = get_page(table->pager, cursor->page_num);
        uint32_t cells = *leaf_node_num_cells(node);

        if (cursor->cell_num < cells &&
            *leaf_node_key(node, cursor->cell_num) == id) {
            Row row;
            deserialize_row(cursor_value(cursor), &row);
            if (row.id != id) {
                free(cursor);
                fprintf(stderr, "lookup returned corrupt row for id=%u\n", id);
                return 1;
            }
            (*hits)++;
        }
        free(cursor);
    }

    *elapsed = monotonic_seconds() - start;
    return 0;
}

static uint32_t parse_u32(const char* text, uint32_t fallback) {
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > UINT32_MAX) {
        return fallback;
    }
    return (uint32_t)value;
}

int main(int argc, char** argv) {
    const char* database = argc > 1 ? argv[1] : "tinydb-bench.db";
    uint32_t rows = argc > 2 ? parse_u32(argv[2], 5000u) : 5000u;
    uint32_t lookups = argc > 3 ? parse_u32(argv[3], 20000u) : 20000u;
    bool json_output = argc > 4 && strcmp(argv[4], "--json") == 0;

    if (file_exists(database)) {
        fprintf(stderr,
                "benchmark database already exists: %s\n"
                "Use a new path or remove the old benchmark database first.\n",
                database);
        return 2;
    }

    if (!json_output) {
        printf("TinyDB benchmark\n");
        printf("database=%s rows=%u lookups=%u initial_metadata_capacity=%u dynamic_page_table=yes\n",
               database, rows, lookups, (unsigned)PAGER_INITIAL_CAPACITY);
    }

    Table* table = db_open(database);
    double insert_seconds = 0.0;
    if (insert_rows(table, rows, &insert_seconds) != 0) {
        db_close(table);
        return 1;
    }

    uint32_t hits = 0;
    double lookup_seconds = 0.0;
    uint32_t hits_before = table->pager->cache_hits;
    uint32_t misses_before = table->pager->cache_misses;
    uint32_t evictions_before = table->pager->evictions;

    if (benchmark_point_lookups(table, rows, lookups, &hits, &lookup_seconds) != 0) {
        db_close(table);
        return 1;
    }

    TableStats stats;
    memset(&stats, 0, sizeof(stats));
    db_get_stats(table, &stats);

    double insert_rate = insert_seconds > 0.0 ? (double)rows / insert_seconds : 0.0;
    double lookup_rate = lookup_seconds > 0.0 ? (double)lookups / lookup_seconds : 0.0;
    uint32_t lookup_cache_hits = table->pager->cache_hits - hits_before;
    uint32_t lookup_cache_misses = table->pager->cache_misses - misses_before;
    uint32_t lookup_evictions = table->pager->evictions - evictions_before;
    uint32_t metadata_capacity = pager_metadata_capacity(table->pager);

    int status = (hits == lookups && stats.total_rows == rows) ? 0 : 1;

    if (json_output) {
        printf("{\"rows\":%u,\"lookups\":%u,\"lookup_hits\":%u,"
               "\"dynamic_page_table\":true,\"initial_metadata_capacity\":%u,"
               "\"metadata_capacity\":%u,\"legacy_page_ceiling\":%u,"
               "\"insert_seconds\":%.6f,\"rows_per_sec\":%.2f,"
               "\"lookup_seconds\":%.6f,\"lookups_per_sec\":%.2f,"
               "\"pages\":%u,\"leaf_pages\":%u,\"internal_pages\":%u,"
               "\"cache_hits\":%u,\"cache_misses\":%u,"
               "\"evictions\":%u,\"ok\":%s}\n",
               rows,
               lookups,
               hits,
               (unsigned)PAGER_INITIAL_CAPACITY,
               metadata_capacity,
               (unsigned)TABLE_MAX_PAGES,
               insert_seconds,
               insert_rate,
               lookup_seconds,
               lookup_rate,
               stats.total_pages,
               stats.leaf_pages,
               stats.internal_pages,
               lookup_cache_hits,
               lookup_cache_misses,
               lookup_evictions,
               status == 0 ? "true" : "false");
    } else {
        printf("insert: inserted=%u seconds=%.6f rows_per_sec=%.2f\n",
               rows, insert_seconds, insert_rate);
        printf("lookup: requested=%u lookup_hits=%u seconds=%.6f lookups_per_sec=%.2f\n",
               lookups, hits, lookup_seconds, lookup_rate);
        printf("storage: pages=%u leaf_pages=%u internal_pages=%u rows=%u metadata_capacity=%u\n",
               stats.total_pages,
               stats.leaf_pages,
               stats.internal_pages,
               stats.total_rows,
               metadata_capacity);
        printf("cache_lookup_phase: hits=%u misses=%u evictions=%u\n",
               lookup_cache_hits,
               lookup_cache_misses,
               lookup_evictions);
        printf("%s\n", status == 0 ? "BENCHMARK_OK" : "BENCHMARK_FAILED");
    }

    db_close(table);
    return status;
}
