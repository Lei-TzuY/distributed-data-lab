#include "diagnostics.h"
#include "record.h"

#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    const char* bucket;
    uint32_t matches;
    bool decode_failed;
} ScanContext;

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

static uint32_t parse_u32(const char* text, uint32_t fallback) {
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > UINT32_MAX) {
        return fallback;
    }
    return (uint32_t)value;
}

static uint32_t next_random(uint32_t* state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static TableSchema* find_schema(Table* table, const char* name) {
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static int create_benchmark_schema(Table* table, TableSchema** schema) {
    char column_names[3][32] = {"id", "bucket", "value"};
    char column_types[3][16] = {"INT", "VARCHAR", "INT"};
    if (!table_create_table(table,
                            "bench_records",
                            3u,
                            column_names,
                            column_types,
                            false,
                            NULL,
                            NULL,
                            NULL,
                            false)) {
        return 1;
    }
    *schema = find_schema(table, "bench_records");
    return *schema == NULL ? 1 : 0;
}

static int insert_rows(Table* table,
                       TableSchema* schema,
                       uint32_t rows,
                       double* elapsed) {
    pager_begin_transaction(table->pager);
    table->in_transaction = true;

    double started = monotonic_seconds();
    for (uint32_t id = 1; id <= rows; id++) {
        TinyDBValue values[3];
        memset(values, 0, sizeof(values));
        values[0].type = COL_TYPE_INT;
        values[0].int_value = id;
        values[1].type = COL_TYPE_VARCHAR;
        snprintf(values[1].text,
                 sizeof(values[1].text),
                 "bucket_%u",
                 id % 64u);
        values[2].type = COL_TYPE_INT;
        values[2].int_value = id * 3u;

        char message[TINYDB_RECORD_MESSAGE_MAX];
        if (!tinydb_record_insert(table,
                                  schema,
                                  values,
                                  3u,
                                  message,
                                  sizeof(message))) {
            fprintf(stderr,
                    "generic insert failed at id=%u: %s\n",
                    id,
                    message);
            pager_rollback(table->pager);
            table->in_transaction = false;
            return 1;
        }
    }

    pager_commit(table->pager);
    table->in_transaction = false;
    *elapsed = monotonic_seconds() - started;
    return 0;
}

static int benchmark_point_lookups(Table* table,
                                   const TableSchema* schema,
                                   uint32_t rows,
                                   uint32_t lookups,
                                   uint32_t* hits,
                                   double* elapsed) {
    uint32_t random_state = 0x51A7C0DEu;
    *hits = 0;
    double started = monotonic_seconds();

    for (uint32_t i = 0; i < lookups; i++) {
        uint32_t id = (next_random(&random_state) % rows) + 1u;
        TinyDBRecord record;
        if (!tinydb_record_find(table, schema, id, &record)) continue;

        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t count = 0;
        char message[TINYDB_RECORD_MESSAGE_MAX];
        if (!tinydb_record_decode(schema,
                                  &record,
                                  values,
                                  MAX_COLUMNS_PER_TABLE,
                                  &count,
                                  message,
                                  sizeof(message)) ||
            count != 3u ||
            values[0].int_value != id ||
            values[2].int_value != id * 3u) {
            fprintf(stderr, "generic lookup decode mismatch at id=%u\n", id);
            return 1;
        }
        (*hits)++;
    }

    *elapsed = monotonic_seconds() - started;
    return 0;
}

static bool scan_bucket_record(const TableSchema* schema,
                               const TinyDBRecord* record,
                               void* raw_context) {
    ScanContext* context = (ScanContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!tinydb_record_decode(schema,
                              record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &count,
                              message,
                              sizeof(message)) ||
        count != 3u) {
        context->decode_failed = true;
        return false;
    }
    if (strcmp(values[1].text, context->bucket) == 0) {
        context->matches++;
    }
    return true;
}

static uint32_t expected_bucket_matches(uint32_t rows, uint32_t bucket) {
    uint32_t matches = 0;
    for (uint32_t id = 1; id <= rows; id++) {
        if (id % 64u == bucket) matches++;
    }
    return matches;
}

static int benchmark_schema_scans(Table* table,
                                  const TableSchema* schema,
                                  uint32_t rows,
                                  uint32_t scan_rounds,
                                  uint32_t* matches,
                                  double* elapsed) {
    const char* target = "bucket_7";
    uint32_t expected_each = expected_bucket_matches(rows, 7u);
    *matches = 0;
    double started = monotonic_seconds();

    for (uint32_t round = 0; round < scan_rounds; round++) {
        ScanContext context;
        memset(&context, 0, sizeof(context));
        context.bucket = target;
        uint32_t visited = tinydb_record_scan(table,
                                              schema,
                                              scan_bucket_record,
                                              &context);
        if (context.decode_failed ||
            visited != rows ||
            context.matches != expected_each) {
            fprintf(stderr,
                    "generic scan mismatch at round=%u visited=%u matches=%u expected=%u\n",
                    round,
                    visited,
                    context.matches,
                    expected_each);
            return 1;
        }
        *matches += context.matches;
    }

    *elapsed = monotonic_seconds() - started;
    return 0;
}

int main(int argc, char** argv) {
    const char* database = argc > 1 ? argv[1] : "tinydb-generic-bench.db";
    uint32_t rows = argc > 2 ? parse_u32(argv[2], 5000u) : 5000u;
    uint32_t lookups = argc > 3 ? parse_u32(argv[3], 20000u) : 20000u;
    uint32_t scan_rounds = argc > 4 ? parse_u32(argv[4], 20u) : 20u;
    bool json_output = argc > 5 && strcmp(argv[5], "--json") == 0;

    if (file_exists(database)) {
        fprintf(stderr,
                "generic benchmark database already exists: %s\n"
                "Use a new path or remove the old benchmark database first.\n",
                database);
        return 2;
    }

    Table* table = db_open(database);
    TableSchema* schema = NULL;
    if (table == NULL || create_benchmark_schema(table, &schema) != 0) {
        fprintf(stderr, "unable to create generic benchmark schema\n");
        if (table != NULL) db_close(table);
        return 1;
    }

    if (!json_output) {
        printf("TinyDB generic record benchmark\n");
        printf("database=%s rows=%u lookups=%u scan_rounds=%u row_size=%u slot_size=%u\n",
               database,
               rows,
               lookups,
               scan_rounds,
               schema->row_size,
               (unsigned)ROW_SIZE);
    }

    double insert_seconds = 0.0;
    if (insert_rows(table, schema, rows, &insert_seconds) != 0) {
        db_close(table);
        return 1;
    }

    uint32_t lookup_cache_hits_before = table->pager->cache_hits;
    uint32_t lookup_cache_misses_before = table->pager->cache_misses;
    uint32_t lookup_evictions_before = table->pager->evictions;
    uint32_t lookup_hits = 0;
    double lookup_seconds = 0.0;
    if (benchmark_point_lookups(table,
                                schema,
                                rows,
                                lookups,
                                &lookup_hits,
                                &lookup_seconds) != 0) {
        db_close(table);
        return 1;
    }
    uint32_t lookup_cache_hits = table->pager->cache_hits - lookup_cache_hits_before;
    uint32_t lookup_cache_misses = table->pager->cache_misses - lookup_cache_misses_before;
    uint32_t lookup_evictions = table->pager->evictions - lookup_evictions_before;

    uint32_t scan_cache_hits_before = table->pager->cache_hits;
    uint32_t scan_cache_misses_before = table->pager->cache_misses;
    uint32_t scan_evictions_before = table->pager->evictions;
    uint32_t scan_matches = 0;
    double scan_seconds = 0.0;
    if (benchmark_schema_scans(table,
                               schema,
                               rows,
                               scan_rounds,
                               &scan_matches,
                               &scan_seconds) != 0) {
        db_close(table);
        return 1;
    }
    uint32_t scan_cache_hits = table->pager->cache_hits - scan_cache_hits_before;
    uint32_t scan_cache_misses = table->pager->cache_misses - scan_cache_misses_before;
    uint32_t scan_evictions = table->pager->evictions - scan_evictions_before;

    TinyDBTreeStats stats;
    memset(&stats, 0, sizeof(stats));
    bool stats_ok = tinydb_get_tree_stats(table, schema->name, &stats);

    double insert_rate = insert_seconds > 0.0 ? (double)rows / insert_seconds : 0.0;
    double lookup_rate = lookup_seconds > 0.0 ? (double)lookups / lookup_seconds : 0.0;
    uint64_t scanned_records = (uint64_t)rows * (uint64_t)scan_rounds;
    double scan_rate = scan_seconds > 0.0
        ? (double)scanned_records / scan_seconds
        : 0.0;
    double slot_utilization = 100.0 * (double)schema->row_size / (double)ROW_SIZE;
    uint32_t expected_matches = expected_bucket_matches(rows, 7u) * scan_rounds;

    int status = lookup_hits == lookups &&
                 scan_matches == expected_matches &&
                 stats_ok &&
                 stats.total_rows == rows
        ? 0
        : 1;

    if (json_output) {
        printf("{\"rows\":%u,\"lookups\":%u,\"scan_rounds\":%u,"
               "\"schema_row_size\":%u,\"leaf_value_slot_size\":%u,"
               "\"slot_utilization_pct\":%.2f,"
               "\"insert_seconds\":%.6f,\"rows_per_sec\":%.2f,"
               "\"lookup_hits\":%u,\"lookup_seconds\":%.6f,"
               "\"lookups_per_sec\":%.2f,"
               "\"scan_records\":%llu,\"scan_matches\":%u,"
               "\"scan_seconds\":%.6f,\"scan_records_per_sec\":%.2f,"
               "\"root_page\":%u,\"leaf_pages\":%u,\"internal_pages\":%u,"
               "\"lookup_cache_hits\":%u,\"lookup_cache_misses\":%u,"
               "\"lookup_evictions\":%u,\"scan_cache_hits\":%u,"
               "\"scan_cache_misses\":%u,\"scan_evictions\":%u,"
               "\"metadata_capacity\":%u,\"ok\":%s}\n",
               rows,
               lookups,
               scan_rounds,
               schema->row_size,
               (unsigned)ROW_SIZE,
               slot_utilization,
               insert_seconds,
               insert_rate,
               lookup_hits,
               lookup_seconds,
               lookup_rate,
               (unsigned long long)scanned_records,
               scan_matches,
               scan_seconds,
               scan_rate,
               stats.root_page_num,
               stats.leaf_pages,
               stats.internal_pages,
               lookup_cache_hits,
               lookup_cache_misses,
               lookup_evictions,
               scan_cache_hits,
               scan_cache_misses,
               scan_evictions,
               pager_metadata_capacity(table->pager),
               status == 0 ? "true" : "false");
    } else {
        printf("insert: inserted=%u seconds=%.6f rows_per_sec=%.2f\n",
               rows,
               insert_seconds,
               insert_rate);
        printf("lookup: requested=%u hits=%u seconds=%.6f lookups_per_sec=%.2f\n",
               lookups,
               lookup_hits,
               lookup_seconds,
               lookup_rate);
        printf("scan: rounds=%u records=%llu matches=%u seconds=%.6f records_per_sec=%.2f\n",
               scan_rounds,
               (unsigned long long)scanned_records,
               scan_matches,
               scan_seconds,
               scan_rate);
        printf("storage: root_page=%u leaf_pages=%u internal_pages=%u rows=%u metadata_capacity=%u\n",
               stats.root_page_num,
               stats.leaf_pages,
               stats.internal_pages,
               stats.total_rows,
               pager_metadata_capacity(table->pager));
        printf("layout: schema_row_size=%u slot_size=%u slot_utilization_pct=%.2f\n",
               schema->row_size,
               (unsigned)ROW_SIZE,
               slot_utilization);
        printf("lookup_cache: hits=%u misses=%u evictions=%u\n",
               lookup_cache_hits,
               lookup_cache_misses,
               lookup_evictions);
        printf("scan_cache: hits=%u misses=%u evictions=%u\n",
               scan_cache_hits,
               scan_cache_misses,
               scan_evictions);
        printf("%s\n",
               status == 0
                   ? "GENERIC_BENCHMARK_OK"
                   : "GENERIC_BENCHMARK_FAILED");
    }

    db_close(table);
    return status;
}
