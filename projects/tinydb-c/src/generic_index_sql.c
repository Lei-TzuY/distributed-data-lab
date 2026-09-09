#include "generic_sql.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#define GENERIC_INDEX_CACHE_SLOTS (MAX_TABLES * MAX_INDEXES)

typedef struct {
    const char* current;
} IndexParser;

typedef struct {
    Table* table;
    char index_name[MAX_NAME_SIZE];
    char database_filename[512];
    uint32_t root_page_num;
    GenericIndexEntry* entries;
    size_t count;
    size_t capacity;
    bool dirty;
    bool used;
} GenericIndexCache;

typedef struct {
    const TableSchema* schema;
    uint32_t column_index;
    GenericIndexCache* cache;
    bool failed;
} GenericIndexBuildContext;

typedef struct {
    TableSchema* schema;
    GenericSecondaryIndex* index;
    uint32_t filter_column_index;
    TinyDBValue filter_value;
    bool count_only;
    bool project_column;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} IndexedSelect;

static GenericIndexCache generic_index_caches[GENERIC_INDEX_CACHE_SLOTS];

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_range_select(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_range(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_range(const TinyDBGenericSelectPlan* plan);

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

static void skip_spaces(IndexParser* parser) {
    while (isspace((unsigned char)*parser->current)) parser->current++;
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool consume_word(IndexParser* parser, const char* word) {
    IndexParser backup = *parser;
    const char* expected = word;
    skip_spaces(parser);
    while (*expected != '\0' &&
           ci_char(*parser->current) == ci_char(*expected)) {
        parser->current++;
        expected++;
    }
    if (*expected != '\0' || is_identifier_char(*parser->current)) {
        *parser = backup;
        return false;
    }
    return true;
}

static bool consume_char(IndexParser* parser, char expected) {
    skip_spaces(parser);
    if (*parser->current != expected) return false;
    parser->current++;
    return true;
}

static bool consume_end(IndexParser* parser) {
    skip_spaces(parser);
    if (*parser->current == ';') parser->current++;
    skip_spaces(parser);
    return *parser->current == '\0';
}

static bool parse_identifier(IndexParser* parser,
                             char* output,
                             size_t output_size) {
    skip_spaces(parser);
    if (!isalpha((unsigned char)*parser->current) && *parser->current != '_') {
        return false;
    }
    const char* start = parser->current;
    while (is_identifier_char(*parser->current)) parser->current++;
    size_t length = (size_t)(parser->current - start);
    if (length == 0 || length >= output_size) return false;
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool parse_uint32(IndexParser* parser, uint32_t* value) {
    skip_spaces(parser);
    if (!isdigit((unsigned char)*parser->current)) return false;
    char* end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(parser->current, &end, 10);
    if (errno == ERANGE || parsed > UINT32_MAX || end == parser->current) {
        return false;
    }
    parser->current = end;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_string(IndexParser* parser,
                         char* output,
                         size_t output_size) {
    skip_spaces(parser);
    if (*parser->current != '\'') return false;
    parser->current++;
    size_t length = 0;

    while (*parser->current != '\0') {
        char value = *parser->current++;
        if (value == '\'') {
            if (*parser->current == '\'') {
                parser->current++;
                value = '\'';
            } else {
                if (length >= output_size) return false;
                output[length] = '\0';
                return true;
            }
        }
        if (length + 1 >= output_size) return false;
        output[length++] = value;
    }
    return false;
}

static TableSchema* find_schema(Table* table, const char* name) {
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static int find_column_index(const TableSchema* schema, const char* name) {
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (ci_equal(schema->columns[i].name, name)) return (int)i;
    }
    return -1;
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

static GenericSecondaryIndex* find_single_column_index(Table* table,
                                                        const char* table_name,
                                                        const char* column_name) {
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && index->num_columns == 1 &&
            ci_equal(index->table_name, table_name) &&
            ci_equal(index->column_name, column_name)) {
            return index;
        }
    }
    return NULL;
}

static bool parse_value_for_column(IndexParser* parser,
                                   const TableColumn* column,
                                   TinyDBValue* value) {
    memset(value, 0, sizeof(*value));
    value->type = column->type;
    if (column->type == COL_TYPE_INT) {
        return parse_uint32(parser, &value->int_value);
    }
    return parse_string(parser, value->text, sizeof(value->text));
}

static bool parse_projection(IndexParser* parser,
                             const TableSchema* schema,
                             IndexedSelect* select) {
    if (consume_char(parser, '*')) {
        select->count_only = false;
        select->project_column = false;
        select->projection_column_index = 0;
        return true;
    }

    IndexParser backup = *parser;
    if (consume_word(parser, "count") && consume_char(parser, '(') &&
        consume_char(parser, '*') && consume_char(parser, ')')) {
        select->count_only = true;
        select->project_column = false;
        select->projection_column_index = 0;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = find_column_index(schema, column);
    if (column_index < 0) return false;
    select->count_only = false;
    select->project_column = true;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool parse_indexed_select(Table* table,
                                 const char* sql,
                                 IndexedSelect* select) {
    memset(select, 0, sizeof(*select));
    IndexParser parser;
    parser.current = sql;
    if (!consume_word(&parser, "select")) return false;

    IndexParser routing = parser;
    if (consume_char(&routing, '*')) {
        /* positioned after projection */
    } else {
        IndexParser backup = routing;
        if (!(consume_word(&routing, "count") && consume_char(&routing, '(') &&
              consume_char(&routing, '*') && consume_char(&routing, ')'))) {
            routing = backup;
            char ignored[MAX_NAME_SIZE];
            if (!parse_identifier(&routing, ignored, sizeof(ignored))) return false;
        }
    }

    char table_name[MAX_NAME_SIZE];
    if (!consume_word(&routing, "from") ||
        !parse_identifier(&routing, table_name, sizeof(table_name))) {
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
    if (!parse_projection(&parser, schema, select)) return false;
    if (!consume_word(&parser, "from") ||
        !parse_identifier(&parser, table_name, sizeof(table_name)) ||
        !ci_equal(table_name, schema->name) ||
        !consume_word(&parser, "where")) {
        return false;
    }

    char filter_column[MAX_NAME_SIZE];
    if (!parse_identifier(&parser, filter_column, sizeof(filter_column))) return false;
    int filter_index = find_column_index(schema, filter_column);
    if (filter_index <= 0) return false;

    skip_spaces(&parser);
    if (*parser.current != '=') return false;
    parser.current++;
    if (*parser.current == '=') return false;

    select->filter_column_index = (uint32_t)filter_index;
    if (!parse_value_for_column(&parser,
                                &schema->columns[select->filter_column_index],
                                &select->filter_value)) {
        return false;
    }

    select->index = find_single_column_index(table,
                                              schema->name,
                                              schema->columns[select->filter_column_index].name);
    if (select->index == NULL) return false;

    if (consume_word(&parser, "limit")) {
        if (!parse_uint32(&parser, &select->limit)) return false;
        select->has_limit = true;
        if (consume_word(&parser, "offset")) {
            if (!parse_uint32(&parser, &select->offset)) return false;
        }
    } else if (consume_word(&parser, "offset")) {
        if (!parse_uint32(&parser, &select->offset)) return false;
    }

    return consume_end(&parser);
}

static int compare_index_entries(const void* left, const void* right) {
    const GenericIndexEntry* a = (const GenericIndexEntry*)left;
    const GenericIndexEntry* b = (const GenericIndexEntry*)right;
    int compared = strcmp(a->key_val, b->key_val);
    if (compared != 0) return compared;
    if (a->primary_key < b->primary_key) return -1;
    if (a->primary_key > b->primary_key) return 1;
    return 0;
}

static void format_index_key(const TinyDBValue* value,
                             char* output,
                             size_t output_size) {
    if (value->type == COL_TYPE_INT) {
        snprintf(output, output_size, "%u", value->int_value);
    } else {
        snprintf(output, output_size, "%s", value->text);
    }
}

static GenericIndexCache* find_cache(Table* table,
                                     const TableSchema* schema,
                                     const GenericSecondaryIndex* index) {
    GenericIndexCache* free_slot = NULL;
    for (size_t i = 0; i < GENERIC_INDEX_CACHE_SLOTS; i++) {
        GenericIndexCache* cache = &generic_index_caches[i];
        if (!cache->used) {
            if (free_slot == NULL) free_slot = cache;
            continue;
        }
        if (cache->table == table &&
            ci_equal(cache->index_name, index->name)) {
            const char* filename = table->pager->filename;
            if (cache->root_page_num != schema->root_page_num ||
                strcmp(cache->database_filename, filename) != 0) {
                cache->count = 0;
                cache->dirty = true;
                cache->root_page_num = schema->root_page_num;
                snprintf(cache->database_filename,
                         sizeof(cache->database_filename),
                         "%s",
                         filename);
            }
            return cache;
        }
    }

    if (free_slot == NULL) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->table = table;
    free_slot->dirty = true;
    free_slot->root_page_num = schema->root_page_num;
    snprintf(free_slot->index_name,
             sizeof(free_slot->index_name),
             "%s",
             index->name);
    snprintf(free_slot->database_filename,
             sizeof(free_slot->database_filename),
             "%s",
             table->pager->filename);
    return free_slot;
}

static bool reserve_cache_entry(GenericIndexCache* cache) {
    if (cache->count < cache->capacity) return true;
    size_t new_capacity = cache->capacity == 0 ? 32u : cache->capacity * 2u;
    if (new_capacity < cache->capacity ||
        new_capacity > SIZE_MAX / sizeof(GenericIndexEntry)) {
        return false;
    }
    GenericIndexEntry* grown = (GenericIndexEntry*)realloc(
        cache->entries,
        new_capacity * sizeof(GenericIndexEntry));
    if (grown == NULL) return false;
    cache->entries = grown;
    cache->capacity = new_capacity;
    return true;
}

static bool build_index_record(const TableSchema* schema,
                               const TinyDBRecord* record,
                               void* raw_context) {
    GenericIndexBuildContext* context = (GenericIndexBuildContext*)raw_context;
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
        !reserve_cache_entry(context->cache)) {
        context->failed = true;
        return false;
    }

    GenericIndexEntry* entry = &context->cache->entries[context->cache->count++];
    memset(entry, 0, sizeof(*entry));
    format_index_key(&values[context->column_index],
                     entry->key_val,
                     sizeof(entry->key_val));
    entry->primary_key = values[0].int_value;
    return true;
}

static bool ensure_cache(Table* table,
                         const IndexedSelect* select,
                         GenericIndexCache** cache_out) {
    GenericIndexCache* cache = find_cache(table, select->schema, select->index);
    if (cache == NULL) return false;

    if (cache->dirty) {
        cache->count = 0;
        GenericIndexBuildContext context;
        memset(&context, 0, sizeof(context));
        context.schema = select->schema;
        context.column_index = select->filter_column_index;
        context.cache = cache;
        (void)tinydb_record_scan(table,
                                 select->schema,
                                 build_index_record,
                                 &context);
        if (context.failed) {
            cache->count = 0;
            return false;
        }
        if (cache->count > 1) {
            qsort(cache->entries,
                  cache->count,
                  sizeof(GenericIndexEntry),
                  compare_index_entries);
        }
        cache->dirty = table->in_transaction;
    }

    *cache_out = cache;
    return true;
}

static size_t lower_bound(const GenericIndexCache* cache, const char* key) {
    size_t min = 0;
    size_t max = cache->count;
    while (min < max) {
        size_t mid = min + (max - min) / 2u;
        if (strcmp(cache->entries[mid].key_val, key) < 0) {
            min = mid + 1u;
        } else {
            max = mid;
        }
    }
    return min;
}

static size_t upper_bound(const GenericIndexCache* cache, const char* key) {
    size_t min = 0;
    size_t max = cache->count;
    while (min < max) {
        size_t mid = min + (max - min) / 2u;
        if (strcmp(cache->entries[mid].key_val, key) <= 0) {
            min = mid + 1u;
        } else {
            max = mid;
        }
    }
    return min;
}

static bool values_equal(const TinyDBValue* left, const TinyDBValue* right) {
    if (left->type != right->type) return false;
    if (left->type == COL_TYPE_INT) return left->int_value == right->int_value;
    return strcmp(left->text, right->text) == 0;
}

static void print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static TinyDBGenericSqlStatus execute_indexed_select(Table* table,
                                                      const IndexedSelect* select,
                                                      TinyDBGenericSqlResult* result) {
    GenericIndexCache* cache = NULL;
    if (!ensure_cache(table, select, &cache)) {
        return execute_error(result, "unable to build schema-aware secondary index cache");
    }

    char key[GENERIC_INDEX_KEY_SIZE];
    format_index_key(&select->filter_value, key, sizeof(key));
    size_t start = lower_bound(cache, key);
    size_t end = upper_bound(cache, key);
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
            return execute_error(result, "unable to decode indexed generic record");
        }
        if (!values_equal(&values[select->filter_column_index],
                          &select->filter_value)) {
            continue;
        }

        matched++;
        if (select->count_only) continue;
        if (matched <= select->offset) continue;
        if (select->has_limit && emitted >= select->limit) break;

        if (select->project_column) {
            print_value(&values[select->projection_column_index]);
        } else {
            tinydb_record_print(select->schema, &record);
        }
        emitted++;
    }

    if (select->count_only) {
        uint32_t count = matched;
        if (select->offset > 0) count = 0;
        if (select->has_limit && select->limit == 0) count = 0;
        printf("%u\n", count);
    }

    return success(result);
}

static void invalidate_table_caches(Table* table) {
    for (size_t i = 0; i < GENERIC_INDEX_CACHE_SLOTS; i++) {
        if (generic_index_caches[i].used &&
            generic_index_caches[i].table == table) {
            generic_index_caches[i].dirty = true;
        }
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

    IndexedSelect select;
    if (parse_indexed_select(table, sql, &select)) {
        return execute_indexed_select(table, &select, output);
    }

    TinyDBGenericSqlStatus status = tinydb_generic_sql_try_execute_range_select(
        table, sql, output);
    if (status != TINYDB_GENERIC_SQL_NOT_APPLICABLE &&
        output->statement_type_valid &&
        (output->statement_type == STATEMENT_INSERT ||
         output->statement_type == STATEMENT_UPDATE ||
         output->statement_type == STATEMENT_DELETE)) {
        invalidate_table_caches(table);
    }
    return status;
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlStatus status = tinydb_generic_sql_build_select_plan_range(
        table, sql, plan, result);
    if (status != TINYDB_GENERIC_SQL_SUCCESS ||
        plan == NULL || !plan->applicable || !plan->has_filter ||
        plan->kind != TINYDB_GENERIC_PLAN_FULL_SCAN) {
        return status;
    }

    const char* op = plan->filter_operator[0] != '\0'
        ? plan->filter_operator
        : "=";
    if (strcmp(op, "=") != 0) return status;

    TableSchema* schema = find_schema(table, plan->table_name);
    if (schema == NULL || is_legacy_fixed_row_schema(schema)) return status;
    int filter_index = find_column_index(schema, plan->filter_column);
    if (filter_index <= 0) return status;

    GenericSecondaryIndex* index = find_single_column_index(
        table, schema->name, schema->columns[filter_index].name);
    if (index == NULL) return status;

    plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_LOOKUP;
    if (plan->filter_operator[0] == '\0') {
        snprintf(plan->filter_operator,
                 sizeof(plan->filter_operator),
                 "%s",
                 "=");
    }
    snprintf(plan->index_name, sizeof(plan->index_name), "%s", index->name);
    return status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable) return;
    if (plan->kind != TINYDB_GENERIC_PLAN_SECONDARY_INDEX_LOOKUP) {
        tinydb_generic_sql_print_plan_range(plan);
        return;
    }

    printf("PLAN: GENERIC SECONDARY INDEX LOOKUP\n");
    printf("  TABLE: %s (root page %u)\n",
           plan->table_name,
           plan->root_page_num);
    printf("  INDEX: %s\n", plan->index_name);
    printf("  PROJECTION: %s\n", plan->projection);
    printf("  FILTER: %s %s %s\n",
           plan->filter_column,
           plan->filter_operator[0] != '\0' ? plan->filter_operator : "=",
           plan->filter_value);
}
