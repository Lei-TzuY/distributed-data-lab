#include "generic_index_candidates.h"
#include "generic_index_epoch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define GENERIC_STATS_MAGIC 0x47495331u /* GIS1 */
#define GENERIC_STATS_VERSION 3u
#define GENERIC_STATS_SAMPLE_MAX 33u
#define GENERIC_STATS_MCV_MAX 16u
#define GENERIC_STATS_FNV_OFFSET 1469598103934665603ULL
#define GENERIC_STATS_FNV_PRIME 1099511628211ULL

typedef struct {
    uint32_t rank;
    TinyDBValue value;
} GenericStatsSample;

typedef struct {
    TinyDBValue value;
    uint32_t frequency;
    uint32_t rows_before;
} GenericStatsMcv;

typedef struct {
    uint64_t epoch;
    uint32_t total_count;
    uint32_t distinct_count;
    uint32_t sample_count;
    uint32_t mcv_count;
    ColumnType column_type;
    GenericStatsSample samples[GENERIC_STATS_SAMPLE_MAX];
    GenericStatsMcv mcvs[GENERIC_STATS_MCV_MAX];
} GenericIndexStats;

typedef struct {
    const TableSchema* schema;
    uint32_t column_index;
    TinyDBValue* values;
    uint32_t count;
    uint32_t capacity;
    bool failed;
} GenericStatsBuildContext;

bool tinydb_generic_index_estimate_candidates_exact_base(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size);

bool tinydb_generic_index_estimate_conjunctive_candidates_exact_base(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size);

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message == NULL || message_size == 0) return;
    snprintf(message, message_size, "%s", text);
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

static uint64_t fnv_update(uint64_t hash, const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= GENERIC_STATS_FNV_PRIME;
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

static bool stats_filename(const GenericSecondaryIndex* index,
                           char* output,
                           size_t output_size) {
    if (index == NULL) return false;
    int written = snprintf(output,
                           output_size,
                           "%s.range.stats",
                           index->index_filename);
    return written >= 0 && (size_t)written < output_size;
}

static int compare_values(ColumnType type,
                          const TinyDBValue* left,
                          const TinyDBValue* right) {
    if (type == COL_TYPE_INT) {
        if (left->int_value < right->int_value) return -1;
        if (left->int_value > right->int_value) return 1;
        return 0;
    }
    return strcmp(left->text, right->text);
}

static int compare_int_values(const void* left, const void* right) {
    const TinyDBValue* a = (const TinyDBValue*)left;
    const TinyDBValue* b = (const TinyDBValue*)right;
    if (a->int_value < b->int_value) return -1;
    if (a->int_value > b->int_value) return 1;
    return 0;
}

static int compare_text_values(const void* left, const void* right) {
    const TinyDBValue* a = (const TinyDBValue*)left;
    const TinyDBValue* b = (const TinyDBValue*)right;
    return strcmp(a->text, b->text);
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
    value->text[sizeof(value->text) - 1] = '\0';
    return true;
}

static bool reserve_values(GenericStatsBuildContext* context,
                           uint32_t needed) {
    if (needed <= context->capacity) return true;
    uint32_t capacity = context->capacity == 0 ? 64u : context->capacity;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    TinyDBValue* grown = (TinyDBValue*)realloc(
        context->values, (size_t)capacity * sizeof(TinyDBValue));
    if (grown == NULL) return false;
    context->values = grown;
    context->capacity = capacity;
    return true;
}

static bool collect_value(const TableSchema* schema,
                          const TinyDBRecord* record,
                          void* raw_context) {
    GenericStatsBuildContext* context =
        (GenericStatsBuildContext*)raw_context;
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
        !reserve_values(context, context->count + 1u)) {
        context->failed = true;
        return false;
    }
    context->values[context->count++] = values[context->column_index];
    return true;
}

static bool mcv_better(ColumnType type,
                       const TinyDBValue* value,
                       uint32_t frequency,
                       const GenericStatsMcv* current) {
    if (frequency != current->frequency) return frequency > current->frequency;
    return compare_values(type, value, &current->value) < 0;
}

static void consider_mcv(GenericIndexStats* stats,
                         const TinyDBValue* value,
                         uint32_t frequency,
                         uint32_t rows_before) {
    uint32_t position = stats->mcv_count;
    if (stats->mcv_count < GENERIC_STATS_MCV_MAX) {
        stats->mcv_count++;
    } else {
        position = GENERIC_STATS_MCV_MAX - 1u;
        if (!mcv_better(stats->column_type,
                        value,
                        frequency,
                        &stats->mcvs[position])) {
            return;
        }
    }

    stats->mcvs[position].value = *value;
    stats->mcvs[position].frequency = frequency;
    stats->mcvs[position].rows_before = rows_before;
    while (position > 0 &&
           mcv_better(stats->column_type,
                      &stats->mcvs[position].value,
                      stats->mcvs[position].frequency,
                      &stats->mcvs[position - 1u])) {
        GenericStatsMcv swap = stats->mcvs[position - 1u];
        stats->mcvs[position - 1u] = stats->mcvs[position];
        stats->mcvs[position] = swap;
        position--;
    }
}

static bool write_stats(const TableSchema* schema,
                        const GenericSecondaryIndex* index,
                        uint32_t column_index,
                        const GenericIndexStats* stats) {
    char filename[640];
    if (!stats_filename(index, filename, sizeof(filename))) return false;
    FILE* file = fopen(filename, "wb");
    if (file == NULL) return false;

    char table_name[MAX_NAME_SIZE] = {0};
    char index_name[MAX_NAME_SIZE] = {0};
    char column_name[MAX_NAME_SIZE] = {0};
    snprintf(table_name, sizeof(table_name), "%s", schema->name);
    snprintf(index_name, sizeof(index_name), "%s", index->name);
    snprintf(column_name,
             sizeof(column_name),
             "%s",
             schema->columns[column_index].name);

    uint64_t hash = GENERIC_STATS_FNV_OFFSET;
    bool ok = write_u32(file, &hash, GENERIC_STATS_MAGIC) &&
              write_u32(file, &hash, GENERIC_STATS_VERSION) &&
              write_u64(file, &hash, stats->epoch) &&
              write_u32(file, &hash, schema->root_page_num) &&
              write_u32(file, &hash, column_index) &&
              write_u32(file,
                        &hash,
                        (uint32_t)schema->columns[column_index].type) &&
              write_u32(file, &hash, stats->total_count) &&
              write_u32(file, &hash, stats->distinct_count) &&
              write_u32(file, &hash, stats->sample_count) &&
              write_u32(file, &hash, stats->mcv_count) &&
              write_bytes(file, &hash, table_name, sizeof(table_name)) &&
              write_bytes(file, &hash, index_name, sizeof(index_name)) &&
              write_bytes(file, &hash, column_name, sizeof(column_name));

    for (uint32_t i = 0; ok && i < stats->sample_count; i++) {
        ok = write_u32(file, &hash, stats->samples[i].rank) &&
             write_typed_value(file,
                               &hash,
                               stats->column_type,
                               &stats->samples[i].value);
    }
    for (uint32_t i = 0; ok && i < stats->mcv_count; i++) {
        ok = write_typed_value(file,
                               &hash,
                               stats->column_type,
                               &stats->mcvs[i].value) &&
             write_u32(file, &hash, stats->mcvs[i].frequency) &&
             write_u32(file, &hash, stats->mcvs[i].rows_before);
    }

    ok = ok && write_u64(file, NULL, hash);
    if (ok) ok = sync_file(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool load_stats(Table* table,
                       const TableSchema* schema,
                       const GenericSecondaryIndex* index,
                       uint32_t column_index,
                       uint64_t expected_epoch,
                       GenericIndexStats* stats) {
    char filename[640];
    if (!stats_filename(index, filename, sizeof(filename))) return false;
    FILE* file = fopen(filename, "rb");
    if (file == NULL) return false;

    memset(stats, 0, sizeof(*stats));
    uint64_t hash = GENERIC_STATS_FNV_OFFSET;
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t root_page_num = 0;
    uint32_t stored_column_index = 0;
    uint32_t stored_column_type = 0;
    char table_name[MAX_NAME_SIZE] = {0};
    char index_name[MAX_NAME_SIZE] = {0};
    char column_name[MAX_NAME_SIZE] = {0};

    bool ok = read_u32(file, &hash, &magic) &&
              read_u32(file, &hash, &version) &&
              read_u64(file, &hash, &stats->epoch) &&
              read_u32(file, &hash, &root_page_num) &&
              read_u32(file, &hash, &stored_column_index) &&
              read_u32(file, &hash, &stored_column_type) &&
              read_u32(file, &hash, &stats->total_count) &&
              read_u32(file, &hash, &stats->distinct_count) &&
              read_u32(file, &hash, &stats->sample_count) &&
              read_u32(file, &hash, &stats->mcv_count) &&
              read_bytes(file, &hash, table_name, sizeof(table_name)) &&
              read_bytes(file, &hash, index_name, sizeof(index_name)) &&
              read_bytes(file, &hash, column_name, sizeof(column_name));

    table_name[MAX_NAME_SIZE - 1] = '\0';
    index_name[MAX_NAME_SIZE - 1] = '\0';
    column_name[MAX_NAME_SIZE - 1] = '\0';

    uint64_t max_entries =
        (uint64_t)table->pager->num_pages * (uint64_t)LEAF_NODE_MAX_CELLS;
    if (!ok || magic != GENERIC_STATS_MAGIC ||
        version != GENERIC_STATS_VERSION || stats->epoch != expected_epoch ||
        root_page_num != schema->root_page_num ||
        stored_column_index != column_index ||
        stored_column_type != (uint32_t)schema->columns[column_index].type ||
        (uint64_t)stats->total_count > max_entries ||
        stats->distinct_count > stats->total_count ||
        stats->sample_count > GENERIC_STATS_SAMPLE_MAX ||
        stats->mcv_count > GENERIC_STATS_MCV_MAX ||
        stats->mcv_count > stats->distinct_count ||
        (stats->total_count == 0 &&
         (stats->sample_count != 0 || stats->mcv_count != 0)) ||
        (stats->total_count > 0 &&
         (stats->sample_count == 0 || stats->mcv_count == 0)) ||
        !ci_equal(table_name, schema->name) ||
        !ci_equal(index_name, index->name) ||
        !ci_equal(column_name, schema->columns[column_index].name)) {
        fclose(file);
        return false;
    }

    stats->column_type = (ColumnType)stored_column_type;
    for (uint32_t i = 0; ok && i < stats->sample_count; i++) {
        GenericStatsSample* sample = &stats->samples[i];
        ok = read_u32(file, &hash, &sample->rank) &&
             read_typed_value(file,
                              &hash,
                              stats->column_type,
                              &sample->value);
        if (ok && sample->rank >= stats->total_count) ok = false;
        if (ok && i > 0) {
            if (sample->rank <= stats->samples[i - 1u].rank ||
                compare_values(stats->column_type,
                               &stats->samples[i - 1u].value,
                               &sample->value) > 0) {
                ok = false;
            }
        }
    }

    uint64_t mcv_rows = 0;
    for (uint32_t i = 0; ok && i < stats->mcv_count; i++) {
        GenericStatsMcv* mcv = &stats->mcvs[i];
        ok = read_typed_value(file,
                              &hash,
                              stats->column_type,
                              &mcv->value) &&
             read_u32(file, &hash, &mcv->frequency) &&
             read_u32(file, &hash, &mcv->rows_before);
        if (!ok || mcv->frequency == 0 || mcv->frequency > stats->total_count ||
            mcv->rows_before > stats->total_count - mcv->frequency) {
            ok = false;
            break;
        }
        if (compare_values(stats->column_type,
                           &mcv->value,
                           &stats->samples[0].value) < 0 ||
            compare_values(stats->column_type,
                           &mcv->value,
                           &stats->samples[stats->sample_count - 1u].value) > 0) {
            ok = false;
            break;
        }
        mcv_rows += mcv->frequency;
        if (mcv_rows > stats->total_count) {
            ok = false;
            break;
        }
        if (i > 0) {
            GenericStatsMcv* previous = &stats->mcvs[i - 1u];
            if (mcv->frequency > previous->frequency ||
                (mcv->frequency == previous->frequency &&
                 compare_values(stats->column_type,
                                &previous->value,
                                &mcv->value) >= 0)) {
                ok = false;
                break;
            }
        }
        for (uint32_t j = 0; j < i; j++) {
            int compared = compare_values(stats->column_type,
                                          &stats->mcvs[j].value,
                                          &mcv->value);
            if (compared == 0 ||
                (compared < 0 && stats->mcvs[j].rows_before >= mcv->rows_before) ||
                (compared > 0 && stats->mcvs[j].rows_before <= mcv->rows_before)) {
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

static bool build_stats(Table* table,
                        const TableSchema* schema,
                        const GenericSecondaryIndex* index,
                        uint32_t column_index,
                        uint64_t epoch,
                        GenericIndexStats* stats) {
    GenericStatsBuildContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    context.column_index = column_index;
    (void)tinydb_record_scan(table, schema, collect_value, &context);
    if (context.failed) {
        free(context.values);
        return false;
    }

    ColumnType type = schema->columns[column_index].type;
    if (context.count > 1) {
        qsort(context.values,
              context.count,
              sizeof(TinyDBValue),
              type == COL_TYPE_INT ? compare_int_values : compare_text_values);
    }

    memset(stats, 0, sizeof(*stats));
    stats->epoch = epoch;
    stats->column_type = type;
    stats->total_count = context.count;
    if (context.count > 0) {
        uint32_t run_start = 0;
        while (run_start < context.count) {
            uint32_t run_end = run_start + 1u;
            while (run_end < context.count &&
                   compare_values(type,
                                  &context.values[run_start],
                                  &context.values[run_end]) == 0) {
                run_end++;
            }
            stats->distinct_count++;
            consider_mcv(stats,
                         &context.values[run_start],
                         run_end - run_start,
                         run_start);
            run_start = run_end;
        }

        stats->sample_count = context.count < GENERIC_STATS_SAMPLE_MAX
            ? context.count
            : GENERIC_STATS_SAMPLE_MAX;
        for (uint32_t i = 0; i < stats->sample_count; i++) {
            uint32_t rank = 0;
            if (stats->sample_count > 1) {
                rank = (uint32_t)(((uint64_t)i *
                                   (uint64_t)(context.count - 1u)) /
                                  (uint64_t)(stats->sample_count - 1u));
            }
            stats->samples[i].rank = rank;
            stats->samples[i].value = context.values[rank];
        }
    }

    free(context.values);
    return write_stats(schema, index, column_index, stats);
}

static bool ensure_stats(Table* table,
                         const TableSchema* schema,
                         const GenericSecondaryIndex* index,
                         uint32_t column_index,
                         GenericIndexStats* stats) {
    if (table == NULL || schema == NULL || index == NULL ||
        column_index >= schema->num_columns || table->in_transaction) {
        return false;
    }

    uint64_t epoch = 0;
    if (!tinydb_generic_index_epoch_current(table, &epoch)) return false;
    if (load_stats(table,
                   schema,
                   index,
                   column_index,
                   epoch,
                   stats)) {
        return true;
    }
    return build_stats(table,
                       schema,
                       index,
                       column_index,
                       epoch,
                       stats);
}

static const GenericStatsMcv* find_mcv(const GenericIndexStats* stats,
                                       const TinyDBValue* value) {
    for (uint32_t i = 0; i < stats->mcv_count; i++) {
        if (compare_values(stats->column_type,
                           value,
                           &stats->mcvs[i].value) == 0) {
            return &stats->mcvs[i];
        }
    }
    return NULL;
}

static uint32_t estimate_equal(const GenericIndexStats* stats,
                               const TinyDBValue* value) {
    if (stats->total_count == 0 || stats->sample_count == 0 ||
        stats->distinct_count == 0) {
        return 0;
    }
    if (compare_values(stats->column_type,
                       value,
                       &stats->samples[0].value) < 0 ||
        compare_values(stats->column_type,
                       value,
                       &stats->samples[stats->sample_count - 1u].value) > 0) {
        return 0;
    }

    const GenericStatsMcv* matched_mcv = find_mcv(stats, value);
    if (matched_mcv != NULL) return matched_mcv->frequency;

    uint32_t mcv_rows = 0;
    for (uint32_t i = 0; i < stats->mcv_count; i++) {
        mcv_rows += stats->mcvs[i].frequency;
    }

    uint32_t remaining_distinct = stats->distinct_count - stats->mcv_count;
    uint32_t remaining_rows = stats->total_count - mcv_rows;
    if (remaining_distinct == 0 || remaining_rows == 0) return 0;
    uint32_t average =
        (remaining_rows + remaining_distinct - 1u) / remaining_distinct;
    return average == 0 ? 1u : average;
}

static uint32_t estimate_less_equal(const GenericIndexStats* stats,
                                    const TinyDBValue* value) {
    if (stats->total_count == 0 || stats->sample_count == 0) return 0;

    const GenericStatsMcv* matched_mcv = find_mcv(stats, value);
    if (matched_mcv != NULL) {
        return matched_mcv->rows_before + matched_mcv->frequency;
    }

    if (compare_values(stats->column_type,
                       value,
                       &stats->samples[0].value) < 0) {
        return 0;
    }
    if (compare_values(stats->column_type,
                       value,
                       &stats->samples[stats->sample_count - 1u].value) >= 0) {
        return stats->total_count;
    }

    uint32_t hi = 1;
    while (hi < stats->sample_count &&
           compare_values(stats->column_type,
                          &stats->samples[hi].value,
                          value) <= 0) {
        hi++;
    }
    uint32_t lo = hi - 1u;
    uint32_t lo_rank = stats->samples[lo].rank;
    uint32_t hi_rank = stats->samples[hi].rank;
    uint64_t rank = ((uint64_t)lo_rank + (uint64_t)hi_rank) / 2u;

    if (stats->column_type == COL_TYPE_INT) {
        uint32_t lo_value = stats->samples[lo].value.int_value;
        uint32_t hi_value = stats->samples[hi].value.int_value;
        uint32_t target = value->int_value;
        if (hi_value > lo_value && target >= lo_value && target <= hi_value) {
            uint64_t value_span = (uint64_t)hi_value - (uint64_t)lo_value;
            uint64_t value_offset = (uint64_t)target - (uint64_t)lo_value;
            uint64_t rank_span = (uint64_t)hi_rank - (uint64_t)lo_rank;
            rank = (uint64_t)lo_rank +
                   (value_offset * rank_span) / value_span;
        }
    }

    if (rank >= stats->total_count) return stats->total_count;
    return (uint32_t)rank + 1u;
}

static uint32_t estimate_predicate(const GenericIndexStats* stats,
                                   const TinyDBGenericPredicate* predicate) {
    uint32_t equal = estimate_equal(stats, &predicate->value);
    uint32_t less_equal = estimate_less_equal(stats, &predicate->value);
    switch (predicate->op) {
        case TINYDB_GENERIC_COMPARE_EQ:
            return equal;
        case TINYDB_GENERIC_COMPARE_LT:
            return less_equal > equal ? less_equal - equal : 0;
        case TINYDB_GENERIC_COMPARE_LTE:
            return less_equal;
        case TINYDB_GENERIC_COMPARE_GT:
            return stats->total_count > less_equal
                ? stats->total_count - less_equal
                : 0;
        case TINYDB_GENERIC_COMPARE_GTE: {
            uint32_t less = less_equal > equal ? less_equal - equal : 0;
            return stats->total_count > less
                ? stats->total_count - less
                : 0;
        }
        default:
            return stats->total_count;
    }
}

static bool predicate_request_valid(const TableSchema* schema,
                                    const GenericSecondaryIndex* index,
                                    const TinyDBGenericPredicate* predicate) {
    if (schema == NULL || index == NULL || predicate == NULL ||
        !index->enabled || index->num_columns != 1 ||
        predicate->column_index == 0 ||
        predicate->column_index >= schema->num_columns ||
        predicate->op > TINYDB_GENERIC_COMPARE_LTE) {
        return false;
    }
    return ci_equal(index->table_name, schema->name) &&
           ci_equal(index->column_name,
                    schema->columns[predicate->column_index].name) &&
           predicate->value.type == schema->columns[predicate->column_index].type;
}

static bool same_literal(const TinyDBGenericPredicate* left,
                         const TinyDBGenericPredicate* right) {
    if (left->value.type != right->value.type) return false;
    if (left->value.type == COL_TYPE_INT) {
        return left->value.int_value == right->value.int_value;
    }
    return strcmp(left->value.text, right->value.text) == 0;
}

static bool estimate_conjunction(const GenericIndexStats* stats,
                                 const TinyDBGenericPredicate* predicates,
                                 uint32_t predicate_count,
                                 uint32_t* candidate_count) {
    const TinyDBGenericPredicate* equality = NULL;
    const TinyDBGenericPredicate* lower = NULL;
    const TinyDBGenericPredicate* upper = NULL;

    for (uint32_t i = 0; i < predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &predicates[i];
        if (predicate->op == TINYDB_GENERIC_COMPARE_EQ) {
            if (equality != NULL && !same_literal(equality, predicate)) {
                *candidate_count = 0;
                return true;
            }
            equality = predicate;
        } else if (predicate->op == TINYDB_GENERIC_COMPARE_GT ||
                   predicate->op == TINYDB_GENERIC_COMPARE_GTE) {
            if (lower == NULL) {
                lower = predicate;
            } else {
                int compared = compare_values(stats->column_type,
                                              &predicate->value,
                                              &lower->value);
                if (compared > 0 ||
                    (compared == 0 && predicate->op == TINYDB_GENERIC_COMPARE_GT)) {
                    lower = predicate;
                }
            }
        } else if (predicate->op == TINYDB_GENERIC_COMPARE_LT ||
                   predicate->op == TINYDB_GENERIC_COMPARE_LTE) {
            if (upper == NULL) {
                upper = predicate;
            } else {
                int compared = compare_values(stats->column_type,
                                              &predicate->value,
                                              &upper->value);
                if (compared < 0 ||
                    (compared == 0 && predicate->op == TINYDB_GENERIC_COMPARE_LT)) {
                    upper = predicate;
                }
            }
        } else {
            return false;
        }
    }

    if (equality != NULL) {
        for (uint32_t i = 0; i < predicate_count; i++) {
            if (!tinydb_generic_predicate_matches(&predicates[i],
                                                  &equality->value)) {
                *candidate_count = 0;
                return true;
            }
        }
        *candidate_count = estimate_equal(stats, &equality->value);
        return true;
    }

    if (lower != NULL && upper != NULL) {
        int compared = compare_values(stats->column_type,
                                      &lower->value,
                                      &upper->value);
        if (compared > 0 ||
            (compared == 0 &&
             (lower->op == TINYDB_GENERIC_COMPARE_GT ||
              upper->op == TINYDB_GENERIC_COMPARE_LT))) {
            *candidate_count = 0;
            return true;
        }
    }

    uint32_t start = 0;
    uint32_t end = stats->total_count;
    if (lower != NULL) {
        uint32_t matched = estimate_predicate(stats, lower);
        start = stats->total_count > matched
            ? stats->total_count - matched
            : 0;
    }
    if (upper != NULL) {
        end = estimate_predicate(stats, upper);
    }
    *candidate_count = end > start ? end - start : 0;
    return true;
}

bool tinydb_generic_index_estimate_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size) {
    if (estimate != NULL) memset(estimate, 0, sizeof(*estimate));
    if (table != NULL && estimate != NULL &&
        predicate_request_valid(schema, index, predicate)) {
        GenericIndexStats stats;
        if (ensure_stats(table,
                         schema,
                         index,
                         predicate->column_index,
                         &stats)) {
            estimate->candidate_count = estimate_predicate(&stats, predicate);
            estimate->total_count = stats.total_count;
            set_message(message,
                        message_size,
                        "ok (persisted generic index statistics with MCV bounds)");
            return true;
        }
    }
    return tinydb_generic_index_estimate_candidates_exact_base(
        table,
        schema,
        index,
        predicate,
        estimate,
        message,
        message_size);
}

bool tinydb_generic_index_estimate_conjunctive_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericIndexEstimate* estimate,
    char* message,
    size_t message_size) {
    if (estimate != NULL) memset(estimate, 0, sizeof(*estimate));
    bool valid = table != NULL && estimate != NULL && predicates != NULL &&
                 predicate_count > 0;
    uint32_t column_index = 0;
    if (valid) {
        column_index = predicates[0].column_index;
        for (uint32_t i = 0; i < predicate_count; i++) {
            if (predicates[i].column_index != column_index ||
                !predicate_request_valid(schema, index, &predicates[i])) {
                valid = false;
                break;
            }
        }
    }

    if (valid) {
        GenericIndexStats stats;
        uint32_t candidate_count = 0;
        if (ensure_stats(table,
                         schema,
                         index,
                         column_index,
                         &stats) &&
            estimate_conjunction(&stats,
                                 predicates,
                                 predicate_count,
                                 &candidate_count)) {
            estimate->candidate_count = candidate_count;
            estimate->total_count = stats.total_count;
            set_message(message,
                        message_size,
                        "ok (persisted generic index statistics with MCV bounds)");
            return true;
        }
    }

    return tinydb_generic_index_estimate_conjunctive_candidates_exact_base(
        table,
        schema,
        index,
        predicates,
        predicate_count,
        estimate,
        message,
        message_size);
}
