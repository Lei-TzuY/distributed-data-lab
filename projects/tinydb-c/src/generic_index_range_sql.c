#include "generic_index_epoch.h"
#include "generic_predicate.h"
#include "generic_sql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define RANGE_INDEX_MAGIC 0x47495231u
#define RANGE_INDEX_VERSION 1u
#define RANGE_INDEX_CACHE_SLOTS (MAX_TABLES * MAX_INDEXES)
#define RANGE_FNV_OFFSET 1469598103934665603ULL
#define RANGE_FNV_PRIME 1099511628211ULL

typedef enum {
    RANGE_PROJECTION_STAR = 0,
    RANGE_PROJECTION_COUNT,
    RANGE_PROJECTION_COLUMN
} RangeProjectionKind;

typedef struct {
    uint32_t int_key;
    char text_key[TINYDB_RECORD_TEXT_MAX + 1];
    uint32_t primary_key;
} RangeIndexEntry;

typedef struct {
    bool used;
    bool dirty;
    char database_filename[512];
    char table_name[MAX_NAME_SIZE];
    char index_name[MAX_NAME_SIZE];
    char column_name[MAX_NAME_SIZE];
    uint32_t root_page_num;
    uint32_t column_index;
    ColumnType column_type;
    uint64_t epoch;
    RangeIndexEntry* entries;
    uint32_t count;
    uint32_t capacity;
} RangeIndexCache;

typedef struct {
    TableSchema* schema;
    GenericSecondaryIndex* index;
    TinyDBGenericPredicate predicate;
    RangeProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} IndexedRangeSelect;

typedef struct {
    const IndexedRangeSelect* select;
    RangeIndexCache* cache;
    bool failed;
} RangeBuildContext;

static RangeIndexCache range_caches[RANGE_INDEX_CACHE_SLOTS];

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_snapshot_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_compound_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_compound_base(
    const TinyDBGenericSelectPlan* plan);

static void initialize_result(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_NOT_APPLICABLE;
    result->execute_result = EXECUTE_SUCCESS;
}

static TinyDBGenericSqlStatus success(TinyDBGenericSqlResult* result) {
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
}

static TinyDBGenericSqlStatus execute_error(TinyDBGenericSqlResult* result,
                                             const char* message) {
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_KEY_NOT_FOUND;
    snprintf(result->message, sizeof(result->message), "%s", message);
    return result->status;
}

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (ci_char(*left) != ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool is_legacy_fixed_row_schema(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3 &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static GenericSecondaryIndex* find_single_column_index(
    Table* table,
    const TableSchema* schema,
    uint32_t column_index) {
    if (schema == NULL || column_index == 0 || column_index >= schema->num_columns) {
        return NULL;
    }
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && index->num_columns == 1 &&
            ci_equal(index->table_name, schema->name) &&
            ci_equal(index->column_name, schema->columns[column_index].name)) {
            return index;
        }
    }
    return NULL;
}

static bool parse_projection(TinyDBGenericParser* parser,
                             const TableSchema* schema,
                             IndexedRangeSelect* select) {
    if (tinydb_generic_consume_char(parser, '*')) {
        select->projection_kind = RANGE_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        select->projection_kind = RANGE_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = tinydb_generic_find_column_index(schema, column);
    if (column_index < 0) return false;
    select->projection_kind = RANGE_PROJECTION_COLUMN;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool parse_range_select(Table* table,
                               const char* sql,
                               IndexedRangeSelect* select) {
    memset(select, 0, sizeof(*select));

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) return false;

    TinyDBGenericParser routing = parser;
    if (tinydb_generic_consume_char(&routing, '*')) {
        /* projection consumed */
    } else {
        TinyDBGenericParser backup = routing;
        if (!(tinydb_generic_consume_word(&routing, "count") &&
              tinydb_generic_consume_char(&routing, '(') &&
              tinydb_generic_consume_char(&routing, '*') &&
              tinydb_generic_consume_char(&routing, ')'))) {
            routing = backup;
            char ignored[MAX_NAME_SIZE];
            if (!tinydb_generic_parse_identifier(&routing,
                                                 ignored,
                                                 sizeof(ignored))) {
                return false;
            }
        }
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_consume_word(&routing, "from") ||
        !tinydb_generic_parse_identifier(&routing,
                                         table_name,
                                         sizeof(table_name))) {
        return false;
    }

    TableSchema* schema = find_schema(table, table_name);
    if (schema == NULL || is_legacy_fixed_row_schema(schema)) return false;
    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return false;
    }

    select->schema = schema;
    if (!parse_projection(&parser, schema, select) ||
        !tinydb_generic_consume_word(&parser, "from") ||
        !tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name)) ||
        !ci_equal(table_name, schema->name) ||
        !tinydb_generic_consume_word(&parser, "where") ||
        !tinydb_generic_parse_predicate(&parser, schema, &select->predicate)) {
        return false;
    }

    if (select->predicate.op == TINYDB_GENERIC_COMPARE_EQ ||
        select->predicate.column_index == 0) {
        return false;
    }

    select->index = find_single_column_index(table,
                                              schema,
                                              select->predicate.column_index);
    if (select->index == NULL) return false;

    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->limit)) return false;
        select->has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset")) {
            if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
    }

    return tinydb_generic_consume_end(&parser);
}

static uint64_t fnv_update(uint64_t hash, const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= RANGE_FNV_PRIME;
    }
    return hash;
}

static void encode_u32(uint32_t value, unsigned char out[4]) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
    out[2] = (unsigned char)((value >> 16) & 0xffu);
    out[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void encode_u64(uint64_t value, unsigned char out[8]) {
    for (uint32_t i = 0; i < 8; i++) {
        out[i] = (unsigned char)((value >> (i * 8u)) & 0xffu);
    }
}

static uint32_t decode_u32(const unsigned char in[4]) {
    return ((uint32_t)in[0]) |
           ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static uint64_t decode_u64(const unsigned char in[8]) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; i++) value |= ((uint64_t)in[i]) << (i * 8u);
    return value;
}

static bool write_bytes(FILE* file,
                        uint64_t* hash,
                        const void* data,
                        size_t size) {
    if (fwrite(data, 1, size, file) != size) return false;
    if (hash != NULL) *hash = fnv_update(*hash, data, size);
    return true;
}

static bool read_bytes(FILE* file,
                       uint64_t* hash,
                       void* data,
                       size_t size) {
    if (fread(data, 1, size, file) != size) return false;
    if (hash != NULL) *hash = fnv_update(*hash, data, size);
    return true;
}

static bool write_u32(FILE* file, uint64_t* hash, uint32_t value) {
    unsigned char bytes[4];
    encode_u32(value, bytes);
    return write_bytes(file, hash, bytes, sizeof(bytes));
}

static bool write_u64(FILE* file, uint64_t* hash, uint64_t value) {
    unsigned char bytes[8];
    encode_u64(value, bytes);
    return write_bytes(file, hash, bytes, sizeof(bytes));
}

static bool read_u32(FILE* file, uint64_t* hash, uint32_t* value) {
    unsigned char bytes[4];
    if (!read_bytes(file, hash, bytes, sizeof(bytes))) return false;
    *value = decode_u32(bytes);
    return true;
}

static bool read_u64(FILE* file, uint64_t* hash, uint64_t* value) {
    unsigned char bytes[8];
    if (!read_bytes(file, hash, bytes, sizeof(bytes))) return false;
    *value = decode_u64(bytes);
    return true;
}

static bool sync_file(FILE* file) {
    if (fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static bool range_filename(const GenericSecondaryIndex* index,
                           char* output,
                           size_t output_size) {
    int written = snprintf(output,
                           output_size,
                           "%s.range",
                           index->index_filename);
    return written >= 0 && (size_t)written < output_size;
}

static bool reserve_entries(RangeIndexCache* cache, uint32_t needed) {
    if (needed <= cache->capacity) return true;
    uint32_t capacity = cache->capacity == 0 ? 32u : cache->capacity;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    RangeIndexEntry* grown = (RangeIndexEntry*)realloc(
        cache->entries, (size_t)capacity * sizeof(RangeIndexEntry));
    if (grown == NULL) return false;
    cache->entries = grown;
    cache->capacity = capacity;
    return true;
}

static void set_cache_identity(RangeIndexCache* cache,
                               Table* table,
                               const IndexedRangeSelect* select) {
    cache->used = true;
    cache->dirty = true;
    cache->count = 0;
    cache->epoch = 0;
    cache->root_page_num = select->schema->root_page_num;
    cache->column_index = select->predicate.column_index;
    cache->column_type = select->schema->columns[select->predicate.column_index].type;
    snprintf(cache->database_filename,
             sizeof(cache->database_filename),
             "%s",
             table->pager->filename);
    snprintf(cache->table_name,
             sizeof(cache->table_name),
             "%s",
             select->schema->name);
    snprintf(cache->index_name,
             sizeof(cache->index_name),
             "%s",
             select->index->name);
    snprintf(cache->column_name,
             sizeof(cache->column_name),
             "%s",
             select->schema->columns[select->predicate.column_index].name);
}

static RangeIndexCache* find_cache(Table* table,
                                   const IndexedRangeSelect* select) {
    RangeIndexCache* free_slot = NULL;
    for (size_t i = 0; i < RANGE_INDEX_CACHE_SLOTS; i++) {
        RangeIndexCache* cache = &range_caches[i];
        if (!cache->used) {
            if (free_slot == NULL) free_slot = cache;
            continue;
        }
        if (strcmp(cache->database_filename, table->pager->filename) == 0 &&
            ci_equal(cache->index_name, select->index->name)) {
            if (cache->root_page_num != select->schema->root_page_num ||
                cache->column_index != select->predicate.column_index ||
                cache->column_type !=
                    select->schema->columns[select->predicate.column_index].type ||
                !ci_equal(cache->table_name, select->schema->name) ||
                !ci_equal(cache->column_name,
                          select->schema->columns[select->predicate.column_index].name)) {
                set_cache_identity(cache, table, select);
            }
            return cache;
        }
    }
    if (free_slot == NULL) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    set_cache_identity(free_slot, table, select);
    return free_slot;
}

static int compare_int_entries(const void* left, const void* right) {
    const RangeIndexEntry* a = (const RangeIndexEntry*)left;
    const RangeIndexEntry* b = (const RangeIndexEntry*)right;
    if (a->int_key < b->int_key) return -1;
    if (a->int_key > b->int_key) return 1;
    if (a->primary_key < b->primary_key) return -1;
    if (a->primary_key > b->primary_key) return 1;
    return 0;
}

static int compare_text_entries(const void* left, const void* right) {
    const RangeIndexEntry* a = (const RangeIndexEntry*)left;
    const RangeIndexEntry* b = (const RangeIndexEntry*)right;
    int compared = strcmp(a->text_key, b->text_key);
    if (compared != 0) return compared;
    if (a->primary_key < b->primary_key) return -1;
    if (a->primary_key > b->primary_key) return 1;
    return 0;
}

static bool build_entry(const TableSchema* schema,
                        const TinyDBRecord* record,
                        void* raw_context) {
    RangeBuildContext* context = (RangeBuildContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_decode(schema,
                              record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &value_count,
                              message,
                              sizeof(message)) ||
        value_count != schema->num_columns ||
        !reserve_entries(context->cache, context->cache->count + 1u)) {
        context->failed = true;
        return false;
    }

    RangeIndexEntry* entry = &context->cache->entries[context->cache->count++];
    memset(entry, 0, sizeof(*entry));
    const TinyDBValue* key = &values[context->select->predicate.column_index];
    if (key->type == COL_TYPE_INT) {
        entry->int_key = key->int_value;
    } else {
        snprintf(entry->text_key, sizeof(entry->text_key), "%s", key->text);
    }
    entry->primary_key = values[0].int_value;
    return true;
}

static bool write_snapshot(const IndexedRangeSelect* select,
                           const RangeIndexCache* cache,
                           uint64_t epoch) {
    char filename[600];
    if (!range_filename(select->index, filename, sizeof(filename))) return false;
    FILE* file = fopen(filename, "wb");
    if (file == NULL) return false;

    uint64_t hash = RANGE_FNV_OFFSET;
    char table_name[MAX_NAME_SIZE] = {0};
    char index_name[MAX_NAME_SIZE] = {0};
    char column_name[MAX_NAME_SIZE] = {0};
    snprintf(table_name, sizeof(table_name), "%s", select->schema->name);
    snprintf(index_name, sizeof(index_name), "%s", select->index->name);
    snprintf(column_name,
             sizeof(column_name),
             "%s",
             select->schema->columns[select->predicate.column_index].name);

    bool ok = write_u32(file, &hash, RANGE_INDEX_MAGIC) &&
              write_u32(file, &hash, RANGE_INDEX_VERSION) &&
              write_u64(file, &hash, epoch) &&
              write_u32(file, &hash, select->schema->root_page_num) &&
              write_u32(file, &hash, select->predicate.column_index) &&
              write_u32(file,
                        &hash,
                        (uint32_t)select->schema->columns[
                            select->predicate.column_index].type) &&
              write_u32(file, &hash, cache->count) &&
              write_bytes(file, &hash, table_name, sizeof(table_name)) &&
              write_bytes(file, &hash, index_name, sizeof(index_name)) &&
              write_bytes(file, &hash, column_name, sizeof(column_name));

    for (uint32_t i = 0; ok && i < cache->count; i++) {
        if (cache->column_type == COL_TYPE_INT) {
            ok = write_u32(file, &hash, cache->entries[i].int_key);
        } else {
            ok = write_bytes(file,
                             &hash,
                             cache->entries[i].text_key,
                             sizeof(cache->entries[i].text_key));
        }
        ok = ok && write_u32(file, &hash, cache->entries[i].primary_key);
    }
    ok = ok && write_u64(file, NULL, hash);
    if (ok) ok = sync_file(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool load_snapshot(const IndexedRangeSelect* select,
                          RangeIndexCache* cache,
                          uint64_t expected_epoch) {
    char filename[600];
    if (!range_filename(select->index, filename, sizeof(filename))) return false;
    FILE* file = fopen(filename, "rb");
    if (file == NULL) return false;

    uint64_t hash = RANGE_FNV_OFFSET;
    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t epoch = 0;
    uint32_t root_page_num = 0;
    uint32_t column_index = 0;
    uint32_t column_type = 0;
    uint32_t count = 0;
    char table_name[MAX_NAME_SIZE] = {0};
    char index_name[MAX_NAME_SIZE] = {0};
    char column_name[MAX_NAME_SIZE] = {0};

    bool ok = read_u32(file, &hash, &magic) &&
              read_u32(file, &hash, &version) &&
              read_u64(file, &hash, &epoch) &&
              read_u32(file, &hash, &root_page_num) &&
              read_u32(file, &hash, &column_index) &&
              read_u32(file, &hash, &column_type) &&
              read_u32(file, &hash, &count) &&
              read_bytes(file, &hash, table_name, sizeof(table_name)) &&
              read_bytes(file, &hash, index_name, sizeof(index_name)) &&
              read_bytes(file, &hash, column_name, sizeof(column_name));

    table_name[MAX_NAME_SIZE - 1] = '\0';
    index_name[MAX_NAME_SIZE - 1] = '\0';
    column_name[MAX_NAME_SIZE - 1] = '\0';

    if (!ok || magic != RANGE_INDEX_MAGIC || version != RANGE_INDEX_VERSION ||
        epoch != expected_epoch || root_page_num != select->schema->root_page_num ||
        column_index != select->predicate.column_index ||
        column_type != (uint32_t)select->schema->columns[
                           select->predicate.column_index].type ||
        !ci_equal(table_name, select->schema->name) ||
        !ci_equal(index_name, select->index->name) ||
        !ci_equal(column_name,
                  select->schema->columns[select->predicate.column_index].name) ||
        !reserve_entries(cache, count)) {
        fclose(file);
        cache->count = 0;
        return false;
    }

    cache->count = 0;
    for (uint32_t i = 0; ok && i < count; i++) {
        RangeIndexEntry* entry = &cache->entries[i];
        memset(entry, 0, sizeof(*entry));
        if ((ColumnType)column_type == COL_TYPE_INT) {
            ok = read_u32(file, &hash, &entry->int_key);
        } else {
            ok = read_bytes(file,
                            &hash,
                            entry->text_key,
                            sizeof(entry->text_key));
            entry->text_key[sizeof(entry->text_key) - 1] = '\0';
        }
        ok = ok && read_u32(file, &hash, &entry->primary_key);
        if (ok) cache->count++;
    }

    uint64_t stored_hash = 0;
    ok = ok && read_u64(file, NULL, &stored_hash) && stored_hash == hash;
    if (ok) ok = fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!ok) {
        cache->count = 0;
        return false;
    }

    cache->epoch = epoch;
    cache->dirty = false;
    return true;
}

static bool ensure_cache(Table* table,
                         const IndexedRangeSelect* select,
                         RangeIndexCache** cache_out) {
    RangeIndexCache* cache = find_cache(table, select);
    if (cache == NULL) return false;

    uint64_t epoch = 0;
    if (!tinydb_generic_index_epoch_current(table, &epoch)) return false;
    if (cache->epoch != epoch) cache->dirty = true;

    if (cache->dirty && !table->in_transaction &&
        load_snapshot(select, cache, epoch)) {
        *cache_out = cache;
        return true;
    }

    if (cache->dirty) {
        cache->count = 0;
        RangeBuildContext context;
        memset(&context, 0, sizeof(context));
        context.select = select;
        context.cache = cache;
        (void)tinydb_record_scan(table, select->schema, build_entry, &context);
        if (context.failed) {
            cache->count = 0;
            return false;
        }
        if (cache->count > 1) {
            qsort(cache->entries,
                  cache->count,
                  sizeof(RangeIndexEntry),
                  cache->column_type == COL_TYPE_INT
                      ? compare_int_entries
                      : compare_text_entries);
        }
        cache->epoch = epoch;
        cache->dirty = table->in_transaction;
        if (!table->in_transaction) (void)write_snapshot(select, cache, epoch);
    }

    *cache_out = cache;
    return true;
}

static int compare_entry_value(const RangeIndexCache* cache,
                               const RangeIndexEntry* entry,
                               const TinyDBValue* value) {
    if (cache->column_type == COL_TYPE_INT) {
        if (entry->int_key < value->int_value) return -1;
        if (entry->int_key > value->int_value) return 1;
        return 0;
    }
    int compared = strcmp(entry->text_key, value->text);
    if (compared < 0) return -1;
    if (compared > 0) return 1;
    return 0;
}

static size_t lower_bound(const RangeIndexCache* cache,
                          const TinyDBValue* value) {
    size_t min = 0;
    size_t max = cache->count;
    while (min < max) {
        size_t mid = min + (max - min) / 2u;
        if (compare_entry_value(cache, &cache->entries[mid], value) < 0) {
            min = mid + 1u;
        } else {
            max = mid;
        }
    }
    return min;
}

static size_t upper_bound(const RangeIndexCache* cache,
                          const TinyDBValue* value) {
    size_t min = 0;
    size_t max = cache->count;
    while (min < max) {
        size_t mid = min + (max - min) / 2u;
        if (compare_entry_value(cache, &cache->entries[mid], value) <= 0) {
            min = mid + 1u;
        } else {
            max = mid;
        }
    }
    return min;
}

static void range_bounds(const RangeIndexCache* cache,
                         const TinyDBGenericPredicate* predicate,
                         size_t* start,
                         size_t* end) {
    *start = 0;
    *end = cache->count;
    switch (predicate->op) {
        case TINYDB_GENERIC_COMPARE_GT:
            *start = upper_bound(cache, &predicate->value);
            break;
        case TINYDB_GENERIC_COMPARE_GTE:
            *start = lower_bound(cache, &predicate->value);
            break;
        case TINYDB_GENERIC_COMPARE_LT:
            *end = lower_bound(cache, &predicate->value);
            break;
        case TINYDB_GENERIC_COMPARE_LTE:
            *end = upper_bound(cache, &predicate->value);
            break;
        case TINYDB_GENERIC_COMPARE_EQ:
            *start = lower_bound(cache, &predicate->value);
            *end = upper_bound(cache, &predicate->value);
            break;
    }
}

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static TinyDBGenericSqlStatus execute_range_index(
    Table* table,
    const IndexedRangeSelect* select,
    TinyDBGenericSqlResult* result) {
    RangeIndexCache* cache = NULL;
    if (!ensure_cache(table, select, &cache)) {
        return execute_error(result,
                             "unable to load or rebuild persistent generic range index");
    }

    size_t start = 0;
    size_t end = 0;
    range_bounds(cache, &select->predicate, &start, &end);
    uint32_t matched = 0;
    uint32_t emitted = 0;

    for (size_t i = start; i < end; i++) {
        TinyDBRecord record;
        if (!tinydb_record_find(table,
                                select->schema,
                                cache->entries[i].primary_key,
                                &record)) {
            continue;
        }

        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t value_count = 0;
        char message[TINYDB_RECORD_MESSAGE_MAX];
        if (!tinydb_record_decode(select->schema,
                                  &record,
                                  values,
                                  MAX_COLUMNS_PER_TABLE,
                                  &value_count,
                                  message,
                                  sizeof(message)) ||
            value_count != select->schema->num_columns) {
            return execute_error(result, "unable to decode range-index candidate");
        }
        if (!tinydb_generic_predicate_matches(
                &select->predicate,
                &values[select->predicate.column_index])) {
            continue;
        }

        matched++;
        if (select->projection_kind == RANGE_PROJECTION_COUNT) continue;
        if (matched <= select->offset) continue;
        if (select->has_limit && emitted >= select->limit) break;

        if (select->projection_kind == RANGE_PROJECTION_COLUMN) {
            print_value(&values[select->projection_column_index]);
        } else {
            tinydb_record_print(select->schema, &record);
        }
        emitted++;
    }

    if (select->projection_kind == RANGE_PROJECTION_COUNT) {
        uint32_t count = matched;
        if (select->offset > 0) count = 0;
        if (select->has_limit && select->limit == 0) count = 0;
        printf("%u\n", count);
    }
    return success(result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    IndexedRangeSelect select;
    if (parse_range_select(table, sql, &select)) {
        return execute_range_index(table, &select, output);
    }
    return tinydb_generic_sql_try_execute_snapshot_base(table, sql, output);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlStatus status = tinydb_generic_sql_build_select_plan_compound_base(
        table, sql, plan, result);
    if (status != TINYDB_GENERIC_SQL_SUCCESS || plan == NULL ||
        !plan->applicable || plan->kind != TINYDB_GENERIC_PLAN_FULL_SCAN ||
        !plan->has_filter || plan->filter_expression[0] != '\0') {
        return status;
    }

    const char* op = plan->filter_operator;
    if (strcmp(op, ">") != 0 && strcmp(op, ">=") != 0 &&
        strcmp(op, "<") != 0 && strcmp(op, "<=") != 0) {
        return status;
    }

    TableSchema* schema = find_schema(table, plan->table_name);
    if (schema == NULL || is_legacy_fixed_row_schema(schema)) return status;
    int column_index = tinydb_generic_find_column_index(schema, plan->filter_column);
    if (column_index <= 0) return status;

    GenericSecondaryIndex* index = find_single_column_index(
        table, schema, (uint32_t)column_index);
    if (index == NULL) return status;

    plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RANGE;
    snprintf(plan->index_name, sizeof(plan->index_name), "%s", index->name);
    return status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable ||
        plan->kind != TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RANGE) {
        tinydb_generic_sql_print_plan_compound_base(plan);
        return;
    }

    printf("PLAN: GENERIC SECONDARY INDEX RANGE SCAN\n");
    printf("  TABLE: %s (root page %u)\n",
           plan->table_name,
           plan->root_page_num);
    printf("  INDEX: %s\n", plan->index_name);
    printf("  PROJECTION: %s\n", plan->projection);
    printf("  FILTER: %s %s %s\n",
           plan->filter_column,
           plan->filter_operator,
           plan->filter_value);
}
