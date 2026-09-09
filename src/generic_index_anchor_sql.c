#include "generic_index_epoch.h"
#include "generic_predicate.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define ANCHOR_INDEX_MAGIC 0x47495231u /* GIR1: shared with typed range snapshot */
#define ANCHOR_INDEX_VERSION 1u
#define ANCHOR_FNV_OFFSET 1469598103934665603ULL
#define ANCHOR_FNV_PRIME 1099511628211ULL

typedef enum {
    ANCHOR_PROJECTION_STAR = 0,
    ANCHOR_PROJECTION_COUNT,
    ANCHOR_PROJECTION_COLUMN
} AnchorProjectionKind;

typedef struct {
    uint32_t int_key;
    char text_key[TINYDB_RECORD_TEXT_MAX + 1];
    uint32_t primary_key;
} AnchorIndexEntry;

typedef struct {
    AnchorIndexEntry* entries;
    uint32_t count;
    uint32_t capacity;
    ColumnType column_type;
} AnchorSnapshot;

typedef struct {
    TableSchema* schema;
    TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t predicate_count;
    uint32_t anchor_predicate_index;
    GenericSecondaryIndex* index;
    AnchorProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} AnchoredSelect;

typedef struct {
    const AnchoredSelect* select;
    AnchorSnapshot* snapshot;
    bool failed;
} AnchorBuildContext;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_range_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_range_index_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_range_index_base(
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

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
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
                             AnchoredSelect* select) {
    if (tinydb_generic_consume_char(parser, '*')) {
        select->projection_kind = ANCHOR_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        select->projection_kind = ANCHOR_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = tinydb_generic_find_column_index(schema, column);
    if (column_index < 0) return false;
    select->projection_kind = ANCHOR_PROJECTION_COLUMN;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool has_primary_key_equality(const AnchoredSelect* select) {
    for (uint32_t i = 0; i < select->predicate_count; i++) {
        if (select->predicates[i].column_index == 0 &&
            select->predicates[i].op == TINYDB_GENERIC_COMPARE_EQ) {
            return true;
        }
    }
    return false;
}

static bool choose_anchor(Table* table, AnchoredSelect* select) {
    GenericSecondaryIndex* best_index = NULL;
    uint32_t best_predicate = 0;
    uint32_t best_score = 0;

    for (uint32_t i = 0; i < select->predicate_count; i++) {
        TinyDBGenericPredicate* predicate = &select->predicates[i];
        GenericSecondaryIndex* index = find_single_column_index(
            table, select->schema, predicate->column_index);
        if (index == NULL) continue;

        uint32_t score = predicate->op == TINYDB_GENERIC_COMPARE_EQ ? 2u : 1u;
        if (score > best_score) {
            best_score = score;
            best_index = index;
            best_predicate = i;
        }
    }

    if (best_index == NULL) return false;
    select->index = best_index;
    select->anchor_predicate_index = best_predicate;
    return true;
}

static bool parse_anchored_select(Table* table,
                                  const char* sql,
                                  AnchoredSelect* select) {
    memset(select, 0, sizeof(*select));
    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "select")) return false;

    /* Resolve table first so projection and predicate columns are typed. */
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
        !tinydb_generic_consume_word(&parser, "where")) {
        return false;
    }

    if (!tinydb_generic_parse_predicate(
            &parser, schema, &select->predicates[0])) {
        return false;
    }
    select->predicate_count = 1;

    while (tinydb_generic_consume_word(&parser, "and")) {
        if (select->predicate_count >= MAX_COLUMNS_PER_TABLE ||
            !tinydb_generic_parse_predicate(
                &parser,
                schema,
                &select->predicates[select->predicate_count])) {
            return false;
        }
        select->predicate_count++;
    }

    if (select->predicate_count < 2) return false;

    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->limit)) return false;
        select->has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset")) {
            if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
    }

    if (!tinydb_generic_consume_end(&parser) ||
        has_primary_key_equality(select) ||
        !choose_anchor(table, select)) {
        return false;
    }
    return true;
}

static bool predicates_match(const AnchoredSelect* select,
                             const TinyDBValue* values) {
    for (uint32_t i = 0; i < select->predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (!tinydb_generic_predicate_matches(
                predicate, &values[predicate->column_index])) {
            return false;
        }
    }
    return true;
}

static uint64_t fnv_update(uint64_t hash, const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= ANCHOR_FNV_PRIME;
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

static bool reserve_entries(AnchorSnapshot* snapshot, uint32_t needed) {
    if (needed <= snapshot->capacity) return true;
    uint32_t capacity = snapshot->capacity == 0 ? 32u : snapshot->capacity;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    AnchorIndexEntry* grown = (AnchorIndexEntry*)realloc(
        snapshot->entries, (size_t)capacity * sizeof(AnchorIndexEntry));
    if (grown == NULL) return false;
    snapshot->entries = grown;
    snapshot->capacity = capacity;
    return true;
}

static int compare_int_entries(const void* left, const void* right) {
    const AnchorIndexEntry* a = (const AnchorIndexEntry*)left;
    const AnchorIndexEntry* b = (const AnchorIndexEntry*)right;
    if (a->int_key < b->int_key) return -1;
    if (a->int_key > b->int_key) return 1;
    if (a->primary_key < b->primary_key) return -1;
    if (a->primary_key > b->primary_key) return 1;
    return 0;
}

static int compare_text_entries(const void* left, const void* right) {
    const AnchorIndexEntry* a = (const AnchorIndexEntry*)left;
    const AnchorIndexEntry* b = (const AnchorIndexEntry*)right;
    int compared = strcmp(a->text_key, b->text_key);
    if (compared != 0) return compared;
    if (a->primary_key < b->primary_key) return -1;
    if (a->primary_key > b->primary_key) return 1;
    return 0;
}

static bool build_entry(const TableSchema* schema,
                        const TinyDBRecord* record,
                        void* raw_context) {
    AnchorBuildContext* context = (AnchorBuildContext*)raw_context;
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
        !reserve_entries(context->snapshot, context->snapshot->count + 1u)) {
        context->failed = true;
        return false;
    }

    const TinyDBGenericPredicate* anchor =
        &context->select->predicates[context->select->anchor_predicate_index];
    AnchorIndexEntry* entry =
        &context->snapshot->entries[context->snapshot->count++];
    memset(entry, 0, sizeof(*entry));
    if (values[anchor->column_index].type == COL_TYPE_INT) {
        entry->int_key = values[anchor->column_index].int_value;
    } else {
        snprintf(entry->text_key,
                 sizeof(entry->text_key),
                 "%s",
                 values[anchor->column_index].text);
    }
    entry->primary_key = values[0].int_value;
    return true;
}

static bool write_snapshot(const AnchoredSelect* select,
                           const AnchorSnapshot* snapshot,
                           uint64_t epoch) {
    char filename[600];
    if (!range_filename(select->index, filename, sizeof(filename))) return false;
    FILE* file = fopen(filename, "wb");
    if (file == NULL) return false;

    const TinyDBGenericPredicate* anchor =
        &select->predicates[select->anchor_predicate_index];
    char table_name[MAX_NAME_SIZE] = {0};
    char index_name[MAX_NAME_SIZE] = {0};
    char column_name[MAX_NAME_SIZE] = {0};
    snprintf(table_name, sizeof(table_name), "%s", select->schema->name);
    snprintf(index_name, sizeof(index_name), "%s", select->index->name);
    snprintf(column_name,
             sizeof(column_name),
             "%s",
             select->schema->columns[anchor->column_index].name);

    uint64_t hash = ANCHOR_FNV_OFFSET;
    bool ok = write_u32(file, &hash, ANCHOR_INDEX_MAGIC) &&
              write_u32(file, &hash, ANCHOR_INDEX_VERSION) &&
              write_u64(file, &hash, epoch) &&
              write_u32(file, &hash, select->schema->root_page_num) &&
              write_u32(file, &hash, anchor->column_index) &&
              write_u32(file,
                        &hash,
                        (uint32_t)select->schema->columns[anchor->column_index].type) &&
              write_u32(file, &hash, snapshot->count) &&
              write_bytes(file, &hash, table_name, sizeof(table_name)) &&
              write_bytes(file, &hash, index_name, sizeof(index_name)) &&
              write_bytes(file, &hash, column_name, sizeof(column_name));

    for (uint32_t i = 0; ok && i < snapshot->count; i++) {
        if (snapshot->column_type == COL_TYPE_INT) {
            ok = write_u32(file, &hash, snapshot->entries[i].int_key);
        } else {
            ok = write_bytes(file,
                             &hash,
                             snapshot->entries[i].text_key,
                             sizeof(snapshot->entries[i].text_key));
        }
        ok = ok && write_u32(file, &hash, snapshot->entries[i].primary_key);
    }
    ok = ok && write_u64(file, NULL, hash);
    if (ok) ok = sync_file(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool load_snapshot(Table* table,
                          const AnchoredSelect* select,
                          AnchorSnapshot* snapshot,
                          uint64_t expected_epoch) {
    char filename[600];
    if (!range_filename(select->index, filename, sizeof(filename))) return false;
    FILE* file = fopen(filename, "rb");
    if (file == NULL) return false;

    const TinyDBGenericPredicate* anchor =
        &select->predicates[select->anchor_predicate_index];
    uint64_t hash = ANCHOR_FNV_OFFSET;
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

    uint64_t max_entries =
        (uint64_t)table->pager->num_pages * (uint64_t)LEAF_NODE_MAX_CELLS;
    if (!ok || magic != ANCHOR_INDEX_MAGIC ||
        version != ANCHOR_INDEX_VERSION || epoch != expected_epoch ||
        root_page_num != select->schema->root_page_num ||
        column_index != anchor->column_index ||
        column_type != (uint32_t)select->schema->columns[anchor->column_index].type ||
        (uint64_t)count > max_entries ||
        !ci_equal(table_name, select->schema->name) ||
        !ci_equal(index_name, select->index->name) ||
        !ci_equal(column_name, select->schema->columns[anchor->column_index].name) ||
        !reserve_entries(snapshot, count)) {
        fclose(file);
        snapshot->count = 0;
        return false;
    }

    snapshot->column_type = (ColumnType)column_type;
    snapshot->count = 0;
    for (uint32_t i = 0; ok && i < count; i++) {
        AnchorIndexEntry* entry = &snapshot->entries[i];
        memset(entry, 0, sizeof(*entry));
        if (snapshot->column_type == COL_TYPE_INT) {
            ok = read_u32(file, &hash, &entry->int_key);
        } else {
            ok = read_bytes(file,
                            &hash,
                            entry->text_key,
                            sizeof(entry->text_key));
            entry->text_key[sizeof(entry->text_key) - 1] = '\0';
        }
        ok = ok && read_u32(file, &hash, &entry->primary_key);
        if (ok) snapshot->count++;
    }

    uint64_t stored_hash = 0;
    ok = ok && read_u64(file, NULL, &stored_hash) && stored_hash == hash;
    if (ok) ok = fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!ok) snapshot->count = 0;
    return ok;
}

static bool ensure_snapshot(Table* table,
                            const AnchoredSelect* select,
                            AnchorSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    const TinyDBGenericPredicate* anchor =
        &select->predicates[select->anchor_predicate_index];
    snapshot->column_type = select->schema->columns[anchor->column_index].type;

    uint64_t epoch = 0;
    if (!tinydb_generic_index_epoch_current(table, &epoch)) return false;
    if (!table->in_transaction && load_snapshot(table, select, snapshot, epoch)) {
        return true;
    }

    AnchorBuildContext context;
    memset(&context, 0, sizeof(context));
    context.select = select;
    context.snapshot = snapshot;
    (void)tinydb_record_scan(table, select->schema, build_entry, &context);
    if (context.failed) return false;

    if (snapshot->count > 1) {
        qsort(snapshot->entries,
              snapshot->count,
              sizeof(AnchorIndexEntry),
              snapshot->column_type == COL_TYPE_INT
                  ? compare_int_entries
                  : compare_text_entries);
    }
    if (!table->in_transaction) (void)write_snapshot(select, snapshot, epoch);
    return true;
}

static int compare_entry_value(const AnchorSnapshot* snapshot,
                               const AnchorIndexEntry* entry,
                               const TinyDBValue* value) {
    if (snapshot->column_type == COL_TYPE_INT) {
        if (entry->int_key < value->int_value) return -1;
        if (entry->int_key > value->int_value) return 1;
        return 0;
    }
    int compared = strcmp(entry->text_key, value->text);
    if (compared < 0) return -1;
    if (compared > 0) return 1;
    return 0;
}

static size_t lower_bound(const AnchorSnapshot* snapshot,
                          const TinyDBValue* value) {
    size_t min = 0;
    size_t max = snapshot->count;
    while (min < max) {
        size_t mid = min + (max - min) / 2u;
        if (compare_entry_value(snapshot, &snapshot->entries[mid], value) < 0) {
            min = mid + 1u;
        } else {
            max = mid;
        }
    }
    return min;
}

static size_t upper_bound(const AnchorSnapshot* snapshot,
                          const TinyDBValue* value) {
    size_t min = 0;
    size_t max = snapshot->count;
    while (min < max) {
        size_t mid = min + (max - min) / 2u;
        if (compare_entry_value(snapshot, &snapshot->entries[mid], value) <= 0) {
            min = mid + 1u;
        } else {
            max = mid;
        }
    }
    return min;
}

static void anchor_bounds(const AnchorSnapshot* snapshot,
                          const TinyDBGenericPredicate* predicate,
                          size_t* start,
                          size_t* end) {
    *start = 0;
    *end = snapshot->count;
    switch (predicate->op) {
        case TINYDB_GENERIC_COMPARE_EQ:
            *start = lower_bound(snapshot, &predicate->value);
            *end = upper_bound(snapshot, &predicate->value);
            break;
        case TINYDB_GENERIC_COMPARE_GT:
            *start = upper_bound(snapshot, &predicate->value);
            break;
        case TINYDB_GENERIC_COMPARE_GTE:
            *start = lower_bound(snapshot, &predicate->value);
            break;
        case TINYDB_GENERIC_COMPARE_LT:
            *end = lower_bound(snapshot, &predicate->value);
            break;
        case TINYDB_GENERIC_COMPARE_LTE:
            *end = upper_bound(snapshot, &predicate->value);
            break;
    }
}

static bool decode_values(const TableSchema* schema,
                          const TinyDBRecord* record,
                          TinyDBValue* values) {
    uint32_t value_count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_record_decode(schema,
                                record,
                                values,
                                MAX_COLUMNS_PER_TABLE,
                                &value_count,
                                message,
                                sizeof(message)) &&
           value_count == schema->num_columns;
}

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static TinyDBGenericSqlStatus execute_anchored_select(
    Table* table,
    const AnchoredSelect* select,
    TinyDBGenericSqlResult* result) {
    AnchorSnapshot snapshot;
    if (!ensure_snapshot(table, select, &snapshot)) {
        free(snapshot.entries);
        return execute_error(result,
                             "unable to load or rebuild compound index anchor");
    }

    const TinyDBGenericPredicate* anchor =
        &select->predicates[select->anchor_predicate_index];
    size_t start = 0;
    size_t end = 0;
    anchor_bounds(&snapshot, anchor, &start, &end);

    uint32_t matched = 0;
    uint32_t emitted = 0;
    for (size_t i = start; i < end; i++) {
        TinyDBRecord record;
        if (!tinydb_record_find(table,
                                select->schema,
                                snapshot.entries[i].primary_key,
                                &record)) {
            continue;
        }
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        if (!decode_values(select->schema, &record, values)) {
            free(snapshot.entries);
            return execute_error(result,
                                 "unable to decode compound index candidate");
        }
        if (!predicates_match(select, values)) continue;

        matched++;
        if (select->projection_kind == ANCHOR_PROJECTION_COUNT) continue;
        if (matched <= select->offset) continue;
        if (select->has_limit && emitted >= select->limit) break;

        if (select->projection_kind == ANCHOR_PROJECTION_COLUMN) {
            print_value(&values[select->projection_column_index]);
        } else {
            tinydb_record_print(select->schema, &record);
        }
        emitted++;
    }

    if (select->projection_kind == ANCHOR_PROJECTION_COUNT) {
        uint32_t count = matched;
        if (select->offset > 0) count = 0;
        if (select->has_limit && select->limit == 0) count = 0;
        printf("%u\n", count);
    }

    free(snapshot.entries);
    return success(result);
}

static bool append_text(char* output,
                        size_t output_size,
                        size_t* length,
                        const char* text) {
    while (*text != '\0') {
        if (*length + 1u >= output_size) return false;
        output[(*length)++] = *text++;
    }
    output[*length] = '\0';
    return true;
}

static bool append_predicate(char* output,
                             size_t output_size,
                             size_t* length,
                             const TableSchema* schema,
                             const TinyDBGenericPredicate* predicate) {
    if (!append_text(output,
                     output_size,
                     length,
                     schema->columns[predicate->column_index].name) ||
        !append_text(output, output_size, length, " ") ||
        !append_text(output,
                     output_size,
                     length,
                     tinydb_generic_compare_op_text(predicate->op)) ||
        !append_text(output, output_size, length, " ")) {
        return false;
    }

    if (predicate->value.type == COL_TYPE_INT) {
        char number[16];
        snprintf(number, sizeof(number), "%u", predicate->value.int_value);
        return append_text(output, output_size, length, number);
    }
    return append_text(output, output_size, length, "'") &&
           append_text(output, output_size, length, predicate->value.text) &&
           append_text(output, output_size, length, "'");
}

static bool build_filter_expression(const AnchoredSelect* select,
                                    char* output,
                                    size_t output_size) {
    size_t length = 0;
    output[0] = '\0';
    for (uint32_t i = 0; i < select->predicate_count; i++) {
        if (i > 0 && !append_text(output, output_size, &length, " AND ")) {
            return false;
        }
        if (!append_predicate(output,
                              output_size,
                              &length,
                              select->schema,
                              &select->predicates[i])) {
            return false;
        }
    }
    return true;
}

static void format_predicate_value(const TinyDBGenericPredicate* predicate,
                                   char* output,
                                   size_t output_size) {
    if (predicate->value.type == COL_TYPE_INT) {
        snprintf(output, output_size, "%u", predicate->value.int_value);
    } else {
        snprintf(output, output_size, "'%s'", predicate->value.text);
    }
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    AnchoredSelect select;
    if (parse_anchored_select(table, sql, &select)) {
        return execute_anchored_select(table, &select, output);
    }
    return tinydb_generic_sql_try_execute_range_index_base(table, sql, output);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (plan != NULL) memset(plan, 0, sizeof(*plan));
    if (table == NULL || sql == NULL || plan == NULL) return output->status;

    AnchoredSelect select;
    if (!parse_anchored_select(table, sql, &select)) {
        return tinydb_generic_sql_build_select_plan_range_index_base(
            table, sql, plan, output);
    }

    const TinyDBGenericPredicate* anchor =
        &select.predicates[select.anchor_predicate_index];
    plan->applicable = true;
    plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RESIDUAL;
    plan->root_page_num = select.schema->root_page_num;
    plan->has_filter = true;
    snprintf(plan->table_name, sizeof(plan->table_name), "%s", select.schema->name);
    snprintf(plan->index_name, sizeof(plan->index_name), "%s", select.index->name);
    snprintf(plan->filter_column,
             sizeof(plan->filter_column),
             "%s",
             select.schema->columns[anchor->column_index].name);
    snprintf(plan->filter_operator,
             sizeof(plan->filter_operator),
             "%s",
             tinydb_generic_compare_op_text(anchor->op));
    format_predicate_value(anchor, plan->filter_value, sizeof(plan->filter_value));

    if (select.projection_kind == ANCHOR_PROJECTION_STAR) {
        snprintf(plan->projection, sizeof(plan->projection), "*");
    } else if (select.projection_kind == ANCHOR_PROJECTION_COUNT) {
        snprintf(plan->projection, sizeof(plan->projection), "COUNT(*)");
    } else {
        snprintf(plan->projection,
                 sizeof(plan->projection),
                 "%s",
                 select.schema->columns[select.projection_column_index].name);
    }
    if (!build_filter_expression(&select,
                                 plan->filter_expression,
                                 sizeof(plan->filter_expression))) {
        return execute_error(output, "compound predicate plan text exceeds capacity");
    }

    output->status = TINYDB_GENERIC_SQL_SUCCESS;
    output->statement_type = STATEMENT_SELECT;
    output->statement_type_valid = true;
    output->execute_result = EXECUTE_SUCCESS;
    return output->status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable ||
        plan->kind != TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RESIDUAL) {
        tinydb_generic_sql_print_plan_range_index_base(plan);
        return;
    }

    printf("PLAN: GENERIC SECONDARY INDEX + RESIDUAL FILTER\n");
    printf("  TABLE: %s (root page %u)\n",
           plan->table_name,
           plan->root_page_num);
    printf("  INDEX: %s\n", plan->index_name);
    printf("  PROJECTION: %s\n", plan->projection);
    printf("  ANCHOR: %s %s %s\n",
           plan->filter_column,
           plan->filter_operator,
           plan->filter_value);
    printf("  FILTER: %s\n", plan->filter_expression);
}
