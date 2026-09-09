#include "generic_index_stats_refresh.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool tinydb_generic_index_estimate_candidates_stats_base(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    const TinyDBGenericPredicate* predicate,
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

static int find_column_index(const TableSchema* schema, const char* name) {
    if (schema == NULL || name == NULL) return -1;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (ci_equal(schema->columns[i].name, name)) return (int)i;
    }
    return -1;
}

static bool stats_sidecar_exists(const GenericSecondaryIndex* index) {
    char filename[640];
    int written = snprintf(filename,
                           sizeof(filename),
                           "%s.range.stats",
                           index->index_filename);
    if (written < 0 || (size_t)written >= sizeof(filename)) return false;

    FILE* file = fopen(filename, "rb");
    if (file == NULL) return false;
    int first = fgetc(file);
    bool ok = first != EOF || !ferror(file);
    fclose(file);
    return ok;
}

bool tinydb_generic_index_refresh_statistics(
    Table* table,
    const TableSchema* schema,
    GenericSecondaryIndex* index,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0) message[0] = '\0';
    if (table == NULL || schema == NULL || index == NULL) {
        set_message(message,
                    message_size,
                    "table, schema, and index are required for optimizer statistics refresh");
        return false;
    }
    if (table->in_transaction) {
        set_message(message,
                    message_size,
                    "optimizer statistics cannot be refreshed inside a transaction");
        return false;
    }
    if (!index->enabled || index->num_columns != 1 ||
        !ci_equal(index->table_name, schema->name)) {
        set_message(message,
                    message_size,
                    "optimizer statistics require one enabled generic secondary index");
        return false;
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema,
                                        schema_message,
                                        sizeof(schema_message))) {
        set_message(message,
                    message_size,
                    "optimizer statistics require an executable generic fixed-slot schema");
        return false;
    }

    int column_index = find_column_index(schema, index->column_name);
    if (column_index <= 0) {
        set_message(message,
                    message_size,
                    "optimizer statistics require a valid non-primary indexed column");
        return false;
    }

    TinyDBGenericPredicate predicate;
    memset(&predicate, 0, sizeof(predicate));
    predicate.column_index = (uint32_t)column_index;
    predicate.op = TINYDB_GENERIC_COMPARE_EQ;
    predicate.value.type = schema->columns[column_index].type;

    TinyDBGenericIndexEstimate estimate;
    char estimate_message[256];
    if (!tinydb_generic_index_estimate_candidates_stats_base(
            table,
            schema,
            index,
            &predicate,
            &estimate,
            estimate_message,
            sizeof(estimate_message))) {
        set_message(message,
                    message_size,
                    estimate_message[0] != '\0'
                        ? estimate_message
                        : "unable to materialize optimizer statistics");
        return false;
    }

    const char* persisted_prefix = "ok (persisted generic index statistics";
    if (strncmp(estimate_message,
                persisted_prefix,
                strlen(persisted_prefix)) != 0 ||
        !stats_sidecar_exists(index)) {
        set_message(message,
                    message_size,
                    "optimizer statistics refresh fell back before durable persistence");
        return false;
    }

    (void)estimate;
    set_message(message, message_size, "ok");
    return true;
}
