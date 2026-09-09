#include "analyze_sql.h"
#include "generic_index_correlation.h"
#include "generic_index_stats_refresh.h"
#include "generic_predicate.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    ANALYZE_TARGET_ANY = 0,
    ANALYZE_TARGET_TABLE,
    ANALYZE_TARGET_INDEX
} AnalyzeTargetKind;

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

static void initialize_result(TinyDBAnalyzeResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_ANALYZE_NOT_APPLICABLE;
}

static TinyDBAnalyzeStatus finish(TinyDBAnalyzeResult* result,
                                  TinyDBAnalyzeStatus status,
                                  const char* message) {
    result->status = status;
    if (message == NULL) {
        result->message[0] = '\0';
    } else {
        snprintf(result->message, sizeof(result->message), "%s", message);
    }
    return status;
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

static GenericSecondaryIndex* find_index(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && ci_equal(index->name, name)) return index;
    }
    return NULL;
}

static int find_column_index(const TableSchema* schema, const char* name) {
    if (schema == NULL || name == NULL) return -1;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (ci_equal(schema->columns[i].name, name)) return (int)i;
    }
    return -1;
}

static bool index_is_eligible(Table* table,
                              const GenericSecondaryIndex* index,
                              TableSchema** schema_out) {
    if (table == NULL || index == NULL || !index->enabled ||
        index->num_columns != 1) {
        return false;
    }

    TableSchema* schema = find_schema(table, index->table_name);
    if (schema == NULL) return false;

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(schema, message, sizeof(message))) {
        return false;
    }

    int column_index = find_column_index(schema, index->column_name);
    if (column_index <= 0) return false;
    if (schema_out != NULL) *schema_out = schema;
    return true;
}

static bool refresh_one(Table* table,
                        GenericSecondaryIndex* index,
                        TinyDBAnalyzeResult* result) {
    TableSchema* schema = NULL;
    if (!index_is_eligible(table, index, &schema)) {
        char message[TINYDB_ANALYZE_MESSAGE_MAX];
        snprintf(message,
                 sizeof(message),
                 "index '%s' does not support persisted generic optimizer statistics",
                 index->name);
        finish(result, TINYDB_ANALYZE_EXECUTE_ERROR, message);
        return false;
    }

    char refresh_message[TINYDB_ANALYZE_MESSAGE_MAX];
    if (!tinydb_generic_index_refresh_statistics(table,
                                                 schema,
                                                 index,
                                                 refresh_message,
                                                 sizeof(refresh_message))) {
        char message[TINYDB_ANALYZE_MESSAGE_MAX];
        snprintf(message,
                 sizeof(message),
                 "ANALYZE failed for index '%s': %.160s",
                 index->name,
                 refresh_message);
        finish(result, TINYDB_ANALYZE_EXECUTE_ERROR, message);
        return false;
    }

    result->refreshed_indexes++;
    return true;
}

static bool refresh_pair(Table* table,
                         const TableSchema* schema,
                         GenericSecondaryIndex* first,
                         GenericSecondaryIndex* second,
                         TinyDBAnalyzeResult* result) {
    char refresh_message[TINYDB_ANALYZE_MESSAGE_MAX];
    if (!tinydb_generic_index_refresh_pair_statistics(
            table,
            schema,
            first,
            second,
            refresh_message,
            sizeof(refresh_message))) {
        char message[TINYDB_ANALYZE_MESSAGE_MAX];
        snprintf(message,
                 sizeof(message),
                 "ANALYZE failed for correlation '%s' + '%s': %.128s",
                 first->name,
                 second->name,
                 refresh_message);
        finish(result, TINYDB_ANALYZE_EXECUTE_ERROR, message);
        return false;
    }
    result->refreshed_correlations++;
    return true;
}

static bool refresh_table_pairs(Table* table,
                                const TableSchema* schema,
                                TinyDBAnalyzeResult* result) {
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* first = &table->sec_indexes[i];
        if (!first->enabled || !ci_equal(first->table_name, schema->name) ||
            !index_is_eligible(table, first, NULL)) {
            continue;
        }
        for (uint32_t j = i + 1u; j < table->num_sec_indexes; j++) {
            GenericSecondaryIndex* second = &table->sec_indexes[j];
            if (!second->enabled || !ci_equal(second->table_name, schema->name) ||
                !index_is_eligible(table, second, NULL)) {
                continue;
            }
            if (!refresh_pair(table, schema, first, second, result)) return false;
        }
    }
    return true;
}

static bool refresh_table_indexes(Table* table,
                                  const TableSchema* schema,
                                  TinyDBAnalyzeResult* result) {
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (!index->enabled || !ci_equal(index->table_name, schema->name)) {
            continue;
        }
        if (!index_is_eligible(table, index, NULL)) continue;
        if (!refresh_one(table, index, result)) return false;
    }
    return refresh_table_pairs(table, schema, result);
}

static bool refresh_all_indexes(Table* table, TinyDBAnalyzeResult* result) {
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (!index_is_eligible(table, index, NULL)) continue;
        if (!refresh_one(table, index, result)) return false;
    }
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (!refresh_table_pairs(table, &table->catalog.schemas[i], result)) {
            return false;
        }
    }
    return true;
}

static TinyDBAnalyzeStatus finish_success(TinyDBAnalyzeResult* result) {
    char message[TINYDB_ANALYZE_MESSAGE_MAX];
    snprintf(message,
             sizeof(message),
             "ANALYZE refreshed %u generic secondary index statistic%s and %u pairwise correlation synopsis%s",
             result->refreshed_indexes,
             result->refreshed_indexes == 1u ? "" : "s",
             result->refreshed_correlations,
             result->refreshed_correlations == 1u ? "" : "es");
    return finish(result, TINYDB_ANALYZE_SUCCESS, message);
}

TinyDBAnalyzeStatus tinydb_analyze_try_execute(
    Table* table,
    const char* sql,
    TinyDBAnalyzeResult* result) {
    TinyDBAnalyzeResult local_result;
    TinyDBAnalyzeResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    if (sql == NULL) return output->status;

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    if (!tinydb_generic_consume_word(&parser, "analyze")) {
        return output->status;
    }

    AnalyzeTargetKind target_kind = ANALYZE_TARGET_ANY;
    char target[MAX_NAME_SIZE];
    target[0] = '\0';

    TinyDBGenericParser after_analyze = parser;
    if (tinydb_generic_consume_end(&after_analyze)) {
        parser = after_analyze;
    } else {
        TinyDBGenericParser target_parser = parser;
        if (tinydb_generic_consume_word(&target_parser, "table")) {
            target_kind = ANALYZE_TARGET_TABLE;
            if (!tinydb_generic_parse_identifier(&target_parser,
                                                 target,
                                                 sizeof(target))) {
                return finish(output,
                              TINYDB_ANALYZE_SYNTAX_ERROR,
                              "ANALYZE TABLE requires a table name");
            }
            parser = target_parser;
        } else {
            target_parser = parser;
            if (tinydb_generic_consume_word(&target_parser, "index")) {
                target_kind = ANALYZE_TARGET_INDEX;
                if (!tinydb_generic_parse_identifier(&target_parser,
                                                     target,
                                                     sizeof(target))) {
                    return finish(output,
                                  TINYDB_ANALYZE_SYNTAX_ERROR,
                                  "ANALYZE INDEX requires an index name");
                }
                parser = target_parser;
            } else if (!tinydb_generic_parse_identifier(&parser,
                                                        target,
                                                        sizeof(target))) {
                return finish(output,
                              TINYDB_ANALYZE_SYNTAX_ERROR,
                              "ANALYZE expects an optional table or index name");
            }
        }

        if (!tinydb_generic_consume_end(&parser)) {
            return finish(output,
                          TINYDB_ANALYZE_SYNTAX_ERROR,
                          "ANALYZE has trailing or unsupported syntax");
        }
    }

    if (table == NULL) {
        return finish(output,
                      TINYDB_ANALYZE_POLICY_ERROR,
                      "ANALYZE requires an open database table");
    }
    if (table->in_transaction) {
        return finish(output,
                      TINYDB_ANALYZE_POLICY_ERROR,
                      "ANALYZE is not allowed inside a transaction because optimizer statistics are non-transactional sidecars");
    }

    if (target[0] == '\0') {
        if (!refresh_all_indexes(table, output)) return output->status;
        return finish_success(output);
    }

    if (target_kind == ANALYZE_TARGET_INDEX) {
        GenericSecondaryIndex* index = find_index(table, target);
        if (index == NULL) {
            char message[TINYDB_ANALYZE_MESSAGE_MAX];
            snprintf(message,
                     sizeof(message),
                     "ANALYZE index '%s' was not found",
                     target);
            return finish(output, TINYDB_ANALYZE_EXECUTE_ERROR, message);
        }
        if (!refresh_one(table, index, output)) return output->status;
        return finish_success(output);
    }

    if (target_kind == ANALYZE_TARGET_TABLE) {
        TableSchema* schema = find_schema(table, target);
        if (schema == NULL) {
            char message[TINYDB_ANALYZE_MESSAGE_MAX];
            snprintf(message,
                     sizeof(message),
                     "ANALYZE table '%s' was not found",
                     target);
            return finish(output, TINYDB_ANALYZE_EXECUTE_ERROR, message);
        }
        if (!refresh_table_indexes(table, schema, output)) return output->status;
        return finish_success(output);
    }

    GenericSecondaryIndex* index = find_index(table, target);
    if (index != NULL) {
        if (!refresh_one(table, index, output)) return output->status;
        return finish_success(output);
    }

    TableSchema* schema = find_schema(table, target);
    if (schema != NULL) {
        if (!refresh_table_indexes(table, schema, output)) return output->status;
        return finish_success(output);
    }

    char message[TINYDB_ANALYZE_MESSAGE_MAX];
    snprintf(message,
             sizeof(message),
             "ANALYZE target '%s' was not found",
             target);
    return finish(output, TINYDB_ANALYZE_EXECUTE_ERROR, message);
}
