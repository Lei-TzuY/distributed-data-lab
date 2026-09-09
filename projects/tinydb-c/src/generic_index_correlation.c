#include "generic_index_correlation.h"
#include "generic_index_epoch.h"
#include "record.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define GENERIC_CORR_MAGIC 0x47494331u /* GIC1 */
#define GENERIC_CORR_VERSION 1u
#define GENERIC_CORR_MCV_MAX 32u
#define GENERIC_CORR_PERSIST_THRESHOLD 64u
#define GENERIC_CORR_FNV_OFFSET 1469598103934665603ULL
#define GENERIC_CORR_FNV_PRIME 1099511628211ULL

typedef struct {
    TinyDBValue first;
    TinyDBValue second;
} GenericPairValue;

typedef struct {
    GenericPairValue pair;
    uint32_t frequency;
} GenericPairMcv;

typedef struct {
    uint64_t epoch;
    uint32_t total_count;
    uint32_t distinct_pair_count;
    uint32_t mcv_count;
    uint32_t first_column_index;
    uint32_t second_column_index;
    ColumnType first_type;
    ColumnType second_type;
    GenericPairMcv mcvs[GENERIC_CORR_MCV_MAX];
} GenericPairStats;

typedef struct {
    const TableSchema* schema;
    uint32_t first_column_index;
    uint32_t second_column_index;
    GenericPairValue* pairs;
    uint32_t count;
    uint32_t capacity;
    bool failed;
} GenericPairBuildContext;

static void set_message(char* message, size_t message_size, const char* text) {
    if (message == NULL || message_size == 0) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static int ci_compare(const char* left, const char* right) {
    if (left == NULL) return right == NULL ? 0 : -1;
    if (right == NULL) return 1;
    while (*left != '\0' && *right != '\0') {
        int a = ci_char(*left);
        int b = ci_char(*right);
        if (a < b) return -1;
        if (a > b) return 1;
        left++;
        right++;
    }
    if (*left == '\0' && *right == '\0') return 0;
    return *left == '\0' ? -1 : 1;
}

static bool ci_equal(const char* left, const char* right) {
    return ci_compare(left, right) == 0;
}

static uint64_t fnv_update(uint64_t hash, const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= GENERIC_CORR_FNV_PRIME;
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
    for (uint32_t i = 0; i < 8; i++) {
        value |= ((uint64_t)in[i]) << (i * 8u);
    }
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

static int compare_value(const TinyDBValue* left, const TinyDBValue* right) {
    if (left->type != right->type) {
        return left->type < right->type ? -1 : 1;
    }
    if (left->type == COL_TYPE_INT) {
        if (left->int_value < right->int_value) return -1;
        if (left->int_value > right->int_value) return 1;
        return 0;
    }
    return strcmp(left->text, right->text);
}

static int compare_pair(const GenericPairValue* left,
                        const GenericPairValue* right) {
    int first = compare_value(&left->first, &right->first);
    if (first != 0) return first;
    return compare_value(&left->second, &right->second);
}

static int compare_pair_qsort(const void* left, const void* right) {
    return compare_pair((const GenericPairValue*)left,
                        (const GenericPairValue*)right);
}

static bool write_typed_value(FILE* file,
                              uint64_t* hash,
                              ColumnType type,
                              const TinyDBValue* value) {
    if (type == COL_TYPE_INT) {
        return write_u32(file, hash, value->int_value);
    }
    return write_bytes(file, hash, value->text, sizeof(value->text));
}

static bool read_typed_value(FILE* file,
                             uint64_t* hash,
                             ColumnType type,
                             TinyDBValue* value) {
    memset(value, 0, sizeof(*value));
    value->type = type;
    if (type == COL_TYPE_INT) {
        return read_u32(file, hash, &value->int_value);
    }
    if (!read_bytes(file, hash, value->text, sizeof(value->text))) return false;
    if (memchr(value->text, '\0', sizeof(value->text)) == NULL) return false;
    value->text[sizeof(value->text) - 1u] = '\0';
    return true;
}

static bool canonical_indexes(const GenericSecondaryIndex* first,
                              const GenericSecondaryIndex* second,
                              const GenericSecondaryIndex** canonical_first,
                              const GenericSecondaryIndex** canonical_second) {
    if (first == NULL || second == NULL || first == second ||
        ci_equal(first->name, second->name)) {
        return false;
    }
    if (ci_compare(first->name, second->name) < 0) {
        *canonical_first = first;
        *canonical_second = second;
    } else {
        *canonical_first = second;
        *canonical_second = first;
    }
    return true;
}

bool tinydb_generic_index_pair_stats_filename(
    Table* table,
    const GenericSecondaryIndex* first,
    const GenericSecondaryIndex* second,
    char* output,
    size_t output_size) {
    const GenericSecondaryIndex* canonical_first = NULL;
    const GenericSecondaryIndex* canonical_second = NULL;
    if (table == NULL || table->pager == NULL || output == NULL || output_size == 0 ||
        !canonical_indexes(first,
                           second,
                           &canonical_first,
                           &canonical_second)) {
        return false;
    }
    int written = snprintf(output,
                           output_size,
                           "%s.%s.%s.corr.stats",
                           table->pager->filename,
                           canonical_first->name,
                           canonical_second->name);
    return written >= 0 && (size_t)written < output_size;
}

static int find_column_index(const TableSchema* schema, const char* name) {
    if (schema == NULL || name == NULL) return -1;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (ci_equal(schema->columns[i].name, name)) return (int)i;
    }
    return -1;
}

static bool validate_pair(Table* table,
                          const TableSchema* schema,
                          GenericSecondaryIndex* first,
                          GenericSecondaryIndex* second,
                          uint32_t* first_column_index,
                          uint32_t* second_column_index,
                          GenericSecondaryIndex** canonical_first,
                          GenericSecondaryIndex** canonical_second) {
    const GenericSecondaryIndex* ordered_first = NULL;
    const GenericSecondaryIndex* ordered_second = NULL;
    if (table == NULL || schema == NULL || first == NULL || second == NULL ||
        !first->enabled || !second->enabled ||
        first->num_columns != 1 || second->num_columns != 1 ||
        !ci_equal(first->table_name, schema->name) ||
        !ci_equal(second->table_name, schema->name) ||
        !canonical_indexes(first, second, &ordered_first, &ordered_second)) {
        return false;
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        return false;
    }

    int first_index = find_column_index(schema, ordered_first->column_name);
    int second_index = find_column_index(schema, ordered_second->column_name);
    if (first_index <= 0 || second_index <= 0 || first_index == second_index) {
        return false;
    }

    *first_column_index = (uint32_t)first_index;
    *second_column_index = (uint32_t)second_index;
    *canonical_first = (GenericSecondaryIndex*)ordered_first;
    *canonical_second = (GenericSecondaryIndex*)ordered_second;
    return true;
}

static bool reserve_pairs(GenericPairBuildContext* context, uint32_t needed) {
    if (needed <= context->capacity) return true;
    uint32_t capacity = context->capacity == 0 ? 64u : context->capacity;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    GenericPairValue* grown = (GenericPairValue*)realloc(
        context->pairs, (size_t)capacity * sizeof(GenericPairValue));
    if (grown == NULL) return false;
    context->pairs = grown;
    context->capacity = capacity;
    return true;
}

static bool collect_pair(const TableSchema* schema,
                         const TinyDBRecord* record,
                         void* raw_context) {
    GenericPairBuildContext* context = (GenericPairBuildContext*)raw_context;
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
        !reserve_pairs(context, context->count + 1u)) {
        context->failed = true;
        return false;
    }

    GenericPairValue* pair = &context->pairs[context->count++];
    pair->first = values[context->first_column_index];
    pair->second = values[context->second_column_index];
    return true;
}

static bool mcv_better(const GenericPairValue* pair,
                       uint32_t frequency,
                       const GenericPairMcv* current) {
    if (frequency != current->frequency) return frequency > current->frequency;
    return compare_pair(pair, &current->pair) < 0;
}

static void consider_mcv(GenericPairStats* stats,
                         const GenericPairValue* pair,
                         uint32_t frequency) {
    uint32_t position = stats->mcv_count;
    if (stats->mcv_count < GENERIC_CORR_MCV_MAX) {
        stats->mcv_count++;
    } else {
        position = GENERIC_CORR_MCV_MAX - 1u;
        if (!mcv_better(pair, frequency, &stats->mcvs[position])) return;
    }

    stats->mcvs[position].pair = *pair;
    stats->mcvs[position].frequency = frequency;
    while (position > 0 &&
           mcv_better(&stats->mcvs[position].pair,
                      stats->mcvs[position].frequency,
                      &stats->mcvs[position - 1u])) {
        GenericPairMcv swap = stats->mcvs[position - 1u];
        stats->mcvs[position - 1u] = stats->mcvs[position];
        stats->mcvs[position] = swap;
        position--;
    }
}

static bool build_stats(Table* table,
                        const TableSchema* schema,
                        uint32_t first_column_index,
                        uint32_t second_column_index,
                        uint64_t epoch,
                        GenericPairStats* stats,
                        GenericPairValue** sorted_pairs_out) {
    GenericPairBuildContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    context.first_column_index = first_column_index;
    context.second_column_index = second_column_index;
    (void)tinydb_record_scan(table, schema, collect_pair, &context);
    if (context.failed) {
        free(context.pairs);
        return false;
    }

    if (context.count > 1) {
        qsort(context.pairs,
              context.count,
              sizeof(GenericPairValue),
              compare_pair_qsort);
    }

    memset(stats, 0, sizeof(*stats));
    stats->epoch = epoch;
    stats->first_column_index = first_column_index;
    stats->second_column_index = second_column_index;
    stats->first_type = schema->columns[first_column_index].type;
    stats->second_type = schema->columns[second_column_index].type;
    stats->total_count = context.count;

    uint32_t run_start = 0;
    while (run_start < context.count) {
        uint32_t run_end = run_start + 1u;
        while (run_end < context.count &&
               compare_pair(&context.pairs[run_start],
                            &context.pairs[run_end]) == 0) {
            run_end++;
        }
        stats->distinct_pair_count++;
        consider_mcv(stats,
                     &context.pairs[run_start],
                     run_end - run_start);
        run_start = run_end;
    }

    if (sorted_pairs_out != NULL) {
        *sorted_pairs_out = context.pairs;
    } else {
        free(context.pairs);
    }
    return true;
}

static bool write_stats(Table* table,
                        const TableSchema* schema,
                        const GenericSecondaryIndex* first,
                        const GenericSecondaryIndex* second,
                        const GenericPairStats* stats) {
    char filename[768];
    if (!tinydb_generic_index_pair_stats_filename(table,
                                                  first,
                                                  second,
                                                  filename,
                                                  sizeof(filename))) {
        return false;
    }

    FILE* file = fopen(filename, "wb");
    if (file == NULL) return false;

    char table_name[MAX_NAME_SIZE] = {0};
    char first_index_name[MAX_NAME_SIZE] = {0};
    char second_index_name[MAX_NAME_SIZE] = {0};
    char first_column_name[MAX_NAME_SIZE] = {0};
    char second_column_name[MAX_NAME_SIZE] = {0};
    snprintf(table_name, sizeof(table_name), "%s", schema->name);
    snprintf(first_index_name, sizeof(first_index_name), "%s", first->name);
    snprintf(second_index_name, sizeof(second_index_name), "%s", second->name);
    snprintf(first_column_name,
             sizeof(first_column_name),
             "%s",
             schema->columns[stats->first_column_index].name);
    snprintf(second_column_name,
             sizeof(second_column_name),
             "%s",
             schema->columns[stats->second_column_index].name);

    uint64_t hash = GENERIC_CORR_FNV_OFFSET;
    bool ok = write_u32(file, &hash, GENERIC_CORR_MAGIC) &&
              write_u32(file, &hash, GENERIC_CORR_VERSION) &&
              write_u64(file, &hash, stats->epoch) &&
              write_u32(file, &hash, schema->root_page_num) &&
              write_u32(file, &hash, stats->first_column_index) &&
              write_u32(file, &hash, (uint32_t)stats->first_type) &&
              write_u32(file, &hash, stats->second_column_index) &&
              write_u32(file, &hash, (uint32_t)stats->second_type) &&
              write_u32(file, &hash, stats->total_count) &&
              write_u32(file, &hash, stats->distinct_pair_count) &&
              write_u32(file, &hash, stats->mcv_count) &&
              write_bytes(file, &hash, table_name, sizeof(table_name)) &&
              write_bytes(file, &hash, first_index_name, sizeof(first_index_name)) &&
              write_bytes(file, &hash, first_column_name, sizeof(first_column_name)) &&
              write_bytes(file, &hash, second_index_name, sizeof(second_index_name)) &&
              write_bytes(file, &hash, second_column_name, sizeof(second_column_name));

    for (uint32_t i = 0; ok && i < stats->mcv_count; i++) {
        ok = write_typed_value(file,
                               &hash,
                               stats->first_type,
                               &stats->mcvs[i].pair.first) &&
             write_typed_value(file,
                               &hash,
                               stats->second_type,
                               &stats->mcvs[i].pair.second) &&
             write_u32(file, &hash, stats->mcvs[i].frequency);
    }

    ok = ok && write_u64(file, NULL, hash);
    if (ok) ok = sync_file(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool load_stats(Table* table,
                       const TableSchema* schema,
                       const GenericSecondaryIndex* first,
                       const GenericSecondaryIndex* second,
                       uint32_t first_column_index,
                       uint32_t second_column_index,
                       uint64_t expected_epoch,
                       GenericPairStats* stats) {
    char filename[768];
    if (!tinydb_generic_index_pair_stats_filename(table,
                                                  first,
                                                  second,
                                                  filename,
                                                  sizeof(filename))) {
        return false;
    }

    FILE* file = fopen(filename, "rb");
    if (file == NULL) return false;

    memset(stats, 0, sizeof(*stats));
    uint64_t hash = GENERIC_CORR_FNV_OFFSET;
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t root_page_num = 0;
    uint32_t first_type = 0;
    uint32_t second_type = 0;
    char table_name[MAX_NAME_SIZE] = {0};
    char first_index_name[MAX_NAME_SIZE] = {0};
    char first_column_name[MAX_NAME_SIZE] = {0};
    char second_index_name[MAX_NAME_SIZE] = {0};
    char second_column_name[MAX_NAME_SIZE] = {0};

    bool ok = read_u32(file, &hash, &magic) &&
              read_u32(file, &hash, &version) &&
              read_u64(file, &hash, &stats->epoch) &&
              read_u32(file, &hash, &root_page_num) &&
              read_u32(file, &hash, &stats->first_column_index) &&
              read_u32(file, &hash, &first_type) &&
              read_u32(file, &hash, &stats->second_column_index) &&
              read_u32(file, &hash, &second_type) &&
              read_u32(file, &hash, &stats->total_count) &&
              read_u32(file, &hash, &stats->distinct_pair_count) &&
              read_u32(file, &hash, &stats->mcv_count) &&
              read_bytes(file, &hash, table_name, sizeof(table_name)) &&
              read_bytes(file, &hash, first_index_name, sizeof(first_index_name)) &&
              read_bytes(file, &hash, first_column_name, sizeof(first_column_name)) &&
              read_bytes(file, &hash, second_index_name, sizeof(second_index_name)) &&
              read_bytes(file, &hash, second_column_name, sizeof(second_column_name));

    table_name[MAX_NAME_SIZE - 1u] = '\0';
    first_index_name[MAX_NAME_SIZE - 1u] = '\0';
    first_column_name[MAX_NAME_SIZE - 1u] = '\0';
    second_index_name[MAX_NAME_SIZE - 1u] = '\0';
    second_column_name[MAX_NAME_SIZE - 1u] = '\0';

    uint64_t max_entries =
        (uint64_t)table->pager->num_pages * (uint64_t)LEAF_NODE_MAX_CELLS;
    if (!ok || magic != GENERIC_CORR_MAGIC || version != GENERIC_CORR_VERSION ||
        stats->epoch != expected_epoch || root_page_num != schema->root_page_num ||
        stats->first_column_index != first_column_index ||
        stats->second_column_index != second_column_index ||
        first_type != (uint32_t)schema->columns[first_column_index].type ||
        second_type != (uint32_t)schema->columns[second_column_index].type ||
        (uint64_t)stats->total_count > max_entries ||
        stats->distinct_pair_count > stats->total_count ||
        stats->mcv_count > GENERIC_CORR_MCV_MAX ||
        stats->mcv_count > stats->distinct_pair_count ||
        (stats->total_count == 0 && stats->mcv_count != 0) ||
        (stats->total_count > 0 && stats->mcv_count == 0) ||
        !ci_equal(table_name, schema->name) ||
        !ci_equal(first_index_name, first->name) ||
        !ci_equal(second_index_name, second->name) ||
        !ci_equal(first_column_name, schema->columns[first_column_index].name) ||
        !ci_equal(second_column_name, schema->columns[second_column_index].name)) {
        fclose(file);
        return false;
    }

    stats->first_type = (ColumnType)first_type;
    stats->second_type = (ColumnType)second_type;
    uint64_t mcv_rows = 0;
    for (uint32_t i = 0; ok && i < stats->mcv_count; i++) {
        GenericPairMcv* mcv = &stats->mcvs[i];
        ok = read_typed_value(file,
                              &hash,
                              stats->first_type,
                              &mcv->pair.first) &&
             read_typed_value(file,
                              &hash,
                              stats->second_type,
                              &mcv->pair.second) &&
             read_u32(file, &hash, &mcv->frequency);
        if (!ok || mcv->frequency == 0 || mcv->frequency > stats->total_count) {
            ok = false;
            break;
        }
        mcv_rows += mcv->frequency;
        if (mcv_rows > stats->total_count) {
            ok = false;
            break;
        }
        if (i > 0) {
            GenericPairMcv* previous = &stats->mcvs[i - 1u];
            if (mcv->frequency > previous->frequency ||
                (mcv->frequency == previous->frequency &&
                 compare_pair(&previous->pair, &mcv->pair) >= 0)) {
                ok = false;
                break;
            }
        }
        for (uint32_t j = 0; j < i; j++) {
            if (compare_pair(&stats->mcvs[j].pair, &mcv->pair) == 0) {
                ok = false;
                break;
            }
        }
    }

    uint64_t stored_hash = 0;
    ok = ok && read_u64(file, NULL, &stored_hash) && stored_hash == hash;
    if (ok) ok = fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return ok;
}

static const GenericPairMcv* find_mcv(const GenericPairStats* stats,
                                      const GenericPairValue* pair) {
    for (uint32_t i = 0; i < stats->mcv_count; i++) {
        if (compare_pair(&stats->mcvs[i].pair, pair) == 0) {
            return &stats->mcvs[i];
        }
    }
    return NULL;
}

static uint32_t estimate_pair(const GenericPairStats* stats,
                              const GenericPairValue* pair,
                              bool* exact_mcv) {
    *exact_mcv = false;
    if (stats->total_count == 0 || stats->distinct_pair_count == 0) return 0;

    const GenericPairMcv* mcv = find_mcv(stats, pair);
    if (mcv != NULL) {
        *exact_mcv = true;
        return mcv->frequency;
    }

    uint32_t mcv_rows = 0;
    for (uint32_t i = 0; i < stats->mcv_count; i++) {
        mcv_rows += stats->mcvs[i].frequency;
    }
    uint32_t remaining_distinct = stats->distinct_pair_count - stats->mcv_count;
    uint32_t remaining_rows = stats->total_count - mcv_rows;
    if (remaining_distinct == 0 || remaining_rows == 0) return 0;
    uint32_t average =
        (remaining_rows + remaining_distinct - 1u) / remaining_distinct;
    return average == 0 ? 1u : average;
}

static uint32_t exact_frequency(const GenericPairValue* pairs,
                                uint32_t count,
                                const GenericPairValue* target) {
    uint32_t matches = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (compare_pair(&pairs[i], target) == 0) matches++;
    }
    return matches;
}

bool tinydb_generic_index_refresh_pair_statistics(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* first,
    GenericSecondaryIndex* second,
    char* message,
    size_t message_size) {
    uint32_t first_column_index = 0;
    uint32_t second_column_index = 0;
    GenericSecondaryIndex* canonical_first = NULL;
    GenericSecondaryIndex* canonical_second = NULL;
    if (!validate_pair(table,
                       schema,
                       first,
                       second,
                       &first_column_index,
                       &second_column_index,
                       &canonical_first,
                       &canonical_second)) {
        set_message(message, message_size, "invalid generic pair statistics target");
        return false;
    }
    if (table->in_transaction) {
        set_message(message,
                    message_size,
                    "pairwise optimizer statistics cannot be refreshed inside a transaction");
        return false;
    }

    uint64_t epoch = 0;
    if (!tinydb_generic_index_epoch_current(table, &epoch)) {
        set_message(message, message_size, "unable to read generic index epoch");
        return false;
    }

    GenericPairStats stats;
    if (!build_stats(table,
                     schema,
                     first_column_index,
                     second_column_index,
                     epoch,
                     &stats,
                     NULL) ||
        !write_stats(table,
                     schema,
                     canonical_first,
                     canonical_second,
                     &stats)) {
        set_message(message,
                    message_size,
                    "unable to build or persist pairwise optimizer statistics");
        return false;
    }

    set_message(message, message_size, "ok");
    return true;
}

static bool predicate_matches_index(const TableSchema* schema,
                                    const GenericSecondaryIndex* index,
                                    const TinyDBGenericPredicate* predicate,
                                    uint32_t expected_column_index) {
    if (schema == NULL || index == NULL || predicate == NULL ||
        predicate->op != TINYDB_GENERIC_COMPARE_EQ ||
        predicate->column_index != expected_column_index ||
        predicate->value.type != schema->columns[expected_column_index].type) {
        return false;
    }
    return ci_equal(index->column_name,
                    schema->columns[expected_column_index].name);
}

bool tinydb_generic_index_estimate_pair_equality(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* first,
    const TinyDBGenericPredicate* first_predicate,
    GenericSecondaryIndex* second,
    const TinyDBGenericPredicate* second_predicate,
    TinyDBGenericPairEstimate* estimate,
    char* message,
    size_t message_size) {
    if (estimate != NULL) memset(estimate, 0, sizeof(*estimate));

    uint32_t first_column_index = 0;
    uint32_t second_column_index = 0;
    GenericSecondaryIndex* canonical_first = NULL;
    GenericSecondaryIndex* canonical_second = NULL;
    if (estimate == NULL ||
        !validate_pair(table,
                       schema,
                       first,
                       second,
                       &first_column_index,
                       &second_column_index,
                       &canonical_first,
                       &canonical_second)) {
        set_message(message, message_size, "invalid generic pair estimate request");
        return false;
    }

    const TinyDBGenericPredicate* canonical_first_predicate = NULL;
    const TinyDBGenericPredicate* canonical_second_predicate = NULL;
    if (canonical_first == first) {
        canonical_first_predicate = first_predicate;
        canonical_second_predicate = second_predicate;
    } else {
        canonical_first_predicate = second_predicate;
        canonical_second_predicate = first_predicate;
    }
    if (!predicate_matches_index(schema,
                                 canonical_first,
                                 canonical_first_predicate,
                                 first_column_index) ||
        !predicate_matches_index(schema,
                                 canonical_second,
                                 canonical_second_predicate,
                                 second_column_index)) {
        set_message(message,
                    message_size,
                    "pairwise correlation estimates require two indexed equality predicates");
        return false;
    }

    uint64_t epoch = 0;
    if (!tinydb_generic_index_epoch_current(table, &epoch)) {
        set_message(message, message_size, "unable to read generic index epoch");
        return false;
    }

    GenericPairValue target;
    target.first = canonical_first_predicate->value;
    target.second = canonical_second_predicate->value;

    GenericPairStats stats;
    if (!table->in_transaction &&
        load_stats(table,
                   schema,
                   canonical_first,
                   canonical_second,
                   first_column_index,
                   second_column_index,
                   epoch,
                   &stats)) {
        estimate->candidate_count = estimate_pair(&stats,
                                                  &target,
                                                  &estimate->exact_mcv);
        estimate->total_count = stats.total_count;
        estimate->persisted = true;
        set_message(message, message_size, "ok");
        return true;
    }

    GenericPairValue* sorted_pairs = NULL;
    if (!build_stats(table,
                     schema,
                     first_column_index,
                     second_column_index,
                     epoch,
                     &stats,
                     &sorted_pairs)) {
        set_message(message, message_size, "unable to scan pairwise correlation values");
        return false;
    }

    estimate->candidate_count = exact_frequency(sorted_pairs,
                                                stats.total_count,
                                                &target);
    estimate->total_count = stats.total_count;
    estimate->exact_mcv = true;
    estimate->persisted = false;

    if (!table->in_transaction &&
        stats.total_count >= GENERIC_CORR_PERSIST_THRESHOLD) {
        if (!write_stats(table,
                         schema,
                         canonical_first,
                         canonical_second,
                         &stats)) {
            free(sorted_pairs);
            set_message(message,
                        message_size,
                        "unable to persist pairwise correlation statistics");
            return false;
        }
        estimate->persisted = true;
    }

    free(sorted_pairs);
    set_message(message, message_size, "ok");
    return true;
}
