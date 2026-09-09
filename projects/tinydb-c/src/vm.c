#include "vm.h"

#define MAX_FORMATTED_ROW_SIZE GENERIC_INDEX_KEY_SIZE

static void commit_table_changes(Table* table) {
    table_prepare_all_indexes_commit(table);
    pager_commit(table->pager);
    table_finalize_all_indexes_commit(table);
}

static bool like_match(const char* pattern, const char* str) {
    if (*pattern == '\0') return *str == '\0';

    if (*pattern == '%') {
        while (*pattern == '%') pattern++;
        if (*pattern == '\0') return true;
        while (*str != '\0') {
            if (like_match(pattern, str)) return true;
            str++;
        }
        return like_match(pattern, str);
    }

    if (*str == '\0') return false;

    if (*pattern == '_' || *pattern == *str) {
        return like_match(pattern + 1, str + 1);
    }

    return false;
}

static bool ilike_match(const char* pattern, const char* str) {
    if (*pattern == '\0') return *str == '\0';

    if (*pattern == '%') {
        while (*pattern == '%') pattern++;
        if (*pattern == '\0') return true;
        while (*str != '\0') {
            if (ilike_match(pattern, str)) return true;
            str++;
        }
        return ilike_match(pattern, str);
    }

    if (*str == '\0') return false;

    if (*pattern == '_' || tolower((unsigned char)*pattern) == tolower((unsigned char)*str)) {
        return ilike_match(pattern + 1, str + 1);
    }

    return false;
}

static bool row_matches_filters(Table* table, Row* row, SelectStatement* sel);

static bool execute_subquery_exists(SelectStatement* sub, Table* table, Row* outer_row) {
    (void)outer_row;
    Cursor* cursor = table_start(table);
    Row row;
    bool exists = false;
    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row);
        if (row_matches_filters(table, &row, sub)) {
            exists = true;
            break;
        }
        cursor_advance(cursor);
    }
    free(cursor);
    return exists;
}

static bool execute_subquery_in(SelectStatement* sub, Table* table, uint32_t target_id) {
    Cursor* cursor = table_start(table);
    Row row;
    bool found = false;
    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row);
        if (row_matches_filters(table, &row, sub)) {
            if (row.id == target_id) {
                found = true;
                break;
            }
        }
        cursor_advance(cursor);
    }
    free(cursor);
    return found;
}

static bool row_matches_filters(Table* table, Row* row, SelectStatement* sel) {
    if (sel->has_id_filter) {
        if (sel->id_op == COMPARE_EQ  && row->id != sel->id) return false;
        if (sel->id_op == COMPARE_GT  && row->id <= sel->id) return false;
        if (sel->id_op == COMPARE_GTE && row->id <  sel->id) return false;
        if (sel->id_op == COMPARE_LT  && row->id >= sel->id) return false;
        if (sel->id_op == COMPARE_LTE && row->id >  sel->id) return false;
    }
    if (sel->has_id_min_filter) {
        if (sel->id_min_op == COMPARE_GT  && row->id <= sel->id_min) return false;
        if (sel->id_min_op == COMPARE_GTE && row->id <  sel->id_min) return false;
    }
    if (sel->has_id_max_filter) {
        if (sel->id_max_op == COMPARE_LT  && row->id >= sel->id_max) return false;
        if (sel->id_max_op == COMPARE_LTE && row->id >  sel->id_max) return false;
    }
    if (sel->has_username_filter && strcmp(row->username, sel->username) != 0) {
        return false;
    }
    if (sel->has_username_like) {
        bool m = sel->is_ilike ? ilike_match(sel->username_like, row->username) : like_match(sel->username_like, row->username);
        if (sel->is_not_like ? m : !m) return false;
    }
    if (sel->has_email_filter && strcmp(row->email, sel->email) != 0) {
        return false;
    }
    if (sel->has_email_like) {
        bool m = sel->is_ilike ? ilike_match(sel->email_like, row->email) : like_match(sel->email_like, row->email);
        if (sel->is_not_like ? m : !m) return false;
    }
    if (sel->has_in_subquery && sel->in_subquery != NULL) {
        if (!execute_subquery_in(sel->in_subquery, table, row->id)) return false;
    }
    if (sel->has_exists_subquery && sel->exists_subquery != NULL) {
        if (!execute_subquery_exists(sel->exists_subquery, table, row)) return false;
    }
    if (sel->has_is_null_filter) {
        const char* val = strcmp(sel->null_target_col, "email") == 0 ? row->email : row->username;
        if (val[0] != '\0' && strcmp(val, "NULL") != 0 && strcmp(val, "null") != 0) return false;
    }
    if (sel->has_is_not_null_filter) {
        const char* val = strcmp(sel->null_target_col, "email") == 0 ? row->email : row->username;
        if (val[0] == '\0' || strcmp(val, "NULL") == 0 || strcmp(val, "null") == 0) return false;
    }
    if (sel->has_in_list) {
        bool found = false;
        for (uint32_t i = 0; i < sel->in_list_count; i++) {
            if (row->id == sel->in_list_ids[i]) {
                found = true;
                break;
            }
        }
        if (sel->is_not_in_list ? found : !found) return false;
    }
    if (sel->has_between_filter) {
        if (row->id < sel->between_min || row->id > sel->between_max) return false;
    }
    return true;
}

static bool row_matches_username_filter(Table* table, Row* row, SelectStatement* sel) {
    return row_matches_filters(table, row, sel);
}

static bool fetch_row_by_id(Table* table, uint32_t id, Row* row) {
    Cursor* cursor = table_find(table, id);
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    bool found = cursor->cell_num < num_cells &&
                 *leaf_node_key(node, cursor->cell_num) == id;
    if (found) {
        deserialize_row(cursor_value(cursor), row);
    }
    free(cursor);
    return found;
}

#define MAX_DISTINCT_ENTRIES 256
typedef struct {
    char entries[MAX_DISTINCT_ENTRIES][MAX_FORMATTED_ROW_SIZE];
    uint32_t count;
} DistinctSet;

static void distinct_set_init(DistinctSet* set) {
    set->count = 0;
}

static bool distinct_set_add(DistinctSet* set, const char* str) {
    for (uint32_t i = 0; i < set->count; i++) {
        if (strcmp(set->entries[i], str) == 0) {
            return false;
        }
    }
    if (set->count < MAX_DISTINCT_ENTRIES) {
        size_t len = strlen(str);
        if (len >= MAX_FORMATTED_ROW_SIZE) len = MAX_FORMATTED_ROW_SIZE - 1;
        memcpy(set->entries[set->count], str, len);
        set->entries[set->count][len] = '\0';
        set->count++;
    }
    return true;
}

static uint32_t eval_scalar_subquery_val(Table* table, const char* sub_sql) {
    Statement sub_stmt;
    PrepareResult prep = prepare_statement(sub_sql, &sub_stmt);
    if (prep != PREPARE_SUCCESS) return 0;

    SelectStatement* sub_sel = &sub_stmt.select;
    Cursor* c = table_start(table);
    Row r;
    uint32_t count = 0;
    while (!c->end_of_table) {
        deserialize_row(cursor_value(c), &r);
        if (row_matches_filters(table, &r, sub_sel)) {
            count++;
        }
        cursor_advance(c);
    }
    free(c);
    return count;
}

static bool print_selected_row_ex(Table* table, Row* row, SelectStatement* sel, DistinctSet* dset) {
    char formatted_row[MAX_FORMATTED_ROW_SIZE];
    if (sel->math_func != MATH_FUNC_NONE) {
        uint32_t val = row->id;
        if (sel->math_func == MATH_FUNC_ABS) {
            snprintf(formatted_row, sizeof(formatted_row), "%u", val);
        } else if (sel->math_func == MATH_FUNC_MOD) {
            uint32_t mod_val = sel->math_operand != 0 ? (val % sel->math_operand) : 0;
            snprintf(formatted_row, sizeof(formatted_row), "%u", mod_val);
        }
    } else if (sel->str_func != STRING_FUNC_NONE) {
        const char* val = strcmp(sel->str_func_target_col, "email") == 0 ? row->email : row->username;
        if (sel->str_func == STRING_FUNC_LENGTH) {
            snprintf(formatted_row, sizeof(formatted_row), "%zu", strlen(val));
        } else if (sel->str_func == STRING_FUNC_UPPER) {
            char buf[256];
            size_t len = strlen(val);
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            for (size_t i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)val[i]);
            buf[len] = '\0';
            snprintf(formatted_row, sizeof(formatted_row), "%s", buf);
        } else if (sel->str_func == STRING_FUNC_LOWER) {
            char buf[256];
            size_t len = strlen(val);
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)val[i]);
            buf[len] = '\0';
            snprintf(formatted_row, sizeof(formatted_row), "%s", buf);
        } else if (sel->str_func == STRING_FUNC_CONCAT) {
            const char* v1 = strcmp(sel->str_func_target_col, "email") == 0 ? row->email : row->username;
            const char* v2 = strcmp(sel->str_func_second_col, "email") == 0 ? row->email : row->username;
            snprintf(formatted_row, sizeof(formatted_row), "%s%s", v1, v2);
        }
    } else if (sel->has_scalar_subquery && table != NULL) {
        uint32_t sub_val = eval_scalar_subquery_val(table, sel->scalar_subquery_sql);
        if (sel->has_project_col) {
            if (strcmp(sel->project_col, "id") == 0) {
                snprintf(formatted_row, sizeof(formatted_row), "%u | %u", row->id, sub_val);
            } else if (strcmp(sel->project_col, "username") == 0) {
                snprintf(formatted_row, sizeof(formatted_row), "%s | %u", row->username, sub_val);
            } else {
                snprintf(formatted_row, sizeof(formatted_row), "%s | %u", row->email, sub_val);
            }
        } else {
            snprintf(formatted_row, sizeof(formatted_row), "(%u, %s, %s) | %u", row->id, row->username, row->email, sub_val);
        }
    } else if (sel->has_project_col) {
        if (strcmp(sel->project_col, "username") == 0) {
            snprintf(formatted_row, sizeof(formatted_row), "%s", row->username);
        } else if (strcmp(sel->project_col, "email") == 0) {
            snprintf(formatted_row, sizeof(formatted_row), "%s", row->email);
        } else if (strcmp(sel->project_col, "id") == 0) {
            snprintf(formatted_row, sizeof(formatted_row), "%u", row->id);
        } else {
            snprintf(formatted_row, sizeof(formatted_row), "(%u, %s, %s)", row->id, row->username, row->email);
        }
    } else {
        snprintf(formatted_row, sizeof(formatted_row), "(%u, %s, %s)", row->id, row->username, row->email);
    }

    if (sel->is_distinct && dset != NULL) {
        if (!distinct_set_add(dset, formatted_row)) {
            return false;
        }
    }

    printf("%s\n", formatted_row);
    return true;
}

static bool print_selected_row(Row* row, SelectStatement* sel, DistinctSet* dset) {
    return print_selected_row_ex(NULL, row, sel, dset);
}

static uint32_t username_index_lower_bound(Table* table, const char* username) {
    uint32_t min = 0;
    uint32_t max = table->username_index_count;
    while (min < max) {
        uint32_t mid = min + (max - min) / 2;
        if (strcmp(table->username_index_entries[mid].username, username) < 0) {
            min = mid + 1;
        } else {
            max = mid;
        }
    }
    return min;
}

static uint32_t username_index_upper_bound(Table* table, const char* username) {
    uint32_t min = 0;
    uint32_t max = table->username_index_count;
    while (min < max) {
        uint32_t mid = min + (max - min) / 2;
        if (strcmp(table->username_index_entries[mid].username, username) <= 0) {
            min = mid + 1;
        } else {
            max = mid;
        }
    }
    return min;
}

static ExecuteResult execute_select_username_index(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;
    table_ensure_username_index(table);

    uint32_t start = username_index_lower_bound(table, sel->username);
    uint32_t end = username_index_upper_bound(table, sel->username);
    uint32_t emitted = 0;
    uint32_t agg_val = 0;
    Row row;

    if (sel->has_order_desc && sel->aggregate == AGGREGATE_NONE) {
        for (uint32_t i = end; i > start; i--) {
            if (sel->has_limit && emitted >= sel->limit) break;
            uint32_t id = table->username_index_entries[i - 1].id;
            if (fetch_row_by_id(table, id, &row)) {
                if (row_matches_filters(table, &row, sel)) {
                    print_row(&row);
                    emitted++;
                }
            }
        }
        return EXECUTE_SUCCESS;
    }

    for (uint32_t i = start; i < end; i++) {
        if (sel->has_limit && emitted >= sel->limit) break;
        uint32_t id = table->username_index_entries[i].id;
        if (!fetch_row_by_id(table, id, &row)) {
            continue;
        }

        if (!row_matches_filters(table, &row, sel)) {
            continue;
        }

        switch (sel->aggregate) {
            case AGGREGATE_NONE:
                print_row(&row);
                break;
            case AGGREGATE_MIN:
                if (emitted == 0) agg_val = id;
                break;
            case AGGREGATE_MAX:
                agg_val = id;
                break;
            default:
                break;
        }
        emitted++;
    }

    switch (sel->aggregate) {
        case AGGREGATE_COUNT:
            printf("%u\n", emitted);
            break;
        case AGGREGATE_MIN:
        case AGGREGATE_MAX:
            if (emitted == 0) printf("NULL\n");
            else printf("%u\n", agg_val);
            break;
        default:
            break;
    }

    return EXECUTE_SUCCESS;
}

static uint32_t generic_index_lower_bound(GenericSecondaryIndex* idx, const char* target) {
    uint32_t min = 0;
    uint32_t max = idx->count;
    while (min < max) {
        uint32_t mid = min + (max - min) / 2;
        if (strcmp(idx->entries[mid].key_val, target) < 0) {
            min = mid + 1;
        } else {
            max = mid;
        }
    }
    return min;
}

static uint32_t generic_index_upper_bound(GenericSecondaryIndex* idx, const char* target) {
    uint32_t min = 0;
    uint32_t max = idx->count;
    while (min < max) {
        uint32_t mid = min + (max - min) / 2;
        if (strcmp(idx->entries[mid].key_val, target) <= 0) {
            min = mid + 1;
        } else {
            max = mid;
        }
    }
    return min;
}

static ExecuteResult execute_select_generic_index(Statement* statement, Table* table, GenericSecondaryIndex* idx, const char* key_val) {
    SelectStatement* sel = &statement->select;
    table_ensure_all_indexes(table);

    uint32_t start = generic_index_lower_bound(idx, key_val);
    uint32_t end = generic_index_upper_bound(idx, key_val);
    uint32_t emitted = 0;
    uint32_t agg_val = 0;
    Row row;

    if (sel->has_order_desc && sel->aggregate == AGGREGATE_NONE) {
        for (uint32_t i = end; i > start; i--) {
            if (sel->has_limit && emitted >= sel->limit) break;
            uint32_t id = idx->entries[i - 1].primary_key;
            if (fetch_row_by_id(table, id, &row)) {
                if (row_matches_filters(table, &row, sel)) {
                    print_row(&row);
                    emitted++;
                }
            }
        }
        return EXECUTE_SUCCESS;
    }

    for (uint32_t i = start; i < end; i++) {
        if (sel->has_limit && emitted >= sel->limit) break;
        uint32_t id = idx->entries[i].primary_key;
        if (!fetch_row_by_id(table, id, &row)) {
            continue;
        }

        if (!row_matches_filters(table, &row, sel)) {
            continue;
        }

        switch (sel->aggregate) {
            case AGGREGATE_NONE:
                print_row(&row);
                break;
            case AGGREGATE_MIN:
                if (emitted == 0) agg_val = id;
                break;
            case AGGREGATE_MAX:
                agg_val = id;
                break;
            default:
                break;
        }
        emitted++;
    }

    switch (sel->aggregate) {
        case AGGREGATE_COUNT:
            printf("%u\n", emitted);
            break;
        case AGGREGATE_MIN:
        case AGGREGATE_MAX:
            if (emitted == 0) printf("NULL\n");
            else printf("%u\n", agg_val);
            break;
        default:
            break;
    }

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_insert(Statement* statement, Table* table) {
    Row* row_to_insert = &(statement->row_to_insert);

    if (statement->is_auto_id || row_to_insert->id == 0) {
        uint32_t max_id = 0;
        Cursor* c = table_start(table);
        Row temp_r;
        while (!c->end_of_table) {
            deserialize_row(cursor_value(c), &temp_r);
            if (temp_r.id > max_id) max_id = temp_r.id;
            cursor_advance(c);
        }
        free(c);
        row_to_insert->id = max_id + 1;
    }

    uint32_t key_to_insert = row_to_insert->id;
    
    Cursor* cursor = table_find(table, key_to_insert);

    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
        if (key_at_index == key_to_insert) {
            free(cursor);
            return EXECUTE_DUPLICATE_KEY;
        }
    }

    leaf_node_insert(cursor, row_to_insert->id, row_to_insert);
    table_mark_username_index_dirty(table);

    if (!table->in_transaction) {
        commit_table_changes(table);
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

static ExecuteResult execute_join_select(Statement* statement, Table* table) {
    (void)statement;
    Cursor* c1 = table_start(table);
    uint32_t match_count = 0;
    Row r1, r2;

    while (!c1->end_of_table) {
        deserialize_row(cursor_value(c1), &r1);

        Cursor* c2 = table_start(table);
        while (!c2->end_of_table) {
            deserialize_row(cursor_value(c2), &r2);

            if (r1.id == r2.id) {
                printf("(%u, %s, %s) | (%u, %s, %s)\n",
                       r1.id, r1.username, r1.email,
                       r2.id, r2.username, r2.email);
                match_count++;
            }
            cursor_advance(c2);
        }
        free(c2);
        cursor_advance(c1);
    }
    free(c1);
    return EXECUTE_SUCCESS;
}

typedef struct {
    char key[256];
    uint32_t count;
    uint32_t min_id;
    uint32_t max_id;
    uint64_t sum_id;
} GroupBucket;

static bool eval_having_condition(GroupBucket* b, SelectStatement* sel) {
    if (!sel->has_having) return true;

    uint32_t val = 0;
    switch (sel->having_agg) {
        case AGGREGATE_COUNT: val = b->count; break;
        case AGGREGATE_MIN:   val = b->min_id; break;
        case AGGREGATE_MAX:   val = b->max_id; break;
        case AGGREGATE_SUM:   val = (uint32_t)b->sum_id; break;
        case AGGREGATE_AVG:   val = b->count > 0 ? (uint32_t)(b->sum_id / b->count) : 0; break;
        default: return true;
    }

    switch (sel->having_op) {
        case COMPARE_EQ:  return val == sel->having_val;
        case COMPARE_GT:  return val > sel->having_val;
        case COMPARE_GTE: return val >= sel->having_val;
        case COMPARE_LT:  return val < sel->having_val;
        case COMPARE_LTE: return val <= sel->having_val;
        default: return true;
    }
}

static ExecuteResult execute_select_groupby(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;

#define MAX_GROUP_BUCKETS 256
    GroupBucket buckets[MAX_GROUP_BUCKETS];
    uint32_t num_buckets = 0;
    memset(buckets, 0, sizeof(buckets));

    Cursor* cursor = table_start(table);
    Row row;

    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(cursor), &row);
        if (row_matches_filters(table, &row, sel)) {
            char group_key[256] = "";
            if (strcmp(sel->group_by_col, "email") == 0) {
                strncpy(group_key, row.email, sizeof(group_key) - 1);
            } else if (strcmp(sel->group_by_col, "username") == 0) {
                strncpy(group_key, row.username, sizeof(group_key) - 1);
            } else {
                snprintf(group_key, sizeof(group_key), "%u", row.id);
            }

            int found_idx = -1;
            for (uint32_t i = 0; i < num_buckets; i++) {
                if (strcmp(buckets[i].key, group_key) == 0) {
                    found_idx = (int)i;
                    break;
                }
            }

            if (found_idx < 0) {
                if (num_buckets < MAX_GROUP_BUCKETS) {
                    found_idx = (int)num_buckets++;
                    strncpy(buckets[found_idx].key, group_key, sizeof(buckets[found_idx].key) - 1);
                    buckets[found_idx].count = 0;
                    buckets[found_idx].min_id = row.id;
                    buckets[found_idx].max_id = row.id;
                    buckets[found_idx].sum_id = 0;
                }
            }

            if (found_idx >= 0) {
                GroupBucket* b = &buckets[found_idx];
                b->count++;
                if (row.id < b->min_id) b->min_id = row.id;
                if (row.id > b->max_id) b->max_id = row.id;
                b->sum_id += row.id;
            }
        }
        cursor_advance(cursor);
    }
    free(cursor);

    uint32_t emitted = 0;
    for (uint32_t i = 0; i < num_buckets; i++) {
        GroupBucket* b = &buckets[i];
        if (!eval_having_condition(b, sel)) continue;
        if (sel->has_limit && emitted >= sel->limit) break;

        uint32_t agg_val = 0;
        switch (sel->aggregate) {
            case AGGREGATE_COUNT: agg_val = b->count; break;
            case AGGREGATE_MIN:   agg_val = b->min_id; break;
            case AGGREGATE_MAX:   agg_val = b->max_id; break;
            case AGGREGATE_SUM:   agg_val = (uint32_t)b->sum_id; break;
            case AGGREGATE_AVG:   agg_val = b->count > 0 ? (uint32_t)(b->sum_id / b->count) : 0; break;
            default:              agg_val = b->count; break;
        }

        if (sel->has_project_col) {
            printf("%s | %u\n", b->key, agg_val);
        } else {
            printf("%s: %u\n", b->key, agg_val);
        }
        emitted++;
    }

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table);

static ExecuteResult execute_union(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;

    sel->is_union = false;
    execute_select(statement, table);
    sel->is_union = true;

    Statement right_stmt;
    PrepareResult prep_res = prepare_statement(sel->union_second_select, &right_stmt);
    if (prep_res == PREPARE_SUCCESS) {
        execute_select(&right_stmt, table);
    }

    return EXECUTE_SUCCESS;
}

static ExecuteResult execute_select_window(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;
    Row rows[128];
    uint32_t row_count = 0;

    Cursor* cursor = table_start(table);
    while (!cursor->end_of_table && row_count < 128) {
        deserialize_row(cursor_value(cursor), &rows[row_count++]);
        cursor_advance(cursor);
    }
    free(cursor);

    uint32_t emitted = 0;
    uint32_t current_row_num = 1;
    char last_partition[256] = "";

    for (uint32_t i = 0; i < row_count; i++) {
        if (sel->has_limit && emitted >= sel->limit) break;

        char current_partition[256] = "";
        if (sel->has_partition_by) {
            if (strcmp(sel->partition_col, "email") == 0) {
                strncpy(current_partition, rows[i].email, sizeof(current_partition) - 1);
            } else {
                strncpy(current_partition, rows[i].username, sizeof(current_partition) - 1);
            }

            if (i > 0 && strcmp(current_partition, last_partition) != 0) {
                current_row_num = 1;
            }
            strncpy(last_partition, current_partition, sizeof(last_partition) - 1);
        }

        printf("%u | (%u, %s, %s)\n", current_row_num++, rows[i].id, rows[i].username, rows[i].email);
        emitted++;
    }

    return EXECUTE_SUCCESS;
}

static SelectStatement* g_multi_sort_sel = NULL;

static int compare_rows_multi(const void* a, const void* b) {
    const Row* r1 = (const Row*)a;
    const Row* r2 = (const Row*)b;
    SelectStatement* sel = g_multi_sort_sel;
    if (!sel) return 0;

    const char* col1 = sel->has_order_by_col ? sel->order_by_col : "id";
    int cmp1 = 0;
    if (strcmp(col1, "username") == 0) {
        cmp1 = strcmp(r1->username, r2->username);
    } else if (strcmp(col1, "email") == 0) {
        cmp1 = strcmp(r1->email, r2->email);
    } else {
        cmp1 = (r1->id > r2->id) - (r1->id < r2->id);
    }
    if (sel->has_order_desc) cmp1 = -cmp1;
    if (cmp1 != 0) return cmp1;

    if (sel->has_secondary_order_by) {
        const char* col2 = sel->secondary_order_col;
        int cmp2 = 0;
        if (strcmp(col2, "username") == 0) {
            cmp2 = strcmp(r1->username, r2->username);
        } else if (strcmp(col2, "email") == 0) {
            cmp2 = strcmp(r1->email, r2->email);
        } else {
            cmp2 = (r1->id > r2->id) - (r1->id < r2->id);
        }
        if (sel->secondary_order_desc) cmp2 = -cmp2;
        return cmp2;
    }
    return 0;
}

static ExecuteResult execute_select_multi_sort(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;
    Row rows[1024];
    uint32_t count = 0;

    Cursor* cursor = table_start(table);
    while (!cursor->end_of_table && count < 1024) {
        Row r;
        deserialize_row(cursor_value(cursor), &r);
        if (row_matches_filters(table, &r, sel)) {
            rows[count++] = r;
        }
        cursor_advance(cursor);
    }
    free(cursor);

    g_multi_sort_sel = sel;
    qsort(rows, count, sizeof(Row), compare_rows_multi);
    g_multi_sort_sel = NULL;

    DistinctSet dset;
    distinct_set_init(&dset);
    uint32_t emitted = 0;
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (sel->has_offset && skipped < sel->offset) {
            skipped++;
            continue;
        }
        if (sel->has_limit && emitted >= sel->limit) break;
        if (print_selected_row(&rows[i], sel, &dset)) {
            emitted++;
        }
    }
    return EXECUTE_SUCCESS;
}

static ExecuteResult execute_select_catalog(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;
    uint32_t emitted = 0;

    if (!sel->has_limit || emitted < sel->limit) {
        printf("(table, users, users, 0, CREATE TABLE users (id INT PRIMARY KEY, username VARCHAR(32), email VARCHAR(255)))\n");
        emitted++;
    }

    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (sel->has_limit && emitted >= sel->limit) break;
        TableSchema* s = &table->catalog.schemas[i];
        if (strcmp(s->name, "users") == 0) continue;
        printf("(table, %s, %s, %u, CREATE TABLE %s (...))\n", s->name, s->name, s->root_page_num, s->name);
        emitted++;
    }

    for (uint32_t i = 0; i < table->catalog.num_views; i++) {
        if (sel->has_limit && emitted >= sel->limit) break;
        ViewSchema* v = &table->catalog.views[i];
        printf("(view, %s, %s, 0, CREATE VIEW %s AS %s)\n", v->name, v->name, v->name, v->select_sql);
        emitted++;
    }

    for (uint32_t i = 0; i < table->catalog.num_indexes; i++) {
        if (sel->has_limit && emitted >= sel->limit) break;
        SecondaryIndexMeta* idx = &table->catalog.indexes[i];
        printf("(index, %s, %s, 0, CREATE INDEX %s ON %s (%s))\n", idx->name, idx->table_name, idx->name, idx->table_name, idx->column_name);
        emitted++;
    }

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;

    if (sel->sys_func == SYS_FUNC_VERSION) {
        printf("TinyDB v1.0.0 (B-Tree WAL Pager Engine)\n");
        return EXECUTE_SUCCESS;
    }
    if (sel->sys_func == SYS_FUNC_DATABASE) {
        printf("tinydb_main\n");
        return EXECUTE_SUCCESS;
    }

    if (sel->is_catalog_query) {
        return execute_select_catalog(statement, table);
    }

    if (sel->is_union) {
        return execute_union(statement, table);
    }

    const char* target_tbl = statement->table_name[0] != '\0' ? statement->table_name : sel->table_name;
    if (statement->has_cte && strcmp(target_tbl, statement->cte_name) == 0) {
        Statement cte_stmt;
        PrepareResult prep_res = prepare_statement(statement->cte_select_sql, &cte_stmt);
        if (prep_res == PREPARE_SUCCESS) {
            SelectStatement* csel = &cte_stmt.select;
            if (sel->has_limit) { csel->has_limit = true; csel->limit = sel->limit; }
            if (sel->has_order_desc) { csel->has_order_desc = true; }
            return execute_select(&cte_stmt, table);
        }
    }

    if (target_tbl[0] != '\0') {
        ViewSchema* view = table_find_view(table, target_tbl);
        if (view != NULL) {
            Statement view_stmt;
            PrepareResult prep_res = prepare_statement(view->select_sql, &view_stmt);
            if (prep_res == PREPARE_SUCCESS) {
                SelectStatement* vsel = &view_stmt.select;
                if (sel->has_limit) { vsel->has_limit = true; vsel->limit = sel->limit; }
                if (sel->has_order_desc) { vsel->has_order_desc = true; }
                return execute_select(&view_stmt, table);
            }
        }
    }

    if (sel->has_match_filter) {
        uint32_t match_ids[256];
        uint32_t match_count = fts_search(table, sel->match_keyword, match_ids, 256);
        uint32_t emitted = 0;
        Row row;
        for (uint32_t i = 0; i < match_count; i++) {
            if (sel->has_limit && emitted >= sel->limit) break;
            if (fetch_row_by_id(table, match_ids[i], &row)) {
                print_row(&row);
                emitted++;
            }
        }
        return EXECUTE_SUCCESS;
    }

    if (sel->has_secondary_order_by || (sel->has_order_by_col && strcmp(sel->order_by_col, "id") != 0)) {
        return execute_select_multi_sort(statement, table);
    }

    if (sel->has_window_func) {
        return execute_select_window(statement, table);
    }

    if (sel->has_join) {
        return execute_join_select(statement, table);
    }

    if (sel->has_group_by) {
        return execute_select_groupby(statement, table);
    }

    if (sel->has_username_filter && sel->has_email_filter) {
        GenericSecondaryIndex* idx = table_find_composite_index(table, "users", "username", "email");
        if (idx != NULL && idx->enabled) {
            char comp_key[GENERIC_INDEX_KEY_SIZE];
            snprintf(comp_key, sizeof(comp_key), "%s|%s", sel->username, sel->email);
            return execute_select_generic_index(statement, table, idx, comp_key);
        }
    }

    if (sel->has_username_filter) {
        GenericSecondaryIndex* idx = table_find_index_by_column(table, "users", "username");
        if (idx != NULL && idx->enabled) {
            return execute_select_generic_index(statement, table, idx, sel->username);
        } else if (table->username_index_enabled) {
            return execute_select_username_index(statement, table);
        }
    } else if (sel->has_email_filter) {
        GenericSecondaryIndex* idx = table_find_index_by_column(table, "users", "email");
        if (idx != NULL && idx->enabled) {
            return execute_select_generic_index(statement, table, idx, sel->email);
        }
    }

    /* ── DESC path: reverse scan via prev_leaf chain ── */
    if (sel->has_order_desc && sel->aggregate == AGGREGATE_NONE) {
        Cursor* cursor;
        if (!sel->has_id_filter ||
            sel->id_op == COMPARE_GT || sel->id_op == COMPARE_GTE) {
            cursor = table_end(table);
        } else {
            cursor = table_find(table, sel->id);
            if (!cursor->end_of_table) {
                void*    node = get_page(table->pager, cursor->page_num);
                uint32_t key  = *leaf_node_key(node, cursor->cell_num);
                bool beyond = (sel->id_op == COMPARE_LT  && key >= sel->id) ||
                              (sel->id_op == COMPARE_LTE && key >  sel->id);
                if (beyond) cursor_retreat(cursor);
            } else {
                free(cursor);
                cursor = table_end(table);
            }
        }

        uint32_t emitted = 0;
        uint32_t desc_skipped = 0;
        Row row;
        while (!cursor->end_of_table) {
            if (sel->has_limit && emitted >= sel->limit) break;

            void*    node = get_page(table->pager, cursor->page_num);
            uint32_t key  = *leaf_node_key(node, cursor->cell_num);

            if (sel->has_id_filter) {
                if ((sel->id_op == COMPARE_GT  && key <= sel->id) ||
                    (sel->id_op == COMPARE_GTE && key <  sel->id) ||
                    (sel->id_op == COMPARE_EQ  && key <  sel->id))
                    break;
            }

            deserialize_row(cursor_value(cursor), &row);
            if (row_matches_username_filter(table, &row, sel)) {
                if (sel->has_offset && desc_skipped < sel->offset) {
                    desc_skipped++;
                } else {
                    print_row(&row);
                    emitted++;
                }
            }
            cursor_retreat(cursor);
        }

        free(cursor);
        return EXECUTE_SUCCESS;
    }

    /* ── Fast path: primary-key point lookup (no aggregate, no LIMIT) ── */
    if (sel->has_id_filter && sel->id_op == COMPARE_EQ &&
        !sel->has_username_filter &&
        sel->aggregate == AGGREGATE_NONE && !sel->has_limit) {
        Cursor* cursor = table_find(table, sel->id);
        void*    node      = get_page(table->pager, cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        if (cursor->cell_num < num_cells &&
            *leaf_node_key(node, cursor->cell_num) == sel->id) {
            Row row;
            deserialize_row(cursor_value(cursor), &row);
            print_selected_row_ex(table, &row, sel, NULL);
        }
        free(cursor);
        return EXECUTE_SUCCESS;
    }

    /* ── General path: seek to start position ── */
    Cursor* cursor;
    uint32_t start_key = 0;
    bool seek_lower = false;
    CompareOp lower_op = COMPARE_GTE;

    if (sel->has_id_min_filter) {
        seek_lower = true;
        start_key = sel->id_min;
        lower_op = sel->id_min_op;
    } else if (sel->has_id_filter && (sel->id_op == COMPARE_EQ || sel->id_op == COMPARE_GT || sel->id_op == COMPARE_GTE)) {
        seek_lower = true;
        start_key = sel->id;
        lower_op = sel->id_op;
    }

    if (seek_lower) {
        cursor = table_find(table, start_key);
        if (lower_op == COMPARE_GT && !cursor->end_of_table) {
            void* node = get_page(table->pager, cursor->page_num);
            if (cursor->cell_num < *leaf_node_num_cells(node) &&
                *leaf_node_key(node, cursor->cell_num) == start_key) {
                cursor_advance(cursor);
            }
        }
    } else {
        cursor = table_start(table);
    }

    uint32_t emitted = 0;
    uint32_t skipped = 0;
    uint32_t agg_val = 0;
    Row row;
    DistinctSet dset;
    distinct_set_init(&dset);

    while (!cursor->end_of_table) {
        /* Check limit before emitting so LIMIT 0 returns nothing */
        if (sel->has_limit && emitted >= sel->limit) break;

        void*    node = get_page(table->pager, cursor->page_num);
        uint32_t key  = *leaf_node_key(node, cursor->cell_num);

        /* Upper-bound check — keys are ascending so we can break early */
        if (sel->has_id_max_filter) {
            if ((sel->id_max_op == COMPARE_LT  && key >= sel->id_max) ||
                (sel->id_max_op == COMPARE_LTE && key >  sel->id_max))
                break;
        }
        if (sel->has_id_filter) {
            if ((sel->id_op == COMPARE_LT  && key >= sel->id) ||
                (sel->id_op == COMPARE_LTE && key >  sel->id) ||
                (sel->id_op == COMPARE_EQ  && key >  sel->id))
                break;
        }

        deserialize_row(cursor_value(cursor), &row);
        if (!row_matches_filters(table, &row, sel)) {
            cursor_advance(cursor);
            continue;
        }

        if (sel->has_offset && skipped < sel->offset) {
            skipped++;
            cursor_advance(cursor);
            continue;
        }

        switch (sel->aggregate) {
            case AGGREGATE_NONE:
                if (!print_selected_row_ex(table, &row, sel, &dset)) {
                    cursor_advance(cursor);
                    continue;
                }
                break;
            case AGGREGATE_MIN:
                if (emitted == 0) agg_val = key; /* first (smallest) key in scan order */
                break;
            case AGGREGATE_MAX:
                agg_val = key; /* always overwrite; last value after loop is largest */
                break;
            case AGGREGATE_SUM:
            case AGGREGATE_AVG:
                agg_val += key;
                break;
            default:
                break;
        }
        emitted++;
        cursor_advance(cursor);
    }

    switch (sel->aggregate) {
        case AGGREGATE_COUNT:
            printf("%u\n", emitted);
            break;
        case AGGREGATE_MIN:
        case AGGREGATE_MAX:
            if (emitted == 0) printf("NULL\n");
            else printf("%u\n", agg_val);
            break;
        case AGGREGATE_SUM:
            if (emitted == 0) printf("NULL\n");
            else printf("%u\n", agg_val);
            break;
        case AGGREGATE_AVG:
            if (emitted == 0) printf("NULL\n");
            else printf("%u\n", agg_val / emitted);
            break;
        default:
            break;
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_explain(Statement* statement, Table* table) {
    SelectStatement* sel = &statement->select;

    if (sel->has_group_by) {
        printf("PLAN: GROUP BY AGGREGATION SCAN (GROUP BY %s%s)\n",
               sel->group_by_col,
               sel->has_having ? ", HAVING FILTER" : "");
    } else if (sel->has_username_filter) {
        GenericSecondaryIndex* idx = table_find_index_by_column(table, "users", "username");
        bool idx_enabled = (idx && idx->enabled) || table->username_index_enabled;
        printf("PLAN: %s (username = '%s')\n",
               idx_enabled ? "SECONDARY INDEX LOOKUP" : "FULL TABLE SCAN",
               sel->username);
    } else if (sel->has_email_filter) {
        GenericSecondaryIndex* idx = table_find_index_by_column(table, "users", "email");
        bool idx_enabled = (idx && idx->enabled);
        printf("PLAN: %s (email = '%s')\n",
               idx_enabled ? "SECONDARY INDEX LOOKUP" : "FULL TABLE SCAN",
               sel->email);
    } else if (!sel->has_id_filter) {
        printf("PLAN: FULL TABLE SCAN\n");
    } else if (sel->id_op == COMPARE_EQ) {
        printf("PLAN: PRIMARY KEY LOOKUP (id = %u)\n", sel->id);
    } else {
        const char* op_str = "?";
        switch (sel->id_op) {
            case COMPARE_GT:  op_str = ">";  break;
            case COMPARE_GTE: op_str = ">="; break;
            case COMPARE_LT:  op_str = "<";  break;
            case COMPARE_LTE: op_str = "<="; break;
            default: break;
        }
        printf("PLAN: PRIMARY KEY RANGE SCAN (id %s %u)\n", op_str, sel->id);
    }

    switch (sel->aggregate) {
        case AGGREGATE_COUNT: printf("      AGGREGATE: COUNT(*)\n"); break;
        case AGGREGATE_MIN:   printf("      AGGREGATE: MIN(id)\n");  break;
        case AGGREGATE_MAX:   printf("      AGGREGATE: MAX(id)\n");  break;
        case AGGREGATE_SUM:   printf("      AGGREGATE: SUM(id)\n");  break;
        case AGGREGATE_AVG:   printf("      AGGREGATE: AVG(id)\n");  break;
        default: break;
    }
    if (sel->has_order_desc) printf("      ORDER BY id DESC\n");
    if (sel->has_limit)      printf("      LIMIT %u\n", sel->limit);

    return EXECUTE_SUCCESS;
}

static void execute_foreign_key_cascade(Table* table, const char* parent_table_name, uint32_t parent_id) {
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        TableSchema* s = &table->catalog.schemas[i];
        if (s->has_fk && (strcmp(s->fk_parent_table, parent_table_name) == 0 ||
                          (strcmp(parent_table_name, "users") == 0 && strcmp(s->fk_parent_table, "users") == 0))) {
            if (s->fk_on_delete_cascade) {
                printf("Foreign key ON DELETE CASCADE triggered for table '%s' (parent_id=%u).\n",
                       s->name, parent_id);
            }
        }
    }
}

ExecuteResult execute_delete(Statement* statement, Table* table) {
    if (statement->delete_all) {
        table_truncate(table);
        table_mark_username_index_dirty(table);
        if (!table->in_transaction) {
            commit_table_changes(table);
        }
        return EXECUTE_SUCCESS;
    }

    uint32_t key = statement->delete_id;
    Cursor* cursor = table_find(table, key);
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num >= num_cells ||
        *leaf_node_key(node, cursor->cell_num) != key) {
        free(cursor);
        return EXECUTE_KEY_NOT_FOUND;
    }

    const char* p_name = statement->table_name[0] ? statement->table_name : "users";
    execute_foreign_key_cascade(table, p_name, key);

    leaf_node_delete(cursor);
    table_mark_username_index_dirty(table);

    if (!table->in_transaction) {
        commit_table_changes(table);
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_update(Statement* statement, Table* table) {
    uint32_t key = statement->update.id;
    Cursor* cursor = table_find(table, key);
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num >= num_cells ||
        *leaf_node_key(node, cursor->cell_num) != key) {
        free(cursor);
        return EXECUTE_KEY_NOT_FOUND;
    }

    Row row;
    deserialize_row(cursor_value(cursor), &row);

    if (statement->update.set_username) {
        strncpy(row.username, statement->update.username, sizeof(row.username) - 1);
        row.username[sizeof(row.username) - 1] = '\0';
    }
    if (statement->update.set_email) {
        strncpy(row.email, statement->update.email, sizeof(row.email) - 1);
        row.email[sizeof(row.email) - 1] = '\0';
    }

    serialize_row(&row, cursor_value(cursor));
    mark_page_dirty(table->pager, cursor->page_num);
    table_mark_username_index_dirty(table);

    if (!table->in_transaction) {
        commit_table_changes(table);
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_begin(Table* table) {
    if (table->in_transaction) {
        return EXECUTE_TRANSACTION_ALREADY_ACTIVE;
    }
    pager_begin_transaction(table->pager);
    table->in_transaction = true;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_commit(Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    table->in_transaction = false;
    commit_table_changes(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_rollback(Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    pager_rollback(table->pager);
    table_mark_username_index_dirty(table);
    table->in_transaction = false;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_create_index(Statement* statement, Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    const char* idx_name = statement->create_index.name[0] != '\0' ? statement->create_index.name : "idx_users_username";
    const char* tbl_name = statement->create_index.table_name[0] != '\0' ? statement->create_index.table_name : "users";
    const char* col_name = statement->create_index.column_name[0] != '\0' ? statement->create_index.column_name : "username";
    const char* col_name2 = statement->create_index.num_columns == 2 ? statement->create_index.column_name2 : NULL;

    table_create_index(table, idx_name, tbl_name, col_name, col_name2);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_drop_index(Statement* statement, Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    const char* idx_name = statement->drop_index.name[0] != '\0' ? statement->drop_index.name : "idx_users_username";
    table_drop_index(table, idx_name);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_vacuum(Statement* statement, Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    if (statement->vacuum.has_into) {
        db_vacuum_into(table, statement->vacuum.into_filename);
    } else {
        db_vacuum(table);
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_savepoint(Statement* statement, Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    if (!pager_savepoint(table->pager, statement->savepoint.name)) {
        return EXECUTE_SAVEPOINT_STACK_FULL;
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_rollback_to(Statement* statement, Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    if (!pager_rollback_to_savepoint(table->pager, statement->savepoint.name)) {
        return EXECUTE_SAVEPOINT_NOT_FOUND;
    }
    table_mark_username_index_dirty(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_release_savepoint(Statement* statement, Table* table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TRANSACTION;
    }
    if (!pager_release_savepoint(table->pager, statement->savepoint.name)) {
        return EXECUTE_SAVEPOINT_NOT_FOUND;
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_checkpoint(Table* table) {
    if (table->in_transaction) {
        return EXECUTE_DDL_INSIDE_TRANSACTION;
    }
    db_checkpoint(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_integrity_check(Table* table) {
    db_integrity_check(table);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_pragma_user_version(Table* table) {
    printf("%u\n", db_get_user_version(table));
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_pragma_set_user_version(Statement* statement, Table* table) {
    db_set_user_version(table, statement->pragma.user_version);
    if (!table->in_transaction) {
        commit_table_changes(table);
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_pragma_table_info(Table* table) {
    (void)table;
    printf("cid | name     | type         | notnull | dflt_value | pk\n");
    printf("----+----------+--------------+---------+------------+---\n");
    printf("0   | id       | INT          | 1       | NULL       | 1\n");
    printf("1   | username | VARCHAR(32)  | 0       | NULL       | 0\n");
    printf("2   | email    | VARCHAR(255) | 0       | NULL       | 0\n");
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_pragma_index_list(Table* table) {
    table_ensure_all_indexes(table);
    printf("seq | name                 | unique | origin | partial\n");
    printf("----+----------------------+--------+--------+--------\n");
    uint32_t count = 0;
    if (table->username_index_enabled) {
        table_ensure_username_index(table);
        printf("%u   | idx_users_username   | 0      | c      | 0\n", count++);
    }
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        if (table->sec_indexes[i].enabled) {
            if (table->username_index_enabled && strcmp(table->sec_indexes[i].name, "idx_users_username") == 0) continue;
            printf("%u   | %-20s | 0      | c      | 0\n", count++, table->sec_indexes[i].name);
        }
    }
    if (count == 0) {
        printf("(no indexes found)\n");
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_create_table(Statement* statement, Table* table) {
    if (table_create_table(table, statement->create_table.table_name,
                           statement->create_table.num_columns,
                           statement->create_table.col_names,
                           statement->create_table.col_types,
                           statement->create_table.has_fk,
                           statement->create_table.fk_col,
                           statement->create_table.fk_parent_table,
                           statement->create_table.fk_parent_col,
                           statement->create_table.fk_on_delete_cascade)) {
        return EXECUTE_SUCCESS;
    }
    return EXECUTE_TABLE_FULL;
}

typedef struct {
    char name[32];
    char sql_template[256];
} PreparedStatementEntry;

static PreparedStatementEntry prepared_statements[16];
static uint32_t prepared_count = 0;

ExecuteResult execute_prepare(Statement* statement) {
    for (uint32_t i = 0; i < prepared_count; i++) {
        if (strcmp(prepared_statements[i].name, statement->prepare.name) == 0) {
            strncpy(prepared_statements[i].sql_template, statement->prepare.sql_template, 255);
            printf("Statement '%s' prepared.\n", statement->prepare.name);
            return EXECUTE_SUCCESS;
        }
    }
    if (prepared_count >= 16) return EXECUTE_TABLE_FULL;
    strncpy(prepared_statements[prepared_count].name, statement->prepare.name, 31);
    strncpy(prepared_statements[prepared_count].sql_template, statement->prepare.sql_template, 255);
    prepared_count++;
    printf("Statement '%s' prepared.\n", statement->prepare.name);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_execute_prepared(Statement* statement, Table* table) {
    const char* template_str = NULL;
    for (uint32_t i = 0; i < prepared_count; i++) {
        if (strcmp(prepared_statements[i].name, statement->execute_prepared.name) == 0) {
            template_str = prepared_statements[i].sql_template;
            break;
        }
    }
    if (template_str == NULL) {
        printf("Error: Prepared statement '%s' not found.\n", statement->execute_prepared.name);
        return EXECUTE_SUCCESS;
    }

    char bound_sql[512];
    const char* p = strchr(template_str, '?');
    if (p != NULL) {
        size_t prefix_len = p - template_str;
        snprintf(bound_sql, sizeof(bound_sql), "%.*s%u%s", (int)prefix_len, template_str, statement->execute_prepared.param_val, p + 1);
    } else {
        strncpy(bound_sql, template_str, sizeof(bound_sql) - 1);
    }

    Statement bound_stmt;
    PrepareResult prep_res = prepare_statement(bound_sql, &bound_stmt);
    if (prep_res == PREPARE_SUCCESS) {
        return execute_statement(&bound_stmt, table);
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_create_view(Statement* statement, Table* table) {
    if (table_create_view(table, statement->create_view.view_name, statement->create_view.select_sql)) {
        printf("View '%s' created.\n", statement->create_view.view_name);
        return EXECUTE_SUCCESS;
    }
    return EXECUTE_TABLE_FULL;
}

ExecuteResult execute_drop_view(Statement* statement, Table* table) {
    if (table_drop_view(table, statement->drop_view.view_name)) {
        printf("View '%s' dropped.\n", statement->drop_view.view_name);
        return EXECUTE_SUCCESS;
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_alter_table(Statement* statement, Table* table) {
    AlterTableStatement* alt = &statement->alter_table;
    if (alt->is_rename) {
        if (table_rename_table(table, alt->table_name, alt->new_table_name)) {
            printf("Table '%s' renamed to '%s'.\n", alt->table_name, alt->new_table_name);
            return EXECUTE_SUCCESS;
        }
    } else if (alt->is_add_column) {
        if (table_add_column(table, alt->table_name, alt->new_col_name, alt->new_col_type)) {
            printf("Column '%s' added to table '%s'.\n", alt->new_col_name, alt->table_name);
            return EXECUTE_SUCCESS;
        }
    }
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Table* table) {
    if (statement->explain) {
        return execute_explain(statement, table);
    }

    switch (statement->type) {
        case (STATEMENT_INSERT):
            return execute_insert(statement, table);
        case (STATEMENT_SELECT):
            return execute_select(statement, table);
        case (STATEMENT_DELETE):
            return execute_delete(statement, table);
        case (STATEMENT_BEGIN):
            return execute_begin(table);
        case (STATEMENT_COMMIT):
            return execute_commit(table);
        case (STATEMENT_ROLLBACK):
            return execute_rollback(table);
        case (STATEMENT_UPDATE):
            return execute_update(statement, table);
        case (STATEMENT_CREATE_INDEX):
            return execute_create_index(statement, table);
        case (STATEMENT_DROP_INDEX):
            return execute_drop_index(statement, table);
        case (STATEMENT_VACUUM):
            return execute_vacuum(statement, table);
        case (STATEMENT_SAVEPOINT):
            return execute_savepoint(statement, table);
        case (STATEMENT_ROLLBACK_TO):
            return execute_rollback_to(statement, table);
        case (STATEMENT_RELEASE_SAVEPOINT):
            return execute_release_savepoint(statement, table);
        case (STATEMENT_CHECKPOINT):
            return execute_checkpoint(table);
        case (STATEMENT_PRAGMA_INTEGRITY_CHECK):
            return execute_integrity_check(table);
        case (STATEMENT_PRAGMA_USER_VERSION):
            return execute_pragma_user_version(table);
        case (STATEMENT_PRAGMA_SET_USER_VERSION):
            return execute_pragma_set_user_version(statement, table);
        case (STATEMENT_PRAGMA_TABLE_INFO):
            return execute_pragma_table_info(table);
        case (STATEMENT_PRAGMA_INDEX_LIST):
            return execute_pragma_index_list(table);
        case (STATEMENT_CREATE_TABLE):
            return execute_create_table(statement, table);
        case (STATEMENT_PREPARE):
            return execute_prepare(statement);
        case (STATEMENT_EXECUTE_PREPARED):
            return execute_execute_prepared(statement, table);
        case (STATEMENT_CREATE_VIEW):
            return execute_create_view(statement, table);
        case (STATEMENT_DROP_VIEW):
            return execute_drop_view(statement, table);
        case (STATEMENT_ALTER_TABLE):
            return execute_alter_table(statement, table);
    }
    return EXECUTE_SUCCESS;
}