#include "generic_index_candidates.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OR_UNION_MAX_GROUPS MAX_COLUMNS_PER_TABLE
#define OR_UNION_MAX_TERMS MAX_COLUMNS_PER_TABLE

typedef enum {
    OR_UNION_PROJECTION_STAR = 0,
    OR_UNION_PROJECTION_COUNT,
    OR_UNION_PROJECTION_COLUMN
} OrUnionProjectionKind;

typedef struct {
    TinyDBGenericPredicate terms[OR_UNION_MAX_TERMS];
    uint32_t count;
    bool primary_key_anchor;
    uint32_t anchor_predicate_index;
    GenericSecondaryIndex* index;
} OrUnionGroup;

typedef struct {
    TableSchema* schema;
    OrUnionGroup groups[OR_UNION_MAX_GROUPS];
    uint32_t group_count;
    OrUnionProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
} OrUnionSelect;

typedef struct {
    uint32_t* ids;
    uint32_t count;
    uint32_t capacity;
} OrUnionIds;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_or_scan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_or_scan_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);

void tinydb_generic_sql_print_plan_or_scan_base(
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
                             OrUnionSelect* select) {
    if (tinydb_generic_consume_char(parser, '*')) {
        select->projection_kind = OR_UNION_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        select->projection_kind = OR_UNION_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = tinydb_generic_find_column_index(schema, column);
    if (column_index < 0) return false;
    select->projection_kind = OR_UNION_PROJECTION_COLUMN;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool choose_group_anchor(Table* table,
                                TableSchema* schema,
                                OrUnionGroup* group) {
    for (uint32_t i = 0; i < group->count; i++) {
        TinyDBGenericPredicate* predicate = &group->terms[i];
        if (predicate->column_index == 0 &&
            predicate->op == TINYDB_GENERIC_COMPARE_EQ) {
            group->primary_key_anchor = true;
            group->anchor_predicate_index = i;
            return true;
        }
    }

    uint32_t best_score = 0;
    GenericSecondaryIndex* best_index = NULL;
    uint32_t best_predicate = 0;
    for (uint32_t i = 0; i < group->count; i++) {
        TinyDBGenericPredicate* predicate = &group->terms[i];
        GenericSecondaryIndex* index = find_single_column_index(
            table, schema, predicate->column_index);
        if (index == NULL) continue;
        uint32_t score = predicate->op == TINYDB_GENERIC_COMPARE_EQ ? 2u : 1u;
        if (score > best_score) {
            best_score = score;
            best_index = index;
            best_predicate = i;
        }
    }
    if (best_index == NULL) return false;
    group->index = best_index;
    group->anchor_predicate_index = best_predicate;
    return true;
}

static bool parse_union_select(Table* table,
                               const char* sql,
                               OrUnionSelect* select) {
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
        !tinydb_generic_consume_word(&parser, "where")) {
        return false;
    }

    select->group_count = 1;
    bool saw_or = false;
    for (;;) {
        OrUnionGroup* group = &select->groups[select->group_count - 1u];
        if (group->count >= OR_UNION_MAX_TERMS ||
            !tinydb_generic_parse_predicate(&parser,
                                            schema,
                                            &group->terms[group->count])) {
            return false;
        }
        group->count++;

        if (tinydb_generic_consume_word(&parser, "and")) continue;
        if (tinydb_generic_consume_word(&parser, "or")) {
            saw_or = true;
            if (select->group_count >= OR_UNION_MAX_GROUPS) return false;
            select->group_count++;
            continue;
        }
        break;
    }
    if (!saw_or) return false;

    if (tinydb_generic_consume_word(&parser, "limit")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->limit)) return false;
        select->has_limit = true;
        if (tinydb_generic_consume_word(&parser, "offset")) {
            if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
        }
    } else if (tinydb_generic_consume_word(&parser, "offset")) {
        if (!tinydb_generic_parse_uint32(&parser, &select->offset)) return false;
    }
    if (!tinydb_generic_consume_end(&parser)) return false;

    for (uint32_t i = 0; i < select->group_count; i++) {
        if (!choose_group_anchor(table, schema, &select->groups[i])) return false;
    }
    return true;
}

static bool group_matches(const OrUnionGroup* group,
                          const TinyDBValue* values) {
    for (uint32_t i = 0; i < group->count; i++) {
        const TinyDBGenericPredicate* predicate = &group->terms[i];
        if (!tinydb_generic_predicate_matches(
                predicate, &values[predicate->column_index])) {
            return false;
        }
    }
    return true;
}

static bool expression_matches(const OrUnionSelect* select,
                               const TinyDBValue* values) {
    for (uint32_t i = 0; i < select->group_count; i++) {
        if (group_matches(&select->groups[i], values)) return true;
    }
    return false;
}

static bool append_id(OrUnionIds* ids, uint32_t id) {
    if (ids->count == ids->capacity) {
        uint32_t capacity = ids->capacity == 0 ? 32u : ids->capacity;
        if (capacity > UINT32_MAX / 2u) return false;
        capacity *= 2u;
        uint32_t* grown = (uint32_t*)realloc(
            ids->ids, (size_t)capacity * sizeof(uint32_t));
        if (grown == NULL) return false;
        ids->ids = grown;
        ids->capacity = capacity;
    }
    ids->ids[ids->count++] = id;
    return true;
}

static int compare_ids(const void* left, const void* right) {
    uint32_t a = *(const uint32_t*)left;
    uint32_t b = *(const uint32_t*)right;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static void sort_and_deduplicate(OrUnionIds* ids) {
    if (ids->count < 2) return;
    qsort(ids->ids, ids->count, sizeof(uint32_t), compare_ids);
    uint32_t write_index = 1;
    for (uint32_t i = 1; i < ids->count; i++) {
        if (ids->ids[i] != ids->ids[write_index - 1u]) {
            ids->ids[write_index++] = ids->ids[i];
        }
    }
    ids->count = write_index;
}

static bool collect_union_candidates(Table* table,
                                     const OrUnionSelect* select,
                                     OrUnionIds* ids,
                                     char* message,
                                     size_t message_size) {
    memset(ids, 0, sizeof(*ids));
    for (uint32_t i = 0; i < select->group_count; i++) {
        const OrUnionGroup* group = &select->groups[i];
        const TinyDBGenericPredicate* anchor =
            &group->terms[group->anchor_predicate_index];
        if (group->primary_key_anchor) {
            if (!append_id(ids, anchor->value.int_value)) goto failed;
            continue;
        }

        TinyDBGenericIndexCandidates candidates;
        if (!tinydb_generic_index_collect_candidates(table,
                                                     select->schema,
                                                     group->index,
                                                     anchor,
                                                     &candidates,
                                                     message,
                                                     message_size)) {
            goto failed;
        }
        for (uint32_t j = 0; j < candidates.count; j++) {
            if (!append_id(ids, candidates.ids[j])) {
                tinydb_generic_index_candidates_free(&candidates);
                goto failed;
            }
        }
        tinydb_generic_index_candidates_free(&candidates);
    }
    sort_and_deduplicate(ids);
    return true;

failed:
    free(ids->ids);
    ids->ids = NULL;
    ids->count = 0;
    ids->capacity = 0;
    if (message != NULL && message_size > 0 && message[0] == '\0') {
        snprintf(message, message_size, "%s", "unable to collect OR index candidates");
    }
    return false;
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

static TinyDBGenericSqlStatus execute_union_select(
    Table* table,
    const OrUnionSelect* select,
    TinyDBGenericSqlResult* result) {
    OrUnionIds ids;
    char message[TINYDB_GENERIC_SQL_MESSAGE_MAX] = {0};
    if (!collect_union_candidates(table,
                                  select,
                                  &ids,
                                  message,
                                  sizeof(message))) {
        return execute_error(result,
                             message[0] != '\0'
                                 ? message
                                 : "unable to collect OR index candidates");
    }

    uint32_t matched = 0;
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < ids.count; i++) {
        TinyDBRecord record;
        if (!tinydb_record_find(table, select->schema, ids.ids[i], &record)) continue;
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        if (!decode_values(select->schema, &record, values)) {
            free(ids.ids);
            return execute_error(result, "unable to decode OR index candidate");
        }
        if (!expression_matches(select, values)) continue;

        matched++;
        if (select->projection_kind == OR_UNION_PROJECTION_COUNT) continue;
        if (matched <= select->offset) continue;
        if (select->has_limit && emitted >= select->limit) break;

        if (select->projection_kind == OR_UNION_PROJECTION_COLUMN) {
            print_value(&values[select->projection_column_index]);
        } else {
            tinydb_record_print(select->schema, &record);
        }
        emitted++;
    }

    if (select->projection_kind == OR_UNION_PROJECTION_COUNT) {
        uint32_t count = matched;
        if (select->offset > 0) count = 0;
        if (select->has_limit && select->limit == 0) count = 0;
        printf("%u\n", count);
    }
    free(ids.ids);
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

static bool build_expression_text(const OrUnionSelect* select,
                                  char* output,
                                  size_t output_size) {
    size_t length = 0;
    output[0] = '\0';
    for (uint32_t i = 0; i < select->group_count; i++) {
        if (i > 0 && !append_text(output, output_size, &length, " OR ")) {
            return false;
        }
        const OrUnionGroup* group = &select->groups[i];
        for (uint32_t j = 0; j < group->count; j++) {
            if (j > 0 && !append_text(output, output_size, &length, " AND ")) {
                return false;
            }
            if (!append_predicate(output,
                                  output_size,
                                  &length,
                                  select->schema,
                                  &group->terms[j])) {
                return false;
            }
        }
    }
    return true;
}

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_or_select_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    OrUnionSelect select;
    if (parse_union_select(table, sql, &select)) {
        return execute_union_select(table, &select, output);
    }
    return tinydb_generic_sql_try_execute_or_scan_base(table, sql, output);
}

TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_parenthesized_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (plan != NULL) memset(plan, 0, sizeof(*plan));
    if (table == NULL || sql == NULL || plan == NULL) return output->status;

    OrUnionSelect select;
    if (!parse_union_select(table, sql, &select)) {
        return tinydb_generic_sql_build_select_plan_or_scan_base(
            table, sql, plan, output);
    }

    plan->applicable = true;
    plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_UNION;
    plan->root_page_num = select.schema->root_page_num;
    plan->has_filter = true;
    plan->index_branch_count = select.group_count;
    snprintf(plan->table_name, sizeof(plan->table_name), "%s", select.schema->name);
    snprintf(plan->index_name, sizeof(plan->index_name), "%s", "multiple");
    if (select.projection_kind == OR_UNION_PROJECTION_STAR) {
        snprintf(plan->projection, sizeof(plan->projection), "*");
    } else if (select.projection_kind == OR_UNION_PROJECTION_COUNT) {
        snprintf(plan->projection, sizeof(plan->projection), "COUNT(*)");
    } else {
        snprintf(plan->projection,
                 sizeof(plan->projection),
                 "%s",
                 select.schema->columns[select.projection_column_index].name);
    }
    if (!build_expression_text(&select,
                               plan->filter_expression,
                               sizeof(plan->filter_expression))) {
        return execute_error(output, "OR index-union plan text exceeds capacity");
    }

    output->status = TINYDB_GENERIC_SQL_SUCCESS;
    output->statement_type = STATEMENT_SELECT;
    output->statement_type_valid = true;
    output->execute_result = EXECUTE_SUCCESS;
    return output->status;
}

void tinydb_generic_sql_print_plan_parenthesized_base(
    const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable ||
        plan->kind != TINYDB_GENERIC_PLAN_SECONDARY_INDEX_UNION) {
        tinydb_generic_sql_print_plan_or_scan_base(plan);
        return;
    }

    printf("PLAN: GENERIC INDEX UNION\n");
    printf("  TABLE: %s (root page %u)\n",
           plan->table_name,
           plan->root_page_num);
    printf("  BRANCHES: %u\n", plan->index_branch_count);
    printf("  PROJECTION: %s\n", plan->projection);
    printf("  FILTER: %s\n", plan->filter_expression);
}
