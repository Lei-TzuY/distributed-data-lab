#include "generic_index_candidates.h"
#include "generic_index_cost.h"
#include "generic_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    ANCHOR_V2_PROJECTION_STAR = 0,
    ANCHOR_V2_PROJECTION_COUNT,
    ANCHOR_V2_PROJECTION_COLUMN
} AnchorV2ProjectionKind;

typedef struct {
    TableSchema* schema;
    TinyDBGenericPredicate predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t predicate_count;
    uint32_t anchor_predicate_index;
    GenericSecondaryIndex* index;
    AnchorV2ProjectionKind projection_kind;
    uint32_t projection_column_index;
    bool has_limit;
    uint32_t limit;
    uint32_t offset;
    bool cost_estimated;
    uint32_t estimated_candidate_count;
    uint32_t estimated_table_rows;
    uint64_t estimated_cost;
    uint64_t estimated_scan_cost;
} AnchorV2Select;

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
                             AnchorV2Select* select) {
    if (tinydb_generic_consume_char(parser, '*')) {
        select->projection_kind = ANCHOR_V2_PROJECTION_STAR;
        return true;
    }
    TinyDBGenericParser backup = *parser;
    if (tinydb_generic_consume_word(parser, "count") &&
        tinydb_generic_consume_char(parser, '(') &&
        tinydb_generic_consume_char(parser, '*') &&
        tinydb_generic_consume_char(parser, ')')) {
        select->projection_kind = ANCHOR_V2_PROJECTION_COUNT;
        return true;
    }
    *parser = backup;

    char column[MAX_NAME_SIZE];
    if (!tinydb_generic_parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = tinydb_generic_find_column_index(schema, column);
    if (column_index < 0) return false;
    select->projection_kind = ANCHOR_V2_PROJECTION_COLUMN;
    select->projection_column_index = (uint32_t)column_index;
    return true;
}

static bool has_primary_key_equality(const AnchorV2Select* select) {
    for (uint32_t i = 0; i < select->predicate_count; i++) {
        if (select->predicates[i].column_index == 0 &&
            select->predicates[i].op == TINYDB_GENERIC_COMPARE_EQ) {
            return true;
        }
    }
    return false;
}

static uint32_t collect_column_predicates(
    const AnchorV2Select* select,
    uint32_t column_index,
    TinyDBGenericPredicate* predicates,
    uint32_t capacity) {
    if (select == NULL || predicates == NULL || capacity == 0) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < select->predicate_count && count < capacity; i++) {
        const TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->column_index == column_index &&
            predicate->op <= TINYDB_GENERIC_COMPARE_LTE) {
            predicates[count++] = *predicate;
        }
    }
    return count;
}

static bool column_seen_before(const AnchorV2Select* select,
                               uint32_t predicate_index) {
    uint32_t column_index = select->predicates[predicate_index].column_index;
    for (uint32_t i = 0; i < predicate_index; i++) {
        const TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->column_index == column_index &&
            predicate->op <= TINYDB_GENERIC_COMPARE_LTE) {
            return true;
        }
    }
    return false;
}

static uint32_t preferred_predicate_for_column(
    const AnchorV2Select* select,
    uint32_t column_index,
    bool* has_equality) {
    uint32_t first = UINT32_MAX;
    *has_equality = false;
    for (uint32_t i = 0; i < select->predicate_count; i++) {
        const TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->column_index != column_index ||
            predicate->op > TINYDB_GENERIC_COMPARE_LTE) {
            continue;
        }
        if (first == UINT32_MAX) first = i;
        if (predicate->op == TINYDB_GENERIC_COMPARE_EQ) {
            *has_equality = true;
            return i;
        }
    }
    return first;
}

static bool choose_anchor_legacy(Table* table, AnchorV2Select* select) {
    uint32_t best_score = 0;
    GenericSecondaryIndex* best_index = NULL;
    uint32_t best_predicate = 0;
    for (uint32_t i = 0; i < select->predicate_count; i++) {
        TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->op == TINYDB_GENERIC_COMPARE_LIKE) continue;
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

static bool choose_anchor(Table* table, AnchorV2Select* select) {
    GenericSecondaryIndex* best_index = NULL;
    uint32_t best_predicate = UINT32_MAX;
    uint32_t best_candidate_count = UINT32_MAX;
    uint32_t best_term_count = 0;
    uint32_t best_table_rows = 0;
    bool best_has_equality = false;
    bool estimated_any = false;

    for (uint32_t i = 0; i < select->predicate_count; i++) {
        TinyDBGenericPredicate* predicate = &select->predicates[i];
        if (predicate->op == TINYDB_GENERIC_COMPARE_LIKE ||
            predicate->column_index == 0 ||
            column_seen_before(select, i)) {
            continue;
        }
        GenericSecondaryIndex* index = find_single_column_index(
            table, select->schema, predicate->column_index);
        if (index == NULL) continue;

        TinyDBGenericPredicate source_predicates[MAX_COLUMNS_PER_TABLE];
        uint32_t source_count = collect_column_predicates(
            select,
            predicate->column_index,
            source_predicates,
            MAX_COLUMNS_PER_TABLE);
        if (source_count == 0) continue;

        TinyDBGenericIndexEstimate estimate;
        char message[TINYDB_GENERIC_SQL_MESSAGE_MAX] = {0};
        bool estimated = source_count >= 2
            ? tinydb_generic_index_estimate_conjunctive_candidates(
                  table,
                  select->schema,
                  index,
                  source_predicates,
                  source_count,
                  &estimate,
                  message,
                  sizeof(message))
            : tinydb_generic_index_estimate_candidates(
                  table,
                  select->schema,
                  index,
                  &source_predicates[0],
                  &estimate,
                  message,
                  sizeof(message));
        if (!estimated) continue;
        estimated_any = true;

        bool has_equality = false;
        uint32_t preferred = preferred_predicate_for_column(
            select, predicate->column_index, &has_equality);
        if (preferred == UINT32_MAX) continue;

        if (best_index == NULL ||
            estimate.candidate_count < best_candidate_count ||
            (estimate.candidate_count == best_candidate_count &&
             has_equality && !best_has_equality) ||
            (estimate.candidate_count == best_candidate_count &&
             has_equality == best_has_equality && source_count > best_term_count)) {
            best_index = index;
            best_predicate = preferred;
            best_candidate_count = estimate.candidate_count;
            best_term_count = source_count;
            best_table_rows = estimate.total_count;
            best_has_equality = has_equality;
        }
    }

    if (!estimated_any || best_index == NULL) {
        return choose_anchor_legacy(table, select);
    }

    uint64_t anchor_cost = tinydb_generic_anchor_cost(best_candidate_count);
    uint64_t scan_cost = tinydb_generic_scan_cost(best_table_rows);
    if (anchor_cost > scan_cost) {
        return false;
    }

    select->index = best_index;
    select->anchor_predicate_index = best_predicate;
    select->cost_estimated = true;
    select->estimated_candidate_count = best_candidate_count;
    select->estimated_table_rows = best_table_rows;
    select->estimated_cost = anchor_cost;
    select->estimated_scan_cost = scan_cost;
    return true;
}

static uint32_t collect_anchor_predicates(
    const AnchorV2Select* select,
    TinyDBGenericPredicate* predicates,
    uint32_t capacity) {
    if (select == NULL || predicates == NULL || capacity == 0) return 0;
    uint32_t column_index =
        select->predicates[select->anchor_predicate_index].column_index;
    return collect_column_predicates(select,
                                     column_index,
                                     predicates,
                                     capacity);
}

static bool parse_anchor_select(Table* table,
                                const char* sql,
                                AnchorV2Select* select) {
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
        !choose_anchor(table, select)) {
        return false;
    }
    return true;
}

static bool predicates_match(const AnchorV2Select* select,
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

static TinyDBGenericSqlStatus execute_anchor_select(
    Table* table,
    const AnchorV2Select* select,
    TinyDBGenericSqlResult* result) {
    TinyDBGenericPredicate index_predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t index_predicate_count = collect_anchor_predicates(
        select,
        index_predicates,
        MAX_COLUMNS_PER_TABLE);
    if (index_predicate_count == 0) {
        return execute_error(result, "compound index anchor has no ordered predicate");
    }

    TinyDBGenericIndexCandidates candidates;
    char message[TINYDB_GENERIC_SQL_MESSAGE_MAX] = {0};
    bool collected = index_predicate_count >= 2
        ? tinydb_generic_index_collect_conjunctive_candidates(
              table,
              select->schema,
              select->index,
              index_predicates,
              index_predicate_count,
              &candidates,
              message,
              sizeof(message))
        : tinydb_generic_index_collect_candidates(
              table,
              select->schema,
              select->index,
              &index_predicates[0],
              &candidates,
              message,
              sizeof(message));
    if (!collected) {
        return execute_error(result,
                             message[0] != '\0' ? message
                                                 : "unable to collect compound index candidates");
    }

    uint32_t matched = 0;
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < candidates.count; i++) {
        TinyDBRecord record;
        if (!tinydb_record_find(table,
                                select->schema,
                                candidates.ids[i],
                                &record)) {
            continue;
        }
        TinyDBValue values[MAX_COLUMNS_PER_TABLE];
        if (!decode_values(select->schema, &record, values)) {
            tinydb_generic_index_candidates_free(&candidates);
            return execute_error(result, "unable to decode compound index candidate");
        }
        if (!predicates_match(select, values)) continue;
        matched++;
        if (select->projection_kind == ANCHOR_V2_PROJECTION_COUNT) continue;
        if (matched <= select->offset) continue;
        if (select->has_limit && emitted >= select->limit) break;
        if (select->projection_kind == ANCHOR_V2_PROJECTION_COLUMN) {
            print_value(&values[select->projection_column_index]);
        } else {
            tinydb_record_print(select->schema, &record);
        }
        emitted++;
    }

    if (select->projection_kind == ANCHOR_V2_PROJECTION_COUNT) {
        uint32_t count = matched;
        if (select->offset > 0) count = 0;
        if (select->has_limit && select->limit == 0) count = 0;
        printf("%u\n", count);
    }
    tinydb_generic_index_candidates_free(&candidates);
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

static bool build_filter_expression(const AnchorV2Select* select,
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

    AnchorV2Select select;
    if (parse_anchor_select(table, sql, &select)) {
        return execute_anchor_select(table, &select, output);
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

    AnchorV2Select select;
    if (!parse_anchor_select(table, sql, &select)) {
        return tinydb_generic_sql_build_select_plan_range_index_base(
            table, sql, plan, output);
    }

    const TinyDBGenericPredicate* anchor =
        &select.predicates[select.anchor_predicate_index];
    TinyDBGenericPredicate index_predicates[MAX_COLUMNS_PER_TABLE];
    uint32_t index_predicate_count = collect_anchor_predicates(
        &select,
        index_predicates,
        MAX_COLUMNS_PER_TABLE);

    plan->applicable = true;
    plan->kind = TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RESIDUAL;
    plan->root_page_num = select.schema->root_page_num;
    plan->has_filter = true;
    plan->index_branch_count = index_predicate_count;
    plan->has_cost_estimate = select.cost_estimated;
    plan->estimated_rows = select.estimated_candidate_count;
    plan->estimated_table_rows = select.estimated_table_rows;
    plan->estimated_cost = select.estimated_cost;
    plan->estimated_scan_cost = select.estimated_scan_cost;
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
    format_predicate_value(anchor,
                           plan->filter_value,
                           sizeof(plan->filter_value));
    snprintf(plan->projection,
             sizeof(plan->projection),
             "%s",
             select.projection_kind == ANCHOR_V2_PROJECTION_STAR
                 ? "*"
                 : select.projection_kind == ANCHOR_V2_PROJECTION_COUNT
                       ? "COUNT(*)"
                       : select.schema->columns[select.projection_column_index].name);
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

static void print_cost(const TinyDBGenericSelectPlan* plan) {
    if (!plan->has_cost_estimate) return;
    printf("  ESTIMATED ROWS: %u / %u\n",
           plan->estimated_rows,
           plan->estimated_table_rows);
    printf("  ESTIMATED COST: %llu (scan %llu)\n",
           (unsigned long long)plan->estimated_cost,
           (unsigned long long)plan->estimated_scan_cost);
}

void tinydb_generic_sql_print_plan(const TinyDBGenericSelectPlan* plan) {
    if (plan == NULL || !plan->applicable ||
        plan->kind != TINYDB_GENERIC_PLAN_SECONDARY_INDEX_RESIDUAL) {
        tinydb_generic_sql_print_plan_range_index_base(plan);
        return;
    }
    if (plan->index_branch_count >= 2) {
        printf("PLAN: GENERIC SECONDARY INDEX FUSED RANGE\n");
        printf("  TABLE: %s (root page %u)\n", plan->table_name, plan->root_page_num);
        printf("  INDEX: %s\n", plan->index_name);
        printf("  PROJECTION: %s\n", plan->projection);
        printf("  RANGE TERMS: %u on %s\n",
               plan->index_branch_count,
               plan->filter_column);
        print_cost(plan);
        printf("  FILTER: %s\n", plan->filter_expression);
        return;
    }
    printf("PLAN: GENERIC SECONDARY INDEX + RESIDUAL FILTER\n");
    printf("  TABLE: %s (root page %u)\n", plan->table_name, plan->root_page_num);
    printf("  INDEX: %s\n", plan->index_name);
    printf("  PROJECTION: %s\n", plan->projection);
    printf("  ANCHOR: %s %s %s\n",
           plan->filter_column,
           plan->filter_operator,
           plan->filter_value);
    print_cost(plan);
    printf("  FILTER: %s\n", plan->filter_expression);
}
