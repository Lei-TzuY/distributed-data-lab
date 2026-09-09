#include "compiler.h"

#include <ctype.h>
#include <errno.h>

typedef struct {
    const char* current;
} Parser;

static void skip_spaces(Parser* parser) {
    while (isspace((unsigned char)*parser->current)) {
        parser->current++;
    }
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

static bool consume_keyword(Parser* parser, const char* keyword) {
    const char* input;
    const char* expected;

    skip_spaces(parser);
    input = parser->current;
    expected = keyword;

    while (*expected != '\0' &&
           tolower((unsigned char)*input) == tolower((unsigned char)*expected)) {
        input++;
        expected++;
    }

    if (*expected != '\0' || is_identifier_char(*input)) {
        return false;
    }

    parser->current = input;
    return true;
}

static bool consume_character(Parser* parser, char expected) {
    skip_spaces(parser);
    if (*parser->current != expected) {
        return false;
    }

    parser->current++;
    return true;
}

static bool consume_statement_end(Parser* parser) {
    skip_spaces(parser);
    if (*parser->current == ';') {
        parser->current++;
        skip_spaces(parser);
    }
    return *parser->current == '\0';
}

static bool check_query_end(Parser* parser) {
    skip_spaces(parser);
    if (*parser->current == ')') return true;
    return consume_statement_end(parser);
}

static bool parse_identifier(Parser* parser) {
    skip_spaces(parser);
    if (!isalpha((unsigned char)*parser->current) && *parser->current != '_') {
        return false;
    }

    while (is_identifier_char(*parser->current)) {
        parser->current++;
    }
    return true;
}

static bool parse_identifier_into(Parser* parser,
                                  char* destination,
                                  size_t capacity) {
    const char* start;
    size_t length;

    skip_spaces(parser);
    if (!isalpha((unsigned char)*parser->current) && *parser->current != '_') {
        return false;
    }

    start = parser->current;
    while (is_identifier_char(*parser->current)) {
        parser->current++;
    }

    length = (size_t)(parser->current - start);
    if (length == 0 || length >= capacity) {
        return false;
    }

    memcpy(destination, start, length);
    destination[length] = '\0';
    return true;
}

static bool parse_uint32(Parser* parser, uint32_t* value) {
    char* end;
    unsigned long long parsed;

    skip_spaces(parser);
    if (!isdigit((unsigned char)*parser->current)) {
        return false;
    }

    errno = 0;
    parsed = strtoull(parser->current, &end, 10);
    if (errno == ERANGE || parsed > UINT32_MAX) {
        return false;
    }

    parser->current = end;
    *value = (uint32_t)parsed;
    return true;
}

static bool consume_compare_op(Parser* parser, CompareOp* op) {
    skip_spaces(parser);
    /* Check two-character operators first so >= isn't parsed as > */
    if (parser->current[0] == '>' && parser->current[1] == '=') {
        *op = COMPARE_GTE; parser->current += 2; return true;
    }
    if (parser->current[0] == '<' && parser->current[1] == '=') {
        *op = COMPARE_LTE; parser->current += 2; return true;
    }
    if (parser->current[0] == '>') { *op = COMPARE_GT; parser->current += 1; return true; }
    if (parser->current[0] == '<') { *op = COMPARE_LT; parser->current += 1; return true; }
    if (parser->current[0] == '=') { *op = COMPARE_EQ; parser->current += 1; return true; }
    return false;
}

static bool parse_token(Parser* parser, char* destination, size_t capacity) {
    const char* start;
    size_t length;

    skip_spaces(parser);
    start = parser->current;
    while (*parser->current != '\0' &&
           !isspace((unsigned char)*parser->current) &&
           *parser->current != ';') {
        parser->current++;
    }

    length = (size_t)(parser->current - start);
    if (length == 0 || length >= capacity) {
        return false;
    }

    memcpy(destination, start, length);
    destination[length] = '\0';
    return true;
}

static bool parse_sql_string(Parser* parser, char* destination, size_t capacity) {
    size_t length = 0;

    skip_spaces(parser);
    if (*parser->current != '\'') {
        return false;
    }
    parser->current++;

    while (*parser->current != '\0') {
        char value = *parser->current++;
        if (value == '\'') {
            if (*parser->current == '\'') {
                parser->current++;
                value = '\'';
            } else {
                destination[length] = '\0';
                return true;
            }
        }

        if (length + 1 >= capacity) {
            return false;
        }
        destination[length++] = value;
    }

    return false;
}

static bool parse_legacy_insert(Parser* parser, Statement* statement) {
    Parser backup = *parser;
    if (parse_uint32(parser, &statement->row_to_insert.id) &&
        parse_token(parser, statement->row_to_insert.username,
                    sizeof(statement->row_to_insert.username)) &&
        parse_token(parser, statement->row_to_insert.email,
                    sizeof(statement->row_to_insert.email)) &&
        consume_statement_end(parser)) {
        return true;
    }

    *parser = backup;
    statement->is_auto_id = true;
    return parse_token(parser, statement->row_to_insert.username,
                       sizeof(statement->row_to_insert.username)) &&
           parse_token(parser, statement->row_to_insert.email,
                       sizeof(statement->row_to_insert.email)) &&
           consume_statement_end(parser);
}

static bool parse_sql_insert(Parser* parser, Statement* statement) {
    return parse_identifier(parser) &&
           consume_keyword(parser, "values") &&
           consume_character(parser, '(') &&
           parse_uint32(parser, &statement->row_to_insert.id) &&
           consume_character(parser, ',') &&
           parse_sql_string(parser, statement->row_to_insert.username,
                            sizeof(statement->row_to_insert.username)) &&
           consume_character(parser, ',') &&
           parse_sql_string(parser, statement->row_to_insert.email,
                            sizeof(statement->row_to_insert.email)) &&
           consume_character(parser, ')') &&
           consume_statement_end(parser);
}

static bool parse_insert(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_INSERT;
    if (consume_keyword(parser, "into")) {
        return parse_sql_insert(parser, statement);
    }
    return parse_legacy_insert(parser, statement);
}

static bool parse_delete(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_DELETE;
    if (!consume_keyword(parser, "from") || !parse_identifier(parser)) {
        return false;
    }
    if (consume_statement_end(parser)) {
        statement->delete_all = true;
        return true;
    }
    return consume_keyword(parser, "where") &&
           consume_keyword(parser, "id") &&
           consume_character(parser, '=') &&
           parse_uint32(parser, &statement->delete_id) &&
           consume_statement_end(parser);
}

static bool parse_update_set_clause(Parser* parser, Statement* statement) {
    bool parsed_any = false;
    while (true) {
        skip_spaces(parser);
        if (consume_keyword(parser, "username")) {
            if (!consume_character(parser, '=')) return false;
            if (!parse_sql_string(parser, statement->update.username,
                                  sizeof(statement->update.username))) return false;
            statement->update.set_username = true;
        } else if (consume_keyword(parser, "email")) {
            if (!consume_character(parser, '=')) return false;
            if (!parse_sql_string(parser, statement->update.email,
                                  sizeof(statement->update.email))) return false;
            statement->update.set_email = true;
        } else {
            break;
        }
        parsed_any = true;
        skip_spaces(parser);
        if (*parser->current != ',') break;
        parser->current++;
    }
    return parsed_any;
}

static bool parse_update(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_UPDATE;
    return parse_identifier(parser) &&
           consume_keyword(parser, "set") &&
           parse_update_set_clause(parser, statement) &&
           consume_keyword(parser, "where") &&
           consume_keyword(parser, "id") &&
           consume_character(parser, '=') &&
           parse_uint32(parser, &statement->update.id) &&
           consume_statement_end(parser);
}

static bool parse_select(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_SELECT;

    if (consume_keyword(parser, "distinct")) {
        statement->select.is_distinct = true;
    }

    /* Bare "select" with nothing following */
    if (consume_statement_end(parser)) return true;

    /* Optional column projection before aggregate: e.g. "email, count(*)" */
    Parser peek = *parser;
    char first_col[32];
    if (parse_identifier_into(&peek, first_col, sizeof(first_col))) {
        if (consume_character(&peek, ',')) {
            statement->select.has_project_col = true;
            strncpy(statement->select.project_col, first_col, sizeof(statement->select.project_col) - 1);
            *parser = peek;
        }
    }

    /* Projection: COUNT(*), MIN(id), MAX(id), SUM(id), AVG(id), or * */
    if (consume_keyword(parser, "count")) {
        if (!consume_character(parser, '(') ||
            !consume_character(parser, '*') ||
            !consume_character(parser, ')'))
            return false;
        statement->select.aggregate = AGGREGATE_COUNT;
    } else if (consume_keyword(parser, "min")) {
        if (!consume_character(parser, '(') ||
            !consume_keyword(parser, "id") ||
            !consume_character(parser, ')'))
            return false;
        statement->select.aggregate = AGGREGATE_MIN;
    } else if (consume_keyword(parser, "max")) {
        if (!consume_character(parser, '(') ||
            !consume_keyword(parser, "id") ||
            !consume_character(parser, ')'))
            return false;
        statement->select.aggregate = AGGREGATE_MAX;
    } else if (consume_keyword(parser, "sum")) {
        if (!consume_character(parser, '(') ||
            !consume_keyword(parser, "id") ||
            !consume_character(parser, ')'))
            return false;
        statement->select.aggregate = AGGREGATE_SUM;
    } else if (consume_keyword(parser, "avg")) {
        if (!consume_character(parser, '(') ||
            !consume_keyword(parser, "id") ||
            !consume_character(parser, ')'))
            return false;
        statement->select.aggregate = AGGREGATE_AVG;
    } else if (consume_keyword(parser, "row_number")) {
        if (!consume_character(parser, '(') || !consume_character(parser, ')')) return false;
        statement->select.has_window_func = true;
        strncpy(statement->select.window_func_name, "row_number", sizeof(statement->select.window_func_name) - 1);
        if (consume_keyword(parser, "over")) {
            if (!consume_character(parser, '(')) return false;
            if (consume_keyword(parser, "partition")) {
                if (!consume_keyword(parser, "by")) return false;
                if (!parse_identifier_into(parser, statement->select.partition_col, sizeof(statement->select.partition_col))) return false;
                statement->select.has_partition_by = true;
            }
            if (consume_keyword(parser, "order")) {
                if (!consume_keyword(parser, "by")) return false;
                if (!parse_identifier_into(parser, statement->select.window_order_col, sizeof(statement->select.window_order_col))) return false;
                statement->select.has_window_order_by = true;
            }
            if (!consume_character(parser, ')')) return false;
        }
    } else if (consume_keyword(parser, "length")) {
        if (!consume_character(parser, '(') ||
            !parse_identifier_into(parser, statement->select.str_func_target_col, sizeof(statement->select.str_func_target_col)) ||
            !consume_character(parser, ')')) return false;
        statement->select.str_func = STRING_FUNC_LENGTH;
    } else if (consume_keyword(parser, "upper")) {
        if (!consume_character(parser, '(') ||
            !parse_identifier_into(parser, statement->select.str_func_target_col, sizeof(statement->select.str_func_target_col)) ||
            !consume_character(parser, ')')) return false;
        statement->select.str_func = STRING_FUNC_UPPER;
    } else if (consume_keyword(parser, "lower")) {
        if (!consume_character(parser, '(') ||
            !parse_identifier_into(parser, statement->select.str_func_target_col, sizeof(statement->select.str_func_target_col)) ||
            !consume_character(parser, ')')) return false;
        statement->select.str_func = STRING_FUNC_LOWER;
    } else if (consume_keyword(parser, "concat")) {
        if (!consume_character(parser, '(') ||
            !parse_identifier_into(parser, statement->select.str_func_target_col, sizeof(statement->select.str_func_target_col)) ||
            !consume_character(parser, ',') ||
            !parse_identifier_into(parser, statement->select.str_func_second_col, sizeof(statement->select.str_func_second_col)) ||
            !consume_character(parser, ')')) return false;
        statement->select.str_func = STRING_FUNC_CONCAT;
    } else if (consume_keyword(parser, "version")) {
        if (!consume_character(parser, '(') || !consume_character(parser, ')')) return false;
        statement->select.sys_func = SYS_FUNC_VERSION;
    } else if (consume_keyword(parser, "database")) {
        if (!consume_character(parser, '(') || !consume_character(parser, ')')) return false;
        statement->select.sys_func = SYS_FUNC_DATABASE;
    } else if (consume_keyword(parser, "abs")) {
        if (!consume_character(parser, '(') ||
            !parse_identifier_into(parser, statement->select.math_target_col, sizeof(statement->select.math_target_col)) ||
            !consume_character(parser, ')')) return false;
        statement->select.math_func = MATH_FUNC_ABS;
    } else if (consume_keyword(parser, "mod")) {
        if (!consume_character(parser, '(') ||
            !parse_identifier_into(parser, statement->select.math_target_col, sizeof(statement->select.math_target_col)) ||
            !consume_character(parser, ',') ||
            !parse_uint32(parser, &statement->select.math_operand) ||
            !consume_character(parser, ')')) return false;
        statement->select.math_func = MATH_FUNC_MOD;
    } else {
        if (!consume_character(parser, '*')) {
            if (consume_character(parser, '(')) {
                skip_spaces(parser);
                const char* sub_start = parser->current;
                int paren_count = 1;
                while (*parser->current != '\0' && paren_count > 0) {
                    if (*parser->current == '(') paren_count++;
                    else if (*parser->current == ')') paren_count--;
                    if (paren_count > 0) parser->current++;
                }
                size_t sub_len = parser->current - sub_start;
                if (sub_len >= sizeof(statement->select.scalar_subquery_sql)) return false;
                memcpy(statement->select.scalar_subquery_sql, sub_start, sub_len);
                statement->select.scalar_subquery_sql[sub_len] = '\0';
                statement->select.has_scalar_subquery = true;
                if (!consume_character(parser, ')')) return false;
            } else if (!parse_identifier_into(parser, statement->select.project_col, sizeof(statement->select.project_col))) {
                return false;
            } else {
                statement->select.has_project_col = true;
                if (consume_character(parser, ',')) {
                    if (consume_character(parser, '(')) {
                        skip_spaces(parser);
                        const char* sub_start = parser->current;
                        int paren_count = 1;
                        while (*parser->current != '\0' && paren_count > 0) {
                            if (*parser->current == '(') paren_count++;
                            else if (*parser->current == ')') paren_count--;
                            if (paren_count > 0) parser->current++;
                        }
                        size_t sub_len = parser->current - sub_start;
                        if (sub_len >= sizeof(statement->select.scalar_subquery_sql)) return false;
                        memcpy(statement->select.scalar_subquery_sql, sub_start, sub_len);
                        statement->select.scalar_subquery_sql[sub_len] = '\0';
                        statement->select.has_scalar_subquery = true;
                        if (!consume_character(parser, ')')) return false;
                    }
                }
            }
        }
    }

    if (statement->select.sys_func != SYS_FUNC_NONE) {
        if (consume_keyword(parser, "from")) {
            skip_spaces(parser);
            const char* tstart = parser->current;
            while (isalnum((unsigned char)*parser->current) || *parser->current == '_') {
                parser->current++;
            }
            size_t tlen = parser->current - tstart;
            if (tlen > 0 && tlen < sizeof(statement->select.table_name)) {
                memcpy(statement->select.table_name, tstart, tlen);
                statement->select.table_name[tlen] = '\0';
                memcpy(statement->table_name, tstart, tlen);
                statement->table_name[tlen] = '\0';
            }
        }
        return check_query_end(parser);
    }

    if (!consume_keyword(parser, "from")) return false;
    skip_spaces(parser);
    const char* tstart = parser->current;
    while (isalnum((unsigned char)*parser->current) || *parser->current == '_') {
        parser->current++;
    }
    size_t tlen = parser->current - tstart;
    if (tlen == 0 || tlen >= sizeof(statement->select.table_name)) return false;
    memcpy(statement->select.table_name, tstart, tlen);
    statement->select.table_name[tlen] = '\0';
    memcpy(statement->table_name, tstart, tlen);
    statement->table_name[tlen] = '\0';

    if (strcmp(statement->select.table_name, "sqlite_master") == 0 ||
        strcmp(statement->select.table_name, "sqlite_schema") == 0 ||
        strcmp(statement->select.table_name, "information_schema") == 0) {
        statement->select.is_catalog_query = true;
    }

    if (consume_keyword(parser, "join")) {
        skip_spaces(parser);
        const char* jstart = parser->current;
        while (isalnum((unsigned char)*parser->current) || *parser->current == '_') {
            parser->current++;
        }
        size_t jlen = parser->current - jstart;
        if (jlen == 0 || jlen >= sizeof(statement->select.join_table)) return false;
        memcpy(statement->select.join_table, jstart, jlen);
        statement->select.join_table[jlen] = '\0';
        statement->select.has_join = true;

        if (consume_keyword(parser, "on")) {
            skip_spaces(parser);
            const char* lstart = parser->current;
            while (isalnum((unsigned char)*parser->current) || *parser->current == '_' || *parser->current == '.') {
                parser->current++;
            }
            size_t llen = parser->current - lstart;
            if (llen > 0 && llen < sizeof(statement->select.join_left_col)) {
                memcpy(statement->select.join_left_col, lstart, llen);
                statement->select.join_left_col[llen] = '\0';
            }

            if (consume_character(parser, '=')) {
                skip_spaces(parser);
                const char* rstart = parser->current;
                while (isalnum((unsigned char)*parser->current) || *parser->current == '_' || *parser->current == '.') {
                    parser->current++;
                }
                size_t rlen = parser->current - rstart;
                if (rlen > 0 && rlen < sizeof(statement->select.join_right_col)) {
                    memcpy(statement->select.join_right_col, rstart, rlen);
                    statement->select.join_right_col[rlen] = '\0';
                }
            }
        }
    }

    if (check_query_end(parser)) return true;

    /* Optional WHERE clause: id <op> N, username = 'val', or email = 'val' (supports AND chaining) */
    if (consume_keyword(parser, "where")) {
        while (true) {
            if (consume_keyword(parser, "exists")) {
                if (!consume_character(parser, '(')) return false;
                if (!consume_keyword(parser, "select")) return false;
                Statement sub_stmt;
                memset(&sub_stmt, 0, sizeof(sub_stmt));
                if (!parse_select(parser, &sub_stmt)) return false;
                if (!consume_character(parser, ')')) return false;

                SelectStatement* heap_sub = (SelectStatement*)calloc(1, sizeof(SelectStatement));
                *heap_sub = sub_stmt.select;
                statement->select.has_exists_subquery = true;
                statement->select.exists_subquery = heap_sub;
            } else if (consume_keyword(parser, "id")) {
                if (consume_keyword(parser, "between")) {
                    uint32_t min_val, max_val;
                    if (!parse_uint32(parser, &min_val)) return false;
                    if (!consume_keyword(parser, "and")) return false;
                    if (!parse_uint32(parser, &max_val)) return false;
                    statement->select.has_between_filter = true;
                    statement->select.between_min = min_val;
                    statement->select.between_max = max_val;
                } else if (consume_keyword(parser, "not")) {
                    if (!consume_keyword(parser, "in")) return false;
                    if (!consume_character(parser, '(')) return false;
                    uint32_t count = 0;
                    while (count < 32) {
                        uint32_t val;
                        if (!parse_uint32(parser, &val)) return false;
                        statement->select.in_list_ids[count++] = val;
                        if (consume_character(parser, ',')) continue;
                        break;
                    }
                    if (!consume_character(parser, ')')) return false;
                    statement->select.has_in_list = true;
                    statement->select.in_list_count = count;
                    statement->select.is_not_in_list = true;
                } else if (consume_keyword(parser, "in")) {
                    if (!consume_character(parser, '(')) return false;
                    if (consume_keyword(parser, "select")) {
                        Statement sub_stmt;
                        memset(&sub_stmt, 0, sizeof(sub_stmt));
                        if (!parse_select(parser, &sub_stmt)) return false;
                        if (!consume_character(parser, ')')) return false;

                        SelectStatement* heap_sub = (SelectStatement*)calloc(1, sizeof(SelectStatement));
                        *heap_sub = sub_stmt.select;
                        statement->select.has_in_subquery = true;
                        statement->select.in_subquery = heap_sub;
                    } else {
                        uint32_t count = 0;
                        while (count < 32) {
                            uint32_t val;
                            if (!parse_uint32(parser, &val)) return false;
                            statement->select.in_list_ids[count++] = val;
                            if (consume_character(parser, ',')) continue;
                            break;
                        }
                        if (!consume_character(parser, ')')) return false;
                        statement->select.has_in_list = true;
                        statement->select.in_list_count = count;
                    }
                } else {
                    CompareOp op;
                    uint32_t id_val;
                    if (!consume_compare_op(parser, &op)) return false;
                    if (!parse_uint32(parser, &id_val)) return false;
                    statement->select.has_id_filter = true;
                    statement->select.id = id_val;
                    statement->select.id_op = op;

                    if (op == COMPARE_GT || op == COMPARE_GTE) {
                        statement->select.has_id_min_filter = true;
                        statement->select.id_min = id_val;
                        statement->select.id_min_op = op;
                    } else if (op == COMPARE_LT || op == COMPARE_LTE) {
                        statement->select.has_id_max_filter = true;
                        statement->select.id_max = id_val;
                        statement->select.id_max_op = op;
                    }
                }
            } else if (consume_keyword(parser, "match")) {
                if (!parse_sql_string(parser, statement->select.match_keyword,
                                      sizeof(statement->select.match_keyword))) return false;
                statement->select.has_match_filter = true;
            } else if (consume_keyword(parser, "username")) {
                if (consume_keyword(parser, "is")) {
                    strncpy(statement->select.null_target_col, "username", sizeof(statement->select.null_target_col) - 1);
                    if (consume_keyword(parser, "not")) {
                        if (!consume_keyword(parser, "null")) return false;
                        statement->select.has_is_not_null_filter = true;
                    } else {
                        if (!consume_keyword(parser, "null")) return false;
                        statement->select.has_is_null_filter = true;
                    }
                } else if (consume_keyword(parser, "match")) {
                    if (!parse_sql_string(parser, statement->select.match_keyword,
                                          sizeof(statement->select.match_keyword))) return false;
                    statement->select.has_match_filter = true;
                } else if (consume_keyword(parser, "not")) {
                    if (!consume_keyword(parser, "like")) return false;
                    if (!parse_sql_string(parser, statement->select.username_like,
                                          sizeof(statement->select.username_like))) return false;
                    statement->select.has_username_like = true;
                    statement->select.is_not_like = true;
                } else if (consume_keyword(parser, "ilike")) {
                    if (!parse_sql_string(parser, statement->select.username_like,
                                          sizeof(statement->select.username_like))) return false;
                    statement->select.has_username_like = true;
                    statement->select.is_ilike = true;
                } else if (consume_keyword(parser, "like")) {
                    if (!parse_sql_string(parser, statement->select.username_like,
                                          sizeof(statement->select.username_like))) return false;
                    statement->select.has_username_like = true;
                } else {
                    if (!consume_character(parser, '=')) return false;
                    if (!parse_sql_string(parser, statement->select.username,
                                          sizeof(statement->select.username))) return false;
                    statement->select.has_username_filter = true;
                }
            } else if (consume_keyword(parser, "email")) {
                if (consume_keyword(parser, "is")) {
                    strncpy(statement->select.null_target_col, "email", sizeof(statement->select.null_target_col) - 1);
                    if (consume_keyword(parser, "not")) {
                        if (!consume_keyword(parser, "null")) return false;
                        statement->select.has_is_not_null_filter = true;
                    } else {
                        if (!consume_keyword(parser, "null")) return false;
                        statement->select.has_is_null_filter = true;
                    }
                } else if (consume_keyword(parser, "match")) {
                    if (!parse_sql_string(parser, statement->select.match_keyword,
                                          sizeof(statement->select.match_keyword))) return false;
                    statement->select.has_match_filter = true;
                } else if (consume_keyword(parser, "not")) {
                    if (!consume_keyword(parser, "like")) return false;
                    if (!parse_sql_string(parser, statement->select.email_like,
                                          sizeof(statement->select.email_like))) return false;
                    statement->select.has_email_like = true;
                    statement->select.is_not_like = true;
                } else if (consume_keyword(parser, "ilike")) {
                    if (!parse_sql_string(parser, statement->select.email_like,
                                          sizeof(statement->select.email_like))) return false;
                    statement->select.has_email_like = true;
                    statement->select.is_ilike = true;
                } else if (consume_keyword(parser, "like")) {
                    if (!parse_sql_string(parser, statement->select.email_like,
                                          sizeof(statement->select.email_like))) return false;
                    statement->select.has_email_like = true;
                } else {
                    if (!consume_character(parser, '=')) return false;
                    if (!parse_sql_string(parser, statement->select.email,
                                          sizeof(statement->select.email))) return false;
                    statement->select.has_email_filter = true;
                }
            } else {
                return false;
            }

            if (consume_keyword(parser, "and")) {
                continue;
            }
            break;
        }
    }

    /* Optional GROUP BY clause: GROUP BY <column_name> */
    if (consume_keyword(parser, "group")) {
        if (!consume_keyword(parser, "by")) return false;
        if (!parse_identifier_into(parser, statement->select.group_by_col, sizeof(statement->select.group_by_col))) {
            return false;
        }
        statement->select.has_group_by = true;
    }

    /* Optional HAVING clause: HAVING COUNT(*) > N, HAVING MIN(id) >= N, etc. */
    if (consume_keyword(parser, "having")) {
        AggregateType h_agg = AGGREGATE_NONE;
        if (consume_keyword(parser, "count")) {
            if (!consume_character(parser, '(') || !consume_character(parser, '*') || !consume_character(parser, ')')) return false;
            h_agg = AGGREGATE_COUNT;
        } else if (consume_keyword(parser, "min")) {
            if (!consume_character(parser, '(') || !consume_keyword(parser, "id") || !consume_character(parser, ')')) return false;
            h_agg = AGGREGATE_MIN;
        } else if (consume_keyword(parser, "max")) {
            if (!consume_character(parser, '(') || !consume_keyword(parser, "id") || !consume_character(parser, ')')) return false;
            h_agg = AGGREGATE_MAX;
        } else if (consume_keyword(parser, "sum")) {
            if (!consume_character(parser, '(') || !consume_keyword(parser, "id") || !consume_character(parser, ')')) return false;
            h_agg = AGGREGATE_SUM;
        } else if (consume_keyword(parser, "avg")) {
            if (!consume_character(parser, '(') || !consume_keyword(parser, "id") || !consume_character(parser, ')')) return false;
            h_agg = AGGREGATE_AVG;
        } else {
            return false;
        }

        CompareOp h_op;
        uint32_t h_val;
        if (!consume_compare_op(parser, &h_op)) return false;
        if (!parse_uint32(parser, &h_val)) return false;

        statement->select.has_having = true;
        statement->select.having_agg = h_agg;
        statement->select.having_op = h_op;
        statement->select.having_val = h_val;
    }

    /* Optional ORDER BY clause */
    if (consume_keyword(parser, "order")) {
        if (!consume_keyword(parser, "by")) return false;
        char ord_first_col[32] = "";
        if (!parse_identifier_into(parser, ord_first_col, sizeof(ord_first_col))) return false;
        statement->select.has_order_by_col = true;
        strncpy(statement->select.order_by_col, ord_first_col, sizeof(statement->select.order_by_col) - 1);

        if (consume_keyword(parser, "desc")) {
            statement->select.has_order_desc = true;
        } else {
            consume_keyword(parser, "asc");
        }

        if (consume_character(parser, ',')) {
            char sec_col[32] = "";
            if (!parse_identifier_into(parser, sec_col, sizeof(sec_col))) return false;
            statement->select.has_secondary_order_by = true;
            strncpy(statement->select.secondary_order_col, sec_col, sizeof(statement->select.secondary_order_col) - 1);

            if (consume_keyword(parser, "desc")) {
                statement->select.secondary_order_desc = true;
            } else {
                consume_keyword(parser, "asc");
            }
        }
    }

    /* Optional LIMIT N OFFSET M */
    if (consume_keyword(parser, "limit")) {
        if (!parse_uint32(parser, &statement->select.limit)) return false;
        statement->select.has_limit = true;
        if (consume_keyword(parser, "offset")) {
            if (!parse_uint32(parser, &statement->select.offset)) return false;
            statement->select.has_offset = true;
        }
    } else if (consume_keyword(parser, "offset")) {
        if (!parse_uint32(parser, &statement->select.offset)) return false;
        statement->select.has_offset = true;
    }

    if (consume_keyword(parser, "union")) {
        statement->select.is_union = true;
        if (consume_keyword(parser, "all")) {
            statement->select.is_union_all = true;
        }
        if (!consume_keyword(parser, "select")) return false;
        skip_spaces(parser);
        snprintf(statement->select.union_second_select, sizeof(statement->select.union_second_select), "select %s", parser->current);
        size_t len = strlen(statement->select.union_second_select);
        while (len > 0 && (statement->select.union_second_select[len - 1] == ';' || statement->select.union_second_select[len - 1] == ' ' || statement->select.union_second_select[len - 1] == '\n')) {
            statement->select.union_second_select[--len] = '\0';
        }
        return true;
    }

    return check_query_end(parser);
}

static bool parse_create_index(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_CREATE_INDEX;
    if (!consume_keyword(parser, "index") ||
        !parse_identifier_into(parser, statement->create_index.name, sizeof(statement->create_index.name)) ||
        !consume_keyword(parser, "on") ||
        !parse_identifier_into(parser, statement->create_index.table_name, sizeof(statement->create_index.table_name)) ||
        !consume_character(parser, '(') ||
        !parse_identifier_into(parser, statement->create_index.column_name, sizeof(statement->create_index.column_name))) {
        return false;
    }

    skip_spaces(parser);
    if (consume_character(parser, ',')) {
        if (!parse_identifier_into(parser, statement->create_index.column_name2, sizeof(statement->create_index.column_name2))) {
            return false;
        }
        statement->create_index.num_columns = 2;
    } else {
        statement->create_index.num_columns = 1;
    }

    return consume_character(parser, ')') && consume_statement_end(parser);
}

static bool parse_drop_index(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_DROP_INDEX;
    return consume_keyword(parser, "index") &&
           parse_identifier_into(parser, statement->drop_index.name, sizeof(statement->drop_index.name)) &&
           consume_statement_end(parser);
}

static bool parse_create_view(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_CREATE_VIEW;
    if (!consume_keyword(parser, "view")) return false;
    if (!parse_identifier_into(parser, statement->create_view.view_name, sizeof(statement->create_view.view_name))) {
        return false;
    }
    if (!consume_keyword(parser, "as")) return false;
    skip_spaces(parser);
    strncpy(statement->create_view.select_sql, parser->current, sizeof(statement->create_view.select_sql) - 1);
    size_t len = strlen(statement->create_view.select_sql);
    while (len > 0 && (statement->create_view.select_sql[len - 1] == ';' || statement->create_view.select_sql[len - 1] == ' ' || statement->create_view.select_sql[len - 1] == '\n')) {
        statement->create_view.select_sql[--len] = '\0';
    }
    return len > 0;
}

static bool parse_drop_view(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_DROP_VIEW;
    if (!consume_keyword(parser, "view")) return false;
    return parse_identifier_into(parser, statement->drop_view.view_name, sizeof(statement->drop_view.view_name)) &&
           consume_statement_end(parser);
}

static bool parse_savepoint(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_SAVEPOINT;
    return parse_identifier_into(parser, statement->savepoint.name, sizeof(statement->savepoint.name)) &&
           consume_statement_end(parser);
}

static bool parse_rollback(Parser* parser, Statement* statement) {
    if (consume_keyword(parser, "to")) {
        consume_keyword(parser, "savepoint");
        statement->type = STATEMENT_ROLLBACK_TO;
        return parse_identifier_into(parser, statement->savepoint.name, sizeof(statement->savepoint.name)) &&
               consume_statement_end(parser);
    }
    statement->type = STATEMENT_ROLLBACK;
    return consume_statement_end(parser);
}

static bool parse_release(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_RELEASE_SAVEPOINT;
    consume_keyword(parser, "savepoint");
    return parse_identifier_into(parser, statement->savepoint.name, sizeof(statement->savepoint.name)) &&
           consume_statement_end(parser);
}

static bool parse_pragma(Parser* parser, Statement* statement) {
    if (consume_keyword(parser, "integrity_check")) {
        statement->type = STATEMENT_PRAGMA_INTEGRITY_CHECK;
        return consume_statement_end(parser);
    }
    if (consume_keyword(parser, "checkpoint")) {
        statement->type = STATEMENT_CHECKPOINT;
        return consume_statement_end(parser);
    }
    if (consume_keyword(parser, "table_info")) {
        statement->type = STATEMENT_PRAGMA_TABLE_INFO;
        if (consume_character(parser, '(')) {
            parse_identifier(parser);
            consume_character(parser, ')');
        }
        return consume_statement_end(parser);
    }
    if (consume_keyword(parser, "index_list")) {
        statement->type = STATEMENT_PRAGMA_INDEX_LIST;
        if (consume_character(parser, '(')) {
            parse_identifier(parser);
            consume_character(parser, ')');
        }
        return consume_statement_end(parser);
    }
    if (consume_keyword(parser, "user_version")) {
        if (consume_character(parser, '=')) {
            statement->type = STATEMENT_PRAGMA_SET_USER_VERSION;
            return parse_uint32(parser, &statement->pragma.user_version) &&
                   consume_statement_end(parser);
        }
        statement->type = STATEMENT_PRAGMA_USER_VERSION;
        return consume_statement_end(parser);
    }
    return false;
}

static bool parse_create_table(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_CREATE_TABLE;
    if (!consume_keyword(parser, "table")) return false;
    
    skip_spaces(parser);
    const char* start = parser->current;
    while (isalnum((unsigned char)*parser->current) || *parser->current == '_') {
        parser->current++;
    }
    size_t len = parser->current - start;
    if (len == 0 || len >= sizeof(statement->create_table.table_name)) return false;
    memcpy(statement->create_table.table_name, start, len);
    statement->create_table.table_name[len] = '\0';

    if (!consume_character(parser, '(')) return false;

    uint32_t col_idx = 0;
    while (true) {
        skip_spaces(parser);
        const char* cstart = parser->current;
        while (isalnum((unsigned char)*parser->current) || *parser->current == '_') {
            parser->current++;
        }
        size_t clen = parser->current - cstart;
        if (clen == 0 || clen >= sizeof(statement->create_table.col_names[0])) return false;
        memcpy(statement->create_table.col_names[col_idx], cstart, clen);
        statement->create_table.col_names[col_idx][clen] = '\0';

        skip_spaces(parser);
        const char* tstart = parser->current;
        while (isalpha((unsigned char)*parser->current)) {
            parser->current++;
        }
        size_t tlen = parser->current - tstart;
        if (tlen == 0 || tlen >= sizeof(statement->create_table.col_types[0])) return false;
        memcpy(statement->create_table.col_types[col_idx], tstart, tlen);
        statement->create_table.col_types[col_idx][tlen] = '\0';

        skip_spaces(parser);
        if (consume_keyword(parser, "references")) {
            statement->create_table.has_fk = true;
            memcpy(statement->create_table.fk_col, statement->create_table.col_names[col_idx], sizeof(statement->create_table.fk_col));
            skip_spaces(parser);
            const char* pstart = parser->current;
            while (isalnum((unsigned char)*parser->current) || *parser->current == '_') {
                parser->current++;
            }
            size_t plen = parser->current - pstart;
            if (plen > 0 && plen < sizeof(statement->create_table.fk_parent_table)) {
                memcpy(statement->create_table.fk_parent_table, pstart, plen);
                statement->create_table.fk_parent_table[plen] = '\0';
            }
            if (consume_character(parser, '(')) {
                const char* pcstart = parser->current;
                while (isalnum((unsigned char)*parser->current) || *parser->current == '_') {
                    parser->current++;
                }
                size_t pclen = parser->current - pcstart;
                if (pclen > 0 && pclen < sizeof(statement->create_table.fk_parent_col)) {
                    memcpy(statement->create_table.fk_parent_col, pcstart, pclen);
                    statement->create_table.fk_parent_col[pclen] = '\0';
                }
                consume_character(parser, ')');
            }

            if (consume_keyword(parser, "on")) {
                if (consume_keyword(parser, "delete") && consume_keyword(parser, "cascade")) {
                    statement->create_table.fk_on_delete_cascade = true;
                }
            }
        }

        col_idx++;
        skip_spaces(parser);
        if (consume_character(parser, ',')) {
            continue;
        }
        if (consume_character(parser, ')')) {
            break;
        }
        return false;
    }
    statement->create_table.num_columns = col_idx;
    return consume_statement_end(parser);
}

static bool parse_prepare(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_PREPARE;
    skip_spaces(parser);
    const char* nstart = parser->current;
    while (isalnum((unsigned char)*parser->current) || *parser->current == '_') {
        parser->current++;
    }
    size_t nlen = parser->current - nstart;
    if (nlen == 0 || nlen >= sizeof(statement->prepare.name)) return false;
    memcpy(statement->prepare.name, nstart, nlen);
    statement->prepare.name[nlen] = '\0';

    if (!consume_keyword(parser, "from")) return false;

    skip_spaces(parser);
    const char* tstart = parser->current;
    size_t tlen = strlen(tstart);
    if (tlen == 0 || tlen >= sizeof(statement->prepare.sql_template)) return false;
    memcpy(statement->prepare.sql_template, tstart, tlen);
    statement->prepare.sql_template[tlen] = '\0';
    return true;
}

static bool parse_execute_prepared(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_EXECUTE_PREPARED;
    skip_spaces(parser);
    const char* nstart = parser->current;
    while (isalnum((unsigned char)*parser->current) || *parser->current == '_') {
        parser->current++;
    }
    size_t nlen = parser->current - nstart;
    if (nlen == 0 || nlen >= sizeof(statement->execute_prepared.name)) return false;
    memcpy(statement->execute_prepared.name, nstart, nlen);
    statement->execute_prepared.name[nlen] = '\0';

    if (consume_keyword(parser, "using")) {
        parse_uint32(parser, &statement->execute_prepared.param_val);
    }
    return true;
}

static bool parse_alter_table(Parser* parser, Statement* statement) {
    statement->type = STATEMENT_ALTER_TABLE;
    if (!consume_keyword(parser, "table")) return false;
    if (!parse_identifier_into(parser, statement->alter_table.table_name, sizeof(statement->alter_table.table_name))) return false;

    if (consume_keyword(parser, "rename")) {
        if (!consume_keyword(parser, "to")) return false;
        if (!parse_identifier_into(parser, statement->alter_table.new_table_name, sizeof(statement->alter_table.new_table_name))) return false;
        statement->alter_table.is_rename = true;
        return consume_statement_end(parser);
    }

    if (consume_keyword(parser, "add")) {
        if (!consume_keyword(parser, "column")) return false;
        if (!parse_identifier_into(parser, statement->alter_table.new_col_name, sizeof(statement->alter_table.new_col_name))) return false;
        if (!parse_identifier_into(parser, statement->alter_table.new_col_type, sizeof(statement->alter_table.new_col_type))) return false;
        statement->alter_table.is_add_column = true;
        return consume_statement_end(parser);
    }

    return false;
}

static bool parse_with_cte(Parser* parser, Statement* statement) {
    if (!parse_identifier_into(parser, statement->cte_name, sizeof(statement->cte_name))) {
        return false;
    }
    if (!consume_keyword(parser, "as")) return false;
    if (!consume_character(parser, '(')) return false;
    if (!consume_keyword(parser, "select")) return false;

    skip_spaces(parser);
    const char* start = parser->current;
    int paren_depth = 1;
    while (*parser->current != '\0') {
        if (*parser->current == '(') paren_depth++;
        else if (*parser->current == ')') {
            paren_depth--;
            if (paren_depth == 0) break;
        }
        parser->current++;
    }
    if (paren_depth != 0) return false;

    size_t len = parser->current - start;
    if (len >= sizeof(statement->cte_select_sql)) return false;
    snprintf(statement->cte_select_sql, sizeof(statement->cte_select_sql), "select %.*s", (int)len, start);
    parser->current++; /* skip ')' */

    statement->has_cte = true;
    if (!consume_keyword(parser, "select")) return false;
    return parse_select(parser, statement);
}

PrepareResult prepare_statement(const char* input, Statement* statement) {
    Parser parser;

    memset(statement, 0, sizeof(*statement));
    parser.current = input;

    if (consume_keyword(&parser, "with")) {
        return parse_with_cte(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "prepare")) {
        return parse_prepare(&parser, statement) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "execute")) {
        return parse_execute_prepared(&parser, statement) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "explain")) {
        statement->explain = true;
        if (!consume_keyword(&parser, "select") ||
            !parse_select(&parser, statement)) {
            return PREPARE_SYNTAX_ERROR;
        }
        return PREPARE_SUCCESS;
    }

    if (consume_keyword(&parser, "create")) {
        if (parse_create_view(&parser, statement)) {
            return PREPARE_SUCCESS;
        }
        if (parse_create_index(&parser, statement)) {
            return PREPARE_SUCCESS;
        }
        if (parse_create_table(&parser, statement)) {
            return PREPARE_SUCCESS;
        }
        return PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "insert")) {
        return parse_insert(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "select")) {
        return parse_select(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "delete")) {
        return parse_delete(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "begin")) {
        statement->type = STATEMENT_BEGIN;
        return consume_statement_end(&parser) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "commit")) {
        statement->type = STATEMENT_COMMIT;
        return consume_statement_end(&parser) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "rollback")) {
        return parse_rollback(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "savepoint")) {
        return parse_savepoint(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "release")) {
        return parse_release(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "update")) {
        return parse_update(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "create")) {
        return parse_create_index(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "drop")) {
        if (parse_drop_view(&parser, statement)) {
            return PREPARE_SUCCESS;
        }
        if (parse_drop_index(&parser, statement)) {
            return PREPARE_SUCCESS;
        }
        return PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "vacuum")) {
        statement->type = STATEMENT_VACUUM;
        if (consume_keyword(&parser, "into")) {
            if (!parse_sql_string(&parser, statement->vacuum.into_filename, sizeof(statement->vacuum.into_filename))) {
                if (!parse_token(&parser, statement->vacuum.into_filename, sizeof(statement->vacuum.into_filename))) {
                    return PREPARE_SYNTAX_ERROR;
                }
            }
            statement->vacuum.has_into = true;
        }
        return consume_statement_end(&parser) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "checkpoint")) {
        statement->type = STATEMENT_CHECKPOINT;
        return consume_statement_end(&parser) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "pragma")) {
        return parse_pragma(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    if (consume_keyword(&parser, "alter")) {
        return parse_alter_table(&parser, statement)
            ? PREPARE_SUCCESS
            : PREPARE_SYNTAX_ERROR;
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}
