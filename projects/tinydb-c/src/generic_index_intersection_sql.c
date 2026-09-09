#include "generic_index_candidates.h"
#include "generic_index_cost.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTERSECTION_MAX_SOURCES MAX_COLUMNS_PER_TABLE

typedef enum {
    INTERSECTION_PROJECTION_STAR = 0,
    INTERSECTION_PROJECTION_COUNT,
    INTERSECTION_PROJECTION_COLUMN
} IntersectionProjectionKind;

typedef struct {
    uint32_t predicate_index;
    uint32_t column_index;
    GenericSecondaryIndex* index;
    uint32_t estimated_count;
} IntersectionSource;

typedef struct {
    TableSchema* schema;
    TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t predicate_count;
    IntersectionSource sources[INTERSECTION_MAX_SOURCES];
    uint32_t source_count;
    IntersectionProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
    bool cost_estimated;
    uint32_t estimated_rows;
    uint32_t estimated_table_rows;
    uint64_t estimated_cost;
    uint64_t estimated_anchor_cost;
    uint64_t estimated_scan_cost;
} IntersectionSelect;

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute_single_anchor_base(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result);
TinyDBGenericSqlStatus tinydb_generic_sql_build_select_plan_single_anchor_base(
    Table* table,
    const char* sql,
    TinyDBGenericSelectPlan* plan,
    TinyDBGenericSqlResult* result);
void tinydb_generic_sql_print_plan_single_anchor_base(
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
                             IntersectionSelect* select) {
    if (tinydb_generic_consume_char(parser, '*')) {
        select->projection_kind = INTERSECTION_PROJECTION_STAR;
        return true;
    }

    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        select->projection_kind = INTERSECTION_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = tinydb_generic_find_column_index(schema, column);
    if (column_index < 0) return false;
    select->projection_kind = INTERSECTION_PROJECTION_COLUMN;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool has_primary_key_equality(const IntersectionSelect* select) {
    for (uint32_t i = 0; i < select->predicate_count; i++) {
        if (select->predicates[i].column_index == 0 &&
            select->predicates[i].op == TINYDB_GENERIC_COMPARE_EQ) {
            return true;
        }
    }
    return false;
}

static int source_for_index(const IntersectionSelect* select,
                            const GenericSecondaryIndex* index) {
    for (uint32_t i = 0; i < select->source_count; i++) {
        if (select->sources[i].index == index) return (int)i;
    }
    return -1;
}

static bool choose_sources(Table* table, IntersectionSelect* select) {
    select->source_count = 0;
    for (uint32_t i = 0; i < select->predicate_count; i++) {
        TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->op == TINYDB_GENERIC_COMPARE_LIKE ||
            predicate->column_index == 0) {
            continue;
        }

        GenericSecondaryIndex* index = find_single_column_index(
            table, select->schema, predicate->column_index);
        if (index == NULL) continue;

        int existing = source_for_index(select, index);
        if (existing >= 0) {
            TinyDBGenericPredicate* current =
                &select->predicates[select->sources[(uint32_t)existing].predicate_index];
            if (predicate->op == TINYDB_GENERIC_COMPARE_EQ &&
                current->op != TINYDB_GENERIC_COMPARE_EQ) {
                select->sources[(uint32_t)existing].predicate_index = i;
            }
            continue;
        }

        if (select->source_count >= INTERSECTION_MAX_SOURCES) return false;
        select->sources[select->source_count].predicate_index = i;
        select->sources[select->source_count].column_index = predicate->column_index;
        select->sources[select->source_count].index = index;
        select->source_count++;
    }
    return select->source_count >= 2;
}

static uint32_t collect_source_predicates(
    const IntersectionSelect* select,
    const IntersectionSource* source,
    TinyDBGenericPredicate* predicates,
    uint32_t capacity) {
    if (select == NULL || source == NULL || predicates == NULL || capacity == 0) {
        return 0;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < select->predicate_count && count < capacity; i++) {
        const TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->column_index == source->column_index &&
            predicate->op <= TINYDB_GENERIC_COMPARE_LTE) {
            predicates[count++] = *predicate;
        }
    }
    return count;
}

static uint32_t ordered_source_term_count(const IntersectionSelect* select) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < select->source_count; i++) {
        TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
        total += collect_source_predicates(select,
                                           &select->sources[i],
                                           predicates,
                                           MAX_COLUMNS_PER_TABLE);
    }
    return total;
}

static uint32_t fused_source_count(const IntersectionSelect* select) {
    uint32_t fused = 0;
    for (uint32_t i = 0; i < select->source_count; i++) {
        TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
        uint32_t count = collect_source_predicates(select,
                                                   &select->sources[i],
                                                   predicates,
                                                   MAX_COLUMNS_PER_TABLE);
        if (count >= 2) fused++;
    }
    return fused;
}

static void sort_sources_by_estimate(IntersectionSelect* select) {
    for (uint32_t i = 0; i < select->source_count; i++) {
        uint32_t smallest = i;
        for (uint32_t j = i + 1u; j < select->source_count; j++) {
            if (select->sources[j].estimated_count <
                select->sources[smallest].estimated_count) {
                smallest = j;
            }
        }
        if (smallest != i) {
            IntersectionSource swap = select->sources[i];
            select->sources[i] = select->sources[smallest];
            select->sources[smallest] = swap;
        }
    }
}

static bool apply_cost_model(Table* table, IntersectionSelect* select) {
    uint32_t source_rows[INTERSECTION_MAX_SOURCES];
    uint32_t table_rows = 0;

    for (uint32_t i = 0; i < select->source_count; i++) {
        TinyDBGenericPredicate source_predicates[MAX_COLUMNS_PER_TABLE];
        uint32_t source_predicate_count = collect_source_predicates(
            select,
            &select->sources[i],
            source_predicates,
            MAX_COLUMNS_PER_TABLE);
        if (source_predicate_count == 0) return true;

        TinyDBGenericIndexEstimate estimate;
        char message[TINYDB_GENERIC_SQL_MESSAGE_MAX] = {0};
        bool estimated = source_predicate_count >= 2
            ? tinydb_generic_index_estimate_conjunctive_candidates(
                  table,
                  select->schema,
                  select->sources[i].index,
                  source_predicates,
                  source_predicate_count,
                  &estimate,
                  message,
                  sizeof(message))
            : tinydb_generic_index_estimate_candidates(
                  table,
                  select->schema,
                  select->sources[i].index,
                  &source_predicates[0],
                  &estimate,
                  message,
                  sizeof(message));
        if (!estimated) {
            return true;
        }
        select->sources[i].estimated_count = estimate.candidate_count;
        source_rows[i] = estimate.candidate_count;
        if (i == 0 || estimate.total_count > table_rows) {
            table_rows = estimate.total_count;
        }

        if (estimate.candidate_count == 0) {
            if (i != 0) {
                IntersectionSource empty_source = select->sources[i];
                for (uint32_t j = i; j > 0; j--) {
                    select->sources[j] = select->sources[j - 1u];
                }
                select->sources[0] = empty_source;
            }
            select->cost_estimated = true;
            select->estimated_rows = 0;
            select->estimated_table_rows = estimate.total_count;
            select->estimated_cost = 0;
            select->estimated_anchor_cost = 0;
            select->estimated_scan_cost = tinydb_generic_scan_cost(estimate.total_count);
            return true;
        }
    }

    sort_sources_by_estimate(select);
    for (uint32_t i = 0; i < select->source_count; i++) {
        source_rows[i] = select->sources[i].estimated_count;
    }

    select->cost_estimated = true;
    select->estimated_table_rows = table_rows;
    select->estimated_scan_cost = tinydb_generic_scan_cost(table_rows);
    select->estimated_anchor_cost =
        tinydb_generic_anchor_cost(select->sources[0].estimated_count);
    select->estimated_cost = tinydb_generic_intersection_cost(
        source_rows,
        select->source_count,
        table_rows,
        &select->estimated_rows);

    return select->estimated_cost <= select->estimated_anchor_cost &&
           select->estimated_cost <= select->estimated_scan_cost;
}

static bool parse_intersection_select(Table* table,
                                      const char* sql,
                                      IntersectionSelect* select) {
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
        !tinydb_generic_parse_predicate(&parser,
                                        schema,
                                        &select->predicates[0])) {
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
        !choose_sources(table, select) ||
        !apply_cost_model(table, select)) {
        return false;
    }
    return true;
}

static int compare_ids(const void* left, const void* right) {
    uint32_t a = *(const uint32_t*)left;
    uint32_t b = *(const uint32_t*)right;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static void sort_ids(TinyDBGenericIndexCandidates* candidates) {
    if (candidates->count > 1) {
        qsort(candidates->ids,
              candidates->count,
              sizeof(uint32_t),
              compare_ids);
    }
}

static void intersect_ids(TinyDBGenericIndexCandidates* left,
                          const TinyDBGenericIndexCandidates* right) {
    uint32_t left_index = 0;
    uint32_t right_index = 0;
    uint32_t write_index = 0;

    while (left_index < left->count && right_index < right->count) {
        uint32_t left_id = left->ids[left_index];
        uint32_t right_id = right->ids[right_index];
        if (left_id < right_id) {
            left_index++;
        } else if (left_id > right_id) {
            right_index++;
        } else {
            left->ids[write_index++] = left_id;
            left_index++;
            right_index++;
        }
    }
    left->count = write_index;
}

static void free_candidate_sets(TinyDBGenericIndexCandidates* sets,
                                uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        tinydb_generic_index_candidates_free(&sets[i]);
    }
}

static bool collect_intersection(Table* table,
                                 const IntersectionSelect* select,
                                 TinyDBGenericIndexCandidates* intersection,
                                 char* message,
                                 size_t message_size) {
    memset(intersection, 0, sizeof(*intersection));
    TinyDBGenericIndexCandidates sets[INTERSECTION_MAX_SOURCES];
    memset(sets, 0, sizeof(sets));

    uint32_t collected = 0;
    for (uint32_t i = 0; i < select->source_count; i++) {
        const IntersectionSource* source = &select->sources[i];
        TinyDBGenericPredicate source_predicates[MAX_COLUMNS_PER_TABLE];
        uint32_t source_predicate_count = collect_source_predicates(
            select,
            source,
            source_predicates,
            MAX_COLUMNS_PER_TABLE);
        if (source_predicate_count == 0) {
            snprintf(message,
                     message_size,
                     "%s",
                     "generic intersection source has no ordered predicate");
            free_candidate_sets(sets, collected);
            return false;
        }

        bool source_collected = source_predicate_count >= 2
            ? tinydb_generic_index_collect_conjunctive_candidates(
                  table,
                  select->schema,
                  source->index,
                  source_predicates,
                  source_predicate_count,
                  &sets[i],
                  message,
                  message_size)
            : tinydb_generic_index_collect_candidates(
                  table,
                  select->schema,
                  source->index,
                  &source_predicates[0],
                  &sets[i],
                  message,
                  message_size);
        if (!source_collected) {
            free_candidate_sets(sets, collected);
            return false;
        }
        collected++;
        sort_ids(&sets[i]);

        if (sets[i].count == 0) {
            if (i != 0) {
                tinydb_generic_index_candidates_free(intersection);
            }
            *intersection = sets[i];
            memset(&sets[i], 0, sizeof(sets[i]));
            free_candidate_sets(sets, collected);
            return true;
        }

        if (i == 0) {
            *intersection = sets[0];
            memset(&sets[0], 0, sizeof(sets[0]));
            continue;
        }

        intersect_ids(intersection, &sets[i]);
        tinydb_generic_index_candidates_free(&sets[i]);
        memset(&sets[i], 0, sizeof(sets[i]));
        if (intersection->count == 0) {
            free_candidate_sets(sets, collected);
            return true;
        }
    }

    free_candidate_sets(sets, collected);
    return true;
}

static bool predicates_match(const IntersectionSelect* select,
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

static TinyDBGenericSqlStatus execute_intersection_select(
    Table* table,
    const IntersectionSelect* select,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericIndexCandidates intersection;
    char message[TINYDB_GENERIC_SQL_MESSAGE_MAX] = {0};
    if (!collect_intersection(table,
                              select,
                              &intersection,
                              message,
                              sizeof(message))) {
        return execute_error(result,
                             message[0] != '\0'
                                 ? message
                                 : "unable to collect generic index intersection");
    }

    uint32_t matched = 0;
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < intersection.count; i++) {
        TinyDBRecord record;
        if (!tinydb_record_find(table,
                                select->schema,
                                intersection.ids[i],
                                &record)) {
            continue;
        }

        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        if (!decode_values(select->schema, &record, values)) {
            tinydb_generic_index_candidates_free(&intersection);
            return execute_error(result,
                                 "unable to decode generic intersection candidate");
        }
        if (!predicates_match(select, values)) continue;

        matched++;
        if (select->projection_kind == INTERSECTION_PROJECTION_COUNT) continue;
        if (matched <= select->offset) continue;
        if (select->has_limit && emitted >= select->limit) break;

        if (select->projection_kind == INTERSECTION_PROJECTION_COLUMN) {
            print_value(&values[select->projection_column_index]);
        } else {
            tinydb_record_print(select->schema, &record);
        }
        emitted++;
    }

    if (select->projection_kind == INTERSECTION_PROJECTION_COUNT) {
        uint32_t count = matched;
        if (select->offset > 0) count = 0;
        if (select->has_limit && select->limit == 0) count = 0;
        printf("%u\n", count);
    }

    tinydb_generic_index_candidates_free(&intersection);
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

static bool build_filter_expression(const IntersectionSelect* select,
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

TinyDBGenericSqlStatus tinydb_generic_sql_try_execute(
    Table* table,
    const char* sql,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericSqlResult local_result;
    TinyDBGenericSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);
    if (table == NULL || sql == NULL) return output->status;

    IntersectionSelect select;
    if (parse_intersection_select(table, sql, &select)) {
        return execute_intersection_select(table, &select, output);
    }
    return tinydb_generic_sql_try_execute_single_anchor_base(table, sql, output);
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

    IntersectionSelect select;
    if (!parse_intersection_select(table, sql, &select)) {
        return tinydb_generic_sql_build_select_plan_single_anchor_base(
            table, sql, plan, output);
    }

    plan->applicable = true;
    plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_INTERSECTION;
    plan->root_page_num = select.schema->root_page_num;
    plan->has_filter = true;
    plan->index_branch_count = select.source_count;
    plan->index_term_count = ordered_source_term_count(&select);
    plan->index_fused_source_count = fused_source_count(&select);
    plan->has_cost_estimate = select.cost_estimated;
    plan->estimated_rows = select.estimated_rows;
    plan->estimated_table_rows = select.estimated_table_rows;
    plan->estimated_cost = select.estimated_cost;
    plan->estimated_scan_cost = select.estimated_scan_cost;
    snprintf(plan->table_name, sizeof(plan->table_name), "%s", select.schema->name);
    snprintf(plan->index_name, sizeof(plan->index_name), "%s", "multiple");
    snprintf(plan->projection,
             sizeof(plan->projection),
             "%s",
             select.projection_kind == INTERSECTION_PROJECTION_STAR
                 ? "*"
                 : select.projection_kind == INTERSECTION_PROJECTION_COUNT
                       ? "COUNT(*)"
                       : select.schema->columns[select.projection_column_index].name);
    if (!build_filter_expression(&select,
                                 plan->filter_expression,
                                 sizeof(plan->filter_expression))) {
        return execute_error(output,
                             "generic index intersection plan text exceeds capacity");
    }

    output->status = TINYDB_GENERIC_SQL_SUCCESS;
    output->statement_type = STATEMENT_SELECT;
    output->statement_type_valid = true;
    output->execute_result = EXECUTE_SUCCESS;
    return output->status;
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable ||
        plan->kind != TINYDB_GENERIC_PLAN_SECONDARY_INDEX_INTERSECTION) {
        tinydb_generic_sql_print_plan_single_anchor_base(plan);
        return;
    }

    printf("PLAN: GENERIC SECONDARY INDEX INTERSECTION\n");
    printf("  TABLE: %s (root page %u)\n", plan->table_name, plan->root_page_num);
    printf("  INDEXES: %u\n", plan->index_branch_count);
    printf("  INDEX TERMS: %u\n", plan->index_term_count);
    if (plan->index_fused_source_count > 0) {
        printf("  FUSED SOURCES: %u\n", plan->index_fused_source_count);
    }
    printf("  PROJECTION: %s\n", plan->projection);
    if (plan->has_cost_estimate) {
        printf("  ESTIMATED ROWS: %u / %u\n",
               plan->estimated_rows,
               plan->estimated_table_rows);
        printf("  ESTIMATED COST: %llu (scan %llu)\n",
               (unsigned long long)plan->estimated_cost,
               (unsigned long long)plan->estimated_scan_cost);
    }
    printf("  FILTER: %s\n", plan->filter_expression);
}
