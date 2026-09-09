#include "generic_index_candidates.h"
#include "generic_index_epoch.h"
#include "record_payload.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define CANDIDATE_INDEX_MAGIC 0x47495231u /* GIR1: shared typed range snapshot */
#define CANDIDATE_INDEX_VERSION 2u
#define CANDIDATE_FNV_OFFSET 1469598103934665603ULL
#define CANDIDATE_FNV_PRIME 1099511628211ULL

typedef struct {
    uint32_t int_key;
    char text_key[TINYDB_RECORD_TEXT_MAX + 1];
    uint32_t primary_key;
} CandidateEntry;

typedef struct {
    CandidateEntry* entries;
    uint32_t count;
    uint32_t capacity;
    ColumnType column_type;
} CandidateSnapshot;

typedef struct {
    const TableSchema* schema;
    uint32_t column_index;
    CandidateSnapshot* snapshot;
    bool failed;
} CandidateBuildContext;

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
        hash *= CANDIDATE_FNV_PRIME;
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

static uint64_t schema_layout_fingerprint(const TableSchema* schema) {
    if (schema == NULL) return 0;

    uint64_t hash = CANDIDATE_FNV_OFFSET;
    unsigned char u32[4];
    encode_u32(schema->num_columns, u32);
    hash = fnv_update(hash, u32, sizeof(u32));
    encode_u32(schema->row_size, u32);
    hash = fnv_update(hash, u32, sizeof(u32));

    for (uint32_t i = 0; i < schema->num_columns; i++) {
        char name[MAX_NAME_SIZE] = {0};
        snprintf(name, sizeof(name), "%s", schema->columns[i].name);
        hash = fnv_update(hash, name, sizeof(name));
        encode_u32((uint32_t)schema->columns[i].type, u32);
        hash = fnv_update(hash, u32, sizeof(u32));
        encode_u32(schema->columns[i].size, u32);
        hash = fnv_update(hash, u32, sizeof(u32));
        encode_u32(schema->columns[i].offset, u32);
        hash = fnv_update(hash, u32, sizeof(u32));
    }
    return hash;
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

#ifndef _WIN32
static bool sync_parent_directory(const char* filename) {
    if (filename == NULL || filename[0] == '\0') return false;
    char directory[640];
    int written = snprintf(directory, sizeof(directory), "%s", filename);
    if (written < 0 || (size_t)written >= sizeof(directory)) return false;
    char* slash = strrchr(directory, '/');
    if (slash == NULL) {
        snprintf(directory, sizeof(directory), ".");
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    int fd = open(directory, O_RDONLY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}
#endif

static bool remove_file_durably_if_present(const char* filename) {
    if (filename == NULL || filename[0] == '\0') return false;
    errno = 0;
    if (remove(filename) != 0) return errno == ENOENT;
#ifndef _WIN32
    return sync_parent_directory(filename);
#else
    return true;
#endif
}

static bool replace_file_atomically(const char* temporary,
                                    const char* destination) {
#ifdef _WIN32
    return MoveFileExA(temporary,
                       destination,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (rename(temporary, destination) != 0) return false;
    return sync_parent_directory(destination);
#endif
}

static bool test_fail_before_snapshot_replace(void) {
    const char* value = getenv("TINYDB_TEST_FAIL_GENERIC_INDEX_SNAPSHOT_BEFORE_REPLACE");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool snapshot_filename(const GenericSecondaryIndex* index,
                              char* output,
                              size_t output_size) {
    int written = snprintf(output,
                           output_size,
                           "%s.range",
                           index->index_filename);
    return written >= 0 && (size_t)written < output_size;
}

static bool reserve_entries(CandidateSnapshot* snapshot, uint32_t needed) {
    if (needed <= snapshot->capacity) return true;
    uint32_t capacity = snapshot->capacity == 0 ? 32u : snapshot->capacity;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    CandidateEntry* grown = (CandidateEntry*)realloc(
        snapshot->entries, (size_t)capacity * sizeof(CandidateEntry));
    if (grown == NULL) return false;
    snapshot->entries = grown;
    snapshot->capacity = capacity;
    return true;
}

static int compare_int_entries(const void* left, const void* right) {
    const CandidateEntry* a = (const CandidateEntry*)left;
    const CandidateEntry* b = (const CandidateEntry*)right;
    if (a->int_key < b->int_key) return -1;
    if (a->int_key > b->int_key) return 1;
    if (a->primary_key < b->primary_key) return -1;
    if (a->primary_key > b->primary_key) return 1;
    return 0;
}

static int compare_text_entries(const void* left, const void* right) {
    const CandidateEntry* a = (const CandidateEntry*)left;
    const CandidateEntry* b = (const CandidateEntry*)right;
    int compared = strcmp(a->text_key, b->text_key);
    if (compared != 0) return compared;
    if (a->primary_key < b->primary_key) return -1;
    if (a->primary_key > b->primary_key) return 1;
    return 0;
}

static bool entry_less_or_equal(const CandidateSnapshot* snapshot,
                                const CandidateEntry* left,
                                const CandidateEntry* right) {
    int compared = snapshot->column_type == COL_TYPE_INT
        ? compare_int_entries(left, right)
        : compare_text_entries(left, right);
    return compared <= 0;
}

static bool append_entry_from_values(CandidateBuildContext* context,
                                     const TinyDBValue* values,
                                     uint32_t value_count) {
    if (context == NULL || context->schema == NULL ||
        context->snapshot == NULL || values == NULL ||
        value_count != context->schema->num_columns ||
        context->column_index >= value_count ||
        values[0].type != COL_TYPE_INT ||
        values[context->column_index].type !=
            context->schema->columns[context->column_index].type ||
        !reserve_entries(context->snapshot,
                         context->snapshot->count + 1u)) {
        if (context != NULL) context->failed = true;
        return false;
    }

    CandidateEntry* entry =
        &context->snapshot->entries[context->snapshot->count++];
    memset(entry, 0, sizeof(*entry));
    const TinyDBValue* key = &values[context->column_index];
    if (key->type == COL_TYPE_INT) {
        entry->int_key = key->int_value;
    } else {
        snprintf(entry->text_key, sizeof(entry->text_key), "%s", key->text);
    }
    entry->primary_key = values[0].int_value;
    return true;
}

static bool build_entry(const TableSchema* schema,
                        const TinyDBRecord* record,
                        void* raw_context) {
    CandidateBuildContext* context = (CandidateBuildContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_decode(schema,
                              record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &value_count,
                              message,
                              sizeof(message))) {
        context->failed = true;
        return false;
    }
    return append_entry_from_values(context, values, value_count);
}

static bool build_payload_entry(const TableSchema* schema,
                                const TinyDBRecordPayload* payload,
                                void* raw_context) {
    CandidateBuildContext* context = (CandidateBuildContext*)raw_context;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_decode_values(schema,
                                             payload,
                                             values,
                                             MAX_COLUMNS_PER_TABLE,
                                             &value_count,
                                             message,
                                             sizeof(message))) {
        context->failed = true;
        return false;
    }
    return append_entry_from_values(context, values, value_count);
}

static bool write_snapshot(const TableSchema* schema,
                           const GenericSecondaryIndex* index,
                           uint32_t column_index,
                           const CandidateSnapshot* snapshot,
                           uint64_t epoch) {
    char filename[600];
    if (!snapshot_filename(index, filename, sizeof(filename))) return false;

    char temporary[640];
    int written = snprintf(temporary, sizeof(temporary), "%s.tmp", filename);
    if (written < 0 || (size_t)written >= sizeof(temporary)) return false;

    FILE* file = fopen(temporary, "wb");
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

    uint64_t hash = CANDIDATE_FNV_OFFSET;
    bool ok = write_u32(file, &hash, CANDIDATE_INDEX_MAGIC) &&
              write_u32(file, &hash, CANDIDATE_INDEX_VERSION) &&
              write_u64(file, &hash, epoch) &&
              write_u32(file, &hash, schema->root_page_num) &&
              write_u32(file, &hash, column_index) &&
              write_u32(file,
                        &hash,
                        (uint32_t)schema->columns[column_index].type) &&
              write_u64(file, &hash, schema_layout_fingerprint(schema)) &&
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
    if (!ok) {
        (void)remove_file_durably_if_present(temporary);
        return false;
    }

    /* The old final snapshot remains authoritative until this point. Leaving
     * the fully synced temp behind models a crash just before atomic replace. */
    if (test_fail_before_snapshot_replace()) return false;

    if (!replace_file_atomically(temporary, filename)) {
        (void)remove_file_durably_if_present(temporary);
        return false;
    }
    return true;
}

static bool load_snapshot(Table* table,
                          const TableSchema* schema,
                          const GenericSecondaryIndex* index,
                          uint32_t column_index,
                          CandidateSnapshot* snapshot,
                          uint64_t expected_epoch) {
    char filename[600];
    if (!snapshot_filename(index, filename, sizeof(filename))) return false;
    FILE* file = fopen(filename, "rb");
    if (file == NULL) return false;

    uint64_t hash = CANDIDATE_FNV_OFFSET;
    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t epoch = 0;
    uint32_t root_page_num = 0;
    uint32_t stored_column_index = 0;
    uint32_t column_type = 0;
    uint64_t stored_schema_fingerprint = 0;
    uint32_t count = 0;
    char table_name[MAX_NAME_SIZE] = {0};
    char index_name[MAX_NAME_SIZE] = {0};
    char column_name[MAX_NAME_SIZE] = {0};

    bool ok = read_u32(file, &hash, &magic) &&
              read_u32(file, &hash, &version) &&
              read_u64(file, &hash, &epoch) &&
              read_u32(file, &hash, &root_page_num) &&
              read_u32(file, &hash, &stored_column_index) &&
              read_u32(file, &hash, &column_type) &&
              read_u64(file, &hash, &stored_schema_fingerprint) &&
              read_u32(file, &hash, &count) &&
              read_bytes(file, &hash, table_name, sizeof(table_name)) &&
              read_bytes(file, &hash, index_name, sizeof(index_name)) &&
              read_bytes(file, &hash, column_name, sizeof(column_name));

    table_name[MAX_NAME_SIZE - 1] = '\0';
    index_name[MAX_NAME_SIZE - 1] = '\0';
    column_name[MAX_NAME_SIZE - 1] = '\0';

    uint64_t max_entries =
        (uint64_t)table->pager->num_pages * (uint64_t)LEAF_NODE_MAX_CELLS;
    if (!ok || magic != CANDIDATE_INDEX_MAGIC ||
        version != CANDIDATE_INDEX_VERSION || epoch != expected_epoch ||
        root_page_num != schema->root_page_num ||
        stored_column_index != column_index ||
        column_type != (uint32_t)schema->columns[column_index].type ||
        stored_schema_fingerprint != schema_layout_fingerprint(schema) ||
        (uint64_t)count > max_entries ||
        !ci_equal(table_name, schema->name) ||
        !ci_equal(index_name, index->name) ||
        !ci_equal(column_name, schema->columns[column_index].name) ||
        !reserve_entries(snapshot, count)) {
        fclose(file);
        snapshot->count = 0;
        return false;
    }

    snapshot->column_type = (ColumnType)column_type;
    snapshot->count = 0;
    for (uint32_t i = 0; ok && i < count; i++) {
        CandidateEntry* entry = &snapshot->entries[i];
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

    if (ok && snapshot->count > 1) {
        for (uint32_t i = 1; i < snapshot->count; i++) {
            if (!entry_less_or_equal(snapshot,
                                     &snapshot->entries[i - 1u],
                                     &snapshot->entries[i])) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) snapshot->count = 0;
    return ok;
}

static void discard_snapshot(CandidateSnapshot* snapshot) {
    if (snapshot == NULL) return;
    free(snapshot->entries);
    snapshot->entries = NULL;
    snapshot->count = 0u;
    snapshot->capacity = 0u;
}

static bool ensure_snapshot(Table* table,
                            const TableSchema* schema,
                            const GenericSecondaryIndex* index,
                            uint32_t column_index,
                            CandidateSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->column_type = schema->columns[column_index].type;

    uint64_t epoch = 0;
    if (!tinydb_generic_index_epoch_current(table, &epoch)) return false;
    if (!table->in_transaction &&
        load_snapshot(table,
                      schema,
                      index,
                      column_index,
                      snapshot,
                      epoch)) {
        return true;
    }

    CandidateBuildContext context;
    memset(&context, 0, sizeof(context));
    context.schema = schema;
    context.column_index = column_index;
    context.snapshot = snapshot;

    bool scan_complete = true;
    if (schema->row_size > ROW_SIZE) {
        char scan_message[TINYDB_RECORD_MESSAGE_MAX];
        scan_complete = false;
        (void)tinydb_record_payload_scan(table,
                                         schema,
                                         build_payload_entry,
                                         &context,
                                         &scan_complete,
                                         scan_message,
                                         sizeof(scan_message));
    } else {
        (void)tinydb_record_scan(table, schema, build_entry, &context);
    }
    if (context.failed || !scan_complete) {
        discard_snapshot(snapshot);
        return false;
    }

    if (snapshot->count > 1) {
        qsort(snapshot->entries,
              snapshot->count,
              sizeof(CandidateEntry),
              snapshot->column_type == COL_TYPE_INT
                  ? compare_int_entries
                  : compare_text_entries);
    }
    if (!table->in_transaction) {
        (void)write_snapshot(schema,
                             index,
                             column_index,
                             snapshot,
                             epoch);
    }
    return true;
}

static int compare_entry_value(const CandidateSnapshot* snapshot,
                               const CandidateEntry* entry,
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

static size_t lower_bound(const CandidateSnapshot* snapshot,
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

static size_t upper_bound(const CandidateSnapshot* snapshot,
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

static void predicate_bounds(const CandidateSnapshot* snapshot,
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

static bool validate_single_request(const TableSchema* schema,
                                    const GenericSecondaryIndex* index,
                                    const TinyDBGenericPredicate* predicate) {
    return schema != NULL && index != NULL && predicate != NULL &&
           predicate->op != TINYDB_GENERIC_COMPARE_LIKE &&
           predicate->column_index != 0 &&
           predicate->column_index < schema->num_columns &&
           index->enabled && index->num_columns == 1 &&
           ci_equal(index->table_name, schema->name) &&
           ci_equal(index->column_name,
                    schema->columns[predicate->column_index].name) &&
           predicate->value.type == schema->columns[predicate->column_index].type;
}

static bool validate_conjunctive_request(
    const TableSchema* schema,
    const GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    uint32_t* column_index) {
    if (schema == NULL || index == NULL || predicates == NULL ||
        predicate_count == 0 || column_index == NULL) {
        return false;
    }
    *column_index = predicates[0].column_index;
    if (*column_index == 0 || *column_index >= schema->num_columns ||
        !index->enabled || index->num_columns != 1 ||
        !ci_equal(index->table_name, schema->name) ||
        !ci_equal(index->column_name, schema->columns[*column_index].name)) {
        return false;
    }
    for (uint32_t i = 0; i < predicate_count; i++) {
        if (predicates[i].column_index != *column_index ||
            predicates[i].op > TINYDB_GENERIC_COMPARE_LTE ||
            predicates[i].value.type != schema->columns[*column_index].type) {
            return false;
        }
    }
    return true;
}

static void conjunctive_bounds(const CandidateSnapshot* snapshot,
                               const TinyDBGenericPredicate* predicates,
                               uint32_t predicate_count,
                               size_t* start,
                               size_t* end) {
    *start = 0;
    *end = snapshot->count;
    for (uint32_t i = 0; i < predicate_count; i++) {
        size_t predicate_start = 0;
        size_t predicate_end = snapshot->count;
        predicate_bounds(snapshot,
                         &predicates[i],
                         &predicate_start,
                         &predicate_end);
        if (predicate_start > *start) *start = predicate_start;
        if (predicate_end < *end) *end = predicate_end;
        if (*start >= *end) {
            *start = *end;
            break;
        }
    }
}

bool tinydb_generic_index_collect_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
    TinyDBGenericIndexCandidates* candidates,
    char* message,
    size_t message_size) {
    if (candidates != NULL) memset(candidates, 0, sizeof(*candidates));
    if (table == NULL || candidates == NULL ||
        !validate_single_request(schema, index, predicate)) {
        set_message(message, message_size, "invalid ordered generic index candidate request");
        return false;
    }

    CandidateSnapshot snapshot;
    if (!ensure_snapshot(table,
                         schema,
                         index,
                         predicate->column_index,
                         &snapshot)) {
        set_message(message,
                    message_size,
                    "unable to load or rebuild typed generic index snapshot");
        return false;
    }

    size_t start = 0;
    size_t end = 0;
    predicate_bounds(&snapshot, predicate, &start, &end);
    size_t count = end >= start ? end - start : 0;
    if (count > UINT32_MAX) {
        free(snapshot.entries);
        set_message(message, message_size, "generic index candidate set is too large");
        return false;
    }

    if (count > 0) {
        candidates->ids = (uint32_t*)malloc(count * sizeof(uint32_t));
        if (candidates->ids == NULL) {
            free(snapshot.entries);
            set_message(message, message_size, "out of memory collecting index candidates");
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            candidates->ids[i] = snapshot.entries[start + i].primary_key;
        }
        candidates->count = (uint32_t)count;
    }

    free(snapshot.entries);
    set_message(message, message_size, "ok");
    return true;
}

bool tinydb_generic_index_collect_conjunctive_candidates(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicates,
    uint32_t predicate_count,
    TinyDBGenericIndexCandidates* candidates,
    char* message,
    size_t message_size) {
    if (candidates != NULL) memset(candidates, 0, sizeof(*candidates));
    uint32_t column_index = 0;
    if (table == NULL || candidates == NULL ||
        !validate_conjunctive_request(schema,
                                      index,
                                      predicates,
                                      predicate_count,
                                      &column_index)) {
        set_message(message,
                    message_size,
                    "invalid conjunctive generic index candidate request");
        return false;
    }

    CandidateSnapshot snapshot;
    if (!ensure_snapshot(table,
                         schema,
                         index,
                         column_index,
                         &snapshot)) {
        set_message(message,
                    message_size,
                    "unable to load or rebuild typed generic index snapshot");
        return false;
    }

    size_t start = 0;
    size_t end = 0;
    conjunctive_bounds(&snapshot,
                       predicates,
                       predicate_count,
                       &start,
                       &end);
    size_t count = end >= start ? end - start : 0;
    if (count > UINT32_MAX) {
        free(snapshot.entries);
        set_message(message,
                    message_size,
                    "generic conjunctive candidate set is too large");
        return false;
    }

    if (count > 0) {
        candidates->ids = (uint32_t*)malloc(count * sizeof(uint32_t));
        if (candidates->ids == NULL) {
            free(snapshot.entries);
            set_message(message,
                        message_size,
                        "out of memory collecting conjunctive index candidates");
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            candidates->ids[i] = snapshot.entries[start + i].primary_key;
        }
        candidates->count = (uint32_t)count;
    }

    free(snapshot.entries);
    set_message(message, message_size, "ok");
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
    if (table == NULL || estimate == NULL ||
        !validate_single_request(schema, index, predicate)) {
        set_message(message, message_size, "invalid ordered generic index estimate request");
        return false;
    }

    CandidateSnapshot snapshot;
    if (!ensure_snapshot(table,
                         schema,
                         index,
                         predicate->column_index,
                         &snapshot)) {
        set_message(message,
                    message_size,
                    "unable to load or rebuild typed generic index snapshot");
        return false;
    }

    size_t start = 0;
    size_t end = 0;
    predicate_bounds(&snapshot, predicate, &start, &end);
    size_t count = end >= start ? end - start : 0;
    if (count > UINT32_MAX || snapshot.count > UINT32_MAX) {
        free(snapshot.entries);
        set_message(message, message_size, "generic index estimate is too large");
        return false;
    }
    estimate->candidate_count = (uint32_t)count;
    estimate->total_count = snapshot.count;
    free(snapshot.entries);
    set_message(message, message_size, "ok");
    return true;
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
    uint32_t column_index = 0;
    if (table == NULL || estimate == NULL ||
        !validate_conjunctive_request(schema,
                                      index,
                                      predicates,
                                      predicate_count,
                                      &column_index)) {
        set_message(message,
                    message_size,
                    "invalid conjunctive generic index estimate request");
        return false;
    }

    CandidateSnapshot snapshot;
    if (!ensure_snapshot(table,
                         schema,
                         index,
                         column_index,
                         &snapshot)) {
        set_message(message,
                    message_size,
                    "unable to load or rebuild typed generic index snapshot");
        return false;
    }

    size_t start = 0;
    size_t end = 0;
    conjunctive_bounds(&snapshot,
                       predicates,
                       predicate_count,
                       &start,
                       &end);
    size_t count = end >= start ? end - start : 0;
    if (count > UINT32_MAX || snapshot.count > UINT32_MAX) {
        free(snapshot.entries);
        set_message(message,
                    message_size,
                    "generic conjunctive index estimate is too large");
        return false;
    }
    estimate->candidate_count = (uint32_t)count;
    estimate->total_count = snapshot.count;
    free(snapshot.entries);
    set_message(message, message_size, "ok");
    return true;
}

void tinydb_generic_index_candidates_free(
    TinyDBGenericIndexCandidates* candidates) {
    if (candidates == NULL) return;
    free(candidates->ids);
    candidates->ids = NULL;
    candidates->count = 0;
}
