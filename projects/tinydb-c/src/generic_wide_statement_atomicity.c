#include "generic_boolean.h"
#include "generic_index_candidates.h"
#include "generic_sql.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define WIDE_STATEMENT_SAVEPOINT_PREFIX "__tinydb_wide_stmt_"

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_wide_grouped_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

typedef enum {
    WIDE_STATEMENT_NONE = 0,
    WIDE_STATEMENT_UPDATE,
    WIDE_STATEMENT_DELETE
} WideStatementMutationKind;

typedef enum {
    WIDE_INDEX_PROJECTION_STAR = 0,
    WIDE_INDEX_PROJECTION_COUNT,
    WIDE_INDEX_PROJECTION_COLUMN
} WideIndexProjectionKind;

typedef struct {
    TableSchema* schema;
    GenericSecondaryIndex* index;
    TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t predicate_count;
    uint32_t indexed_column;
    WideIndexProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} WideIndexSelect;

static int wide_statement_ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool wide_statement_ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (wide_statement_ci_char(*left) != wide_statement_ci_char(*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool wide_statement_legacy_shape(const TableSchema* schema) {
    return schema != NULL && schema->num_columns == 3u &&
           wide_statement_ci_equal(schema->columns[0].name, "id") &&
           wide_statement_ci_equal(schema->columns[1].name, "username") &&
           wide_statement_ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static TableSchema* wide_statement_find_schema(Table* table,
                                               const char* table_name) {
    if (table == NULL || table_name == NULL) return NULL;
    for (uint32_t i = 0u; i < table->catalog.num_tables; i++) {
        if (wide_statement_ci_equal(table->catalog.schemas[i].name,
                                    table_name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool wide_statement_schema_owned(const TableSchema* schema) {
    return schema != NULL && schema->row_size > ROW_SIZE &&
           !wide_statement_legacy_shape(schema);
}

static GenericSecondaryIndex* wide_statement_find_index(
    Table* table,
    const TableSchema* schema,
    uint32_t column_index) {
    if (table == NULL || schema == NULL || column_index == 0u ||
        column_index >= schema->num_columns) {
        return NULL;
    }
    for (uint32_t i = 0u; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && index->num_columns == 1u &&
            wide_statement_ci_equal(index->table_name, schema->name) &&
            wide_statement_ci_equal(index->column_name,
                                    schema->columns[column_index].name)) {
            return index;
        }
    }
    return NULL;
}

static WideStatementMutationKind wide_statement_target(
    Table* table,
    const char* sql,
    TableSchema** out_schema) {
    if (out_schema != NULL) *out_schema = NULL;
    if (table == NULL || sql == NULL) return WIDE_STATEMENT_NONE;

    TinyDBGenericParser parser;
    tinydb_generic_parser_init(&parser, sql);
    WideStatementMutationKind kind = WIDE_STATEMENT_NONE;
    if (tinydb_generic_consume_word(&parser, "update")) {
        kind = WIDE_STATEMENT_UPDATE;
    } else if (tinydb_generic_consume_word(&parser, "delete") &&
               tinydb_generic_consume_word(&parser, "from")) {
        kind = WIDE_STATEMENT_DELETE;
    } else {
        return WIDE_STATEMENT_NONE;
    }

    char table_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name))) {
        return WIDE_STATEMENT_NONE;
    }
    TableSchema* schema = wide_statement_find_schema(table, table_name);
    if (!wide_statement_schema_owned(schema)) return WIDE_STATEMENT_NONE;
    if (out_schema != NULL) *out_schema = schema;
    return kind;
}

static TinyDBGenericSqlStatus wide_statement_atomicity_error(
    TinyDBGenericSqlResult* result,
    WideStatementMutationKind kind,
    const char* message) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = kind == WIDE_STATEMENT_DELETE
        ? STATEMENT_DELETE
        : STATEMENT_UPDATE;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = false;
    snprintf(result->message,
             sizeof(result->message),
             "%s",
             message != NULL ? message
                             : "unable to preserve wide statement atomicity");
    return result->status;
}

static TinyDBGenericSqlStatus wide_index_error(TinyDBGenericSqlResult* result,
                                                const char* message) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_EXECUTE_ERROR;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_KEY_NOT_FOUND;
    result->executed = false;
    snprintf(result->message,
             sizeof(result->message),
             "%s",
             message != NULL ? message : "wide index candidate execution failed");
    return result->status;
}

static TinyDBGenericSqlStatus wide_index_success(TinyDBGenericSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_GENERIC_SQL_SUCCESS;
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;
    result->executed = true;
    return result->status;
}

static bool wide_statement_savepoint_name(Pager* pager,
                                          char* name,
                                          size_t name_size) {
    if (pager == NULL || name == NULL || name_size == 0u) return false;
    for (uint32_t attempt = 0u; attempt <= MAX_SAVEPOINTS; attempt++) {
        snprintf(name,
                 name_size,
                 WIDE_STATEMENT_SAVEPOINT_PREFIX "%u_%u",
                 pager->savepoint_count,
                 attempt);
        bool duplicate = false;
        for (uint32_t i = 0u; i < pager->savepoint_count; i++) {
            if (strcmp(pager->savepoints[i].name, name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) return true;
    }
    return false;
}

static bool wide_index_parse_projection(TinyDBGenericParser* parser,
                                        const TableSchema* schema,
                                        WideIndexSelect* select) {
    if (tinydb_generic_consume_char(parser, '*')) {
        select->projection_kind = WIDE_INDEX_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        select->projection_kind = WIDE_INDEX_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    char column_name[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(parser,
                                         column_name,
                                         sizeof(column_name))) {
        return false;
    }
    int column_index = tinydb_generic_find_column_index(schema, column_name);
    if (column_index < 0) return false;
    select->projection_kind = WIDE_INDEX_PROJECTION_COLUMN;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool wide_index_parse_select(Table* table,
                                    const char* sql,
                                    WideIndexSelect* select) {
    memset(select, 0, sizeof(*select));
    TinyDBGenericParser routing;
    tinydb_generic_parser_init(&routing, sql);
    if (!tinydb_generic_consume_word(&routing, "select")) return false;

    TinyDBGenericParser after_select = routing;
    if (tinydb_generic_consume_char(&routing, '*')) {
        /* projection consumed only to reach the table name */
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
    TableSchema* schema = wide_statement_find_schema(table, table_name);
    if (!wide_statement_schema_owned(schema)) return false;
    select->schema = schema;

    TinyDBGenericParser parser = after_select;
    if (!wide_index_parse_projection(&parser, schema, select) ||
        !tinydb_generic_consume_word(&parser, "from") ||
        !tinydb_generic_parse_identifier(&parser,
                                         table_name,
                                         sizeof(table_name)) ||
        !wide_statement_ci_equal(table_name, schema->name) ||
        !tinydb_generic_consume_word(&parser, "where") ||
        !tinydb_generic_parse_predicate(&parser,
                                        schema,
                                        &select->predicates[0])) {
        return false;
    }
    select->predicate_count = 1u;
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

    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->limit)) return false;
        select->has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset") &&
            !tinydb_generic_parse_uint32(&parser, &select->offset)) {
            return false;
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
    }
    if (!tinydb_generic_consume_end(&parser)) return false;

    for (uint32_t i = 0u; i < select->predicate_count; i++) {
        TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->column_index == 0u ||
            predicate->op == TINYDB_GENERIC_COMPARE_LIKE) {
            continue;
        }
        GenericSecondaryIndex* index = wide_statement_find_index(
            table, schema, predicate->column_index);
        if (index != NULL) {
            select->index = index;
            select->indexed_column = predicate->column_index;
            return true;
        }
    }
    return false;
}

static void wide_index_print_value(const TinyDBValue* value) {
    if (value->type == COL_TYPE_INT) {
        printf("%u\n", value->int_value);
    } else {
        printf("%s\n", value->text);
    }
}

static void wide_index_print_row(const TinyDBValue* values,
                                 uint32_t value_count) {
    printf("(");
    for (uint32_t i = 0u; i < value_count; i++) {
        if (i > 0u) printf(", ");
        if (values[i].type == COL_TYPE_INT) {
            printf("%u", values[i].int_value);
        } else {
            printf("%s", values[i].text);
        }
    }
    printf(")\n");
}

static bool wide_index_predicates_match(const WideIndexSelect* select,
                                        const TinyDBValue* values) {
    for (uint32_t i = 0u; i < select->predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (!tinydb_generic_predicate_matches(
                predicate, &values[predicate->column_index])) {
            return false;
        }
    }
    return true;
}

static bool wide_index_collect_candidates(
    Table* table,
    const WideIndexSelect* select,
    TinyDBGenericIndexCandidates* candidates,
    char* message,
    size_t message_size) {
    TinyDBGenericPredicate indexed[MAX_COLUMNS_PER_TABLE];
    uint32_t indexed_count = 0u;
    for (uint32_t i = 0u; i < select->predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->column_index == select->indexed_column &&
            predicate->op != TINYDB_GENERIC_COMPARE_LIKE) {
            indexed[indexed_count++] = *predicate;
        }
    }
    if (indexed_count == 0u) return false;
    if (indexed_count == 1u) {
        return tinydb_generic_index_collect_candidates(table,
                                                       select->schema,
                                                       select->index,
                                                       &indexed[0],
                                                       candidates,
                                                       message,
                                                       message_size);
    }
    return tinydb_generic_index_collect_conjunctive_candidates(
        table,
        select->schema,
        select->index,
        indexed,
        indexed_count,
        candidates,
        message,
        message_size);
}

static TinyDBGenericSqlStatus wide_index_execute_select(
    Table* table,
    const WideIndexSelect* select,
    TinyDBGenericSqlResult* result) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    message[0] = '\0';
    if (!tinydb_record_payload_schema_supported(select->schema,
                                                message,
                                                sizeof(message))) {
        return wide_index_error(result, message);
    }

    TinyDBGenericIndexCandidates candidates;
    memset(&candidates, 0, sizeof(candidates));
    if (!wide_index_collect_candidates(table,
                                       select,
                                       &candidates,
                                       message,
                                       sizeof(message))) {
        return wide_index_error(
            result,
            message[0] != '\0' ? message : "unable to collect wide index candidates");
    }

    uint32_t matched = 0u;
    uint32_t emitted = 0u;
    for (uint32_t i = 0u; i < candidates.count; i++) {
        TinyDBRecordPayload payload;
        message[0] = '\0';
        if (!tinydb_record_payload_find(table,
                                        select->schema,
                                        candidates.ids[i],
                                        &payload,
                                        message,
                                        sizeof(message))) {
            /* Secondary-index entries are candidate hints. A missing row is a
             * stale false positive, not permission to fabricate a result. */
            continue;
        }

        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        uint32_t value_count = 0u;
        if (!tinydb_record_payload_decode_values(select->schema,
                                                 &payload,
                                                 values,
                                                 MAX_COLUMNS_PER_TABLE,
                                                 &value_count,
                                                 message,
                                                 sizeof(message)) ||
            value_count != select->schema->num_columns) {
            tinydb_generic_index_candidates_free(&candidates);
            return wide_index_error(
                result,
                message[0] != '\0'
                    ? message
                    : "unable to decode schema-sized index candidate");
        }
        if (!wide_index_predicates_match(select, values)) continue;

        matched++;
        if (select->projection_kind == WIDE_INDEX_PROJECTION_COUNT ||
            matched <= select->offset) {
            continue;
        }
        if (select->has_limit && emitted >= select->limit) break;

        if (select->projection_kind == WIDE_INDEX_PROJECTION_COLUMN) {
            wide_index_print_value(&values[select->projection_column_index]);
        } else {
            wide_index_print_row(values, value_count);
        }
        emitted++;
    }

    tinydb_generic_index_candidates_free(&candidates);
    if (select->projection_kind == WIDE_INDEX_PROJECTION_COUNT) {
        uint32_t count = matched;
        if (select->offset > 0u) count = 0u;
        if (select->has_limit && select->limit == 0u) count = 0u;
        printf("%u\n", count);
    }
    return wide_index_success(result);
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;

    if (table != NULL && sql != NULL) {
        WideIndexSelect select;
        if (wide_index_parse_select(table, sql, &select)) {
            return wide_index_execute_select(table, &select, output);
        }
    }

    TableSchema* schema = NULL;
    WideStatementMutationKind kind = wide_statement_target(table, sql, &schema);
    (void)schema;
    if (kind == WIDE_STATEMENT_NONE || table == NULL || table->pager == NULL ||
        !table->in_transaction) {
        return tinydb_generic_sql_try_execute_wide_grouped_base(table,
                                                                sql,
                                                                output);
    }

    if (!table->pager->in_transaction) {
        return wide_statement_atomicity_error(
            output,
            kind,
            "wide statement transaction state is inconsistent");
    }

    char savepoint_name[64];
    if (!wide_statement_savepoint_name(table->pager,
                                       savepoint_name,
                                       sizeof(savepoint_name)) ||
        !pager_savepoint(table->pager, savepoint_name)) {
        return wide_statement_atomicity_error(
            output,
            kind,
            "unable to reserve a statement savepoint for wide mutation");
    }

    TinyDBGenericSqlStatus status =
        tinydb_generic_sql_try_execute_wide_grouped_base(table, sql, output);
    if (status == TINYDB_GENERIC_SQL_SUCCESS) {
        if (!pager_release_savepoint(table->pager, savepoint_name)) {
            return wide_statement_atomicity_error(
                output,
                kind,
                "unable to release successful wide statement savepoint");
        }
        return status;
    }

    if (!pager_rollback_to_savepoint(table->pager, savepoint_name)) {
        return wide_statement_atomicity_error(
            output,
            kind,
            "unable to roll back failed wide statement to its savepoint");
    }
    if (!pager_release_savepoint(table->pager, savepoint_name)) {
        return wide_statement_atomicity_error(
            output,
            kind,
            "unable to release failed wide statement savepoint");
    }
    return status;
}
