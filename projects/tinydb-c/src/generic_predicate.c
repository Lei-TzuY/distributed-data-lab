#include "generic_predicate.h"

#include <ctype.h>
#include <errno.h>

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool is_identifier_char(char value) {
    return isalnum((unsigned char)value) || value == '_';
}

void tinydb_generic_parser_init(TinyDBGenericParser* parser, const char* sql) {
    parser->current = sql;
}

void tinydb_generic_skip_spaces(TinyDBGenericParser* parser) {
    while (isspace((unsigned char)*parser->current)) parser->current++;
}

bool tinydb_generic_consume_word(TinyDBGenericParser* parser, const char* word) {
    TinyDBGenericParser backup = *parser;
    const char* expected = word;
    tinydb_generic_skip_spaces(parser);
    while (*expected != '\0' &&
           ci_char(*parser->current) == ci_char(*expected)) {
        parser->current++;
        expected++;
    }
    if (*expected != '\0' || is_identifier_char(*parser->current)) {
        *parser = backup;
        return false;
    }
    return true;
}

bool tinydb_generic_consume_char(TinyDBGenericParser* parser, char expected) {
    tinydb_generic_skip_spaces(parser);
    if (*parser->current != expected) return false;
    parser->current++;
    return true;
}

bool tinydb_generic_parse_identifier(TinyDBGenericParser* parser,
                                     char* output,
                                     size_t output_size) {
    tinydb_generic_skip_spaces(parser);
    if (!isalpha((unsigned char)*parser->current) && *parser->current != '_') {
        return false;
    }
    const char* start = parser->current;
    while (is_identifier_char(*parser->current)) parser->current++;
    size_t length = (size_t)(parser->current - start);
    if (length == 0 || length >= output_size) return false;
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

bool tinydb_generic_parse_uint32(TinyDBGenericParser* parser, uint32_t* value) {
    tinydb_generic_skip_spaces(parser);
    if (!isdigit((unsigned char)*parser->current)) return false;
    char* end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(parser->current, &end, 10);
    if (errno == ERANGE || parsed > UINT32_MAX || end == parser->current) {
        return false;
    }
    parser->current = end;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_string(TinyDBGenericParser* parser,
                         char* output,
                         size_t output_size) {
    tinydb_generic_skip_spaces(parser);
    if (*parser->current != '\'') return false;
    parser->current++;
    size_t length = 0;

    while (*parser->current != '\0') {
        char value = *parser->current++;
        if (value == '\'') {
            if (*parser->current == '\'') {
                parser->current++;
                value = '\'';
            } else {
                if (length >= output_size) return false;
                output[length] = '\0';
                return true;
            }
        }
        if (length + 1 >= output_size) return false;
        output[length++] = value;
    }
    return false;
}

bool tinydb_generic_consume_end(TinyDBGenericParser* parser) {
    tinydb_generic_skip_spaces(parser);
    if (*parser->current == ';') parser->current++;
    tinydb_generic_skip_spaces(parser);
    return *parser->current == '\0';
}

int tinydb_generic_find_column_index(const TableSchema* schema, const char* name) {
    if (schema == NULL || name == NULL) return -1;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        const char* left = schema->columns[i].name;
        const char* right = name;
        while (*left != '\0' && *right != '\0' &&
               ci_char(*left) == ci_char(*right)) {
            left++;
            right++;
        }
        if (*left == '\0' && *right == '\0') return (int)i;
    }
    return -1;
}

bool tinydb_generic_parse_value_for_column(TinyDBGenericParser* parser,
                                           const TableColumn* column,
                                           TinyDBValue* value) {
    if (parser == NULL || column == NULL || value == NULL) return false;
    memset(value, 0, sizeof(*value));
    value->type = column->type;
    if (column->type == COL_TYPE_INT) {
        return tinydb_generic_parse_uint32(parser, &value->int_value);
    }
    if (column->type == COL_TYPE_VARCHAR) {
        return parse_string(parser, value->text, sizeof(value->text));
    }
    return false;
}

static bool parse_compare_op(TinyDBGenericParser* parser,
                             TinyDBGenericCompareOp* op) {
    if (tinydb_generic_consume_word(parser, "like")) {
        *op = TINYDB_GENERIC_COMPARE_LIKE;
        return true;
    }

    tinydb_generic_skip_spaces(parser);
    if (*parser->current == '=') {
        parser->current++;
        *op = TINYDB_GENERIC_COMPARE_EQ;
        return true;
    }
    if (*parser->current == '>') {
        parser->current++;
        if (*parser->current == '=') {
            parser->current++;
            *op = TINYDB_GENERIC_COMPARE_GTE;
        } else {
            *op = TINYDB_GENERIC_COMPARE_GT;
        }
        return true;
    }
    if (*parser->current == '<') {
        parser->current++;
        if (*parser->current == '=') {
            parser->current++;
            *op = TINYDB_GENERIC_COMPARE_LTE;
        } else {
            *op = TINYDB_GENERIC_COMPARE_LT;
        }
        return true;
    }
    return false;
}

bool tinydb_generic_parse_predicate(TinyDBGenericParser* parser,
                                    const TableSchema* schema,
                                    TinyDBGenericPredicate* predicate) {
    char column[MAX_NAME_SIZE];
    if (parser == NULL || schema == NULL || predicate == NULL) return false;
    if (!tinydb_generic_parse_identifier(parser, column, sizeof(column))) return false;
    int column_index = tinydb_generic_find_column_index(schema, column);
    if (column_index < 0 || !parse_compare_op(parser, &predicate->op)) return false;
    predicate->column_index = (uint32_t)column_index;
    if (predicate->op == TINYDB_GENERIC_COMPARE_LIKE &&
        schema->columns[predicate->column_index].type != COL_TYPE_VARCHAR) {
        return false;
    }
    return tinydb_generic_parse_value_for_column(
        parser, &schema->columns[predicate->column_index], &predicate->value);
}

static int compare_values(const TinyDBValue* left, const TinyDBValue* right) {
    if (left->type == COL_TYPE_INT) {
        if (left->int_value < right->int_value) return -1;
        if (left->int_value > right->int_value) return 1;
        return 0;
    }
    int compared = strcmp(left->text, right->text);
    if (compared < 0) return -1;
    if (compared > 0) return 1;
    return 0;
}

static bool like_match(const char* pattern, const char* text) {
    if (*pattern == '\0') return *text == '\0';

    if (*pattern == '%') {
        while (*pattern == '%') pattern++;
        if (*pattern == '\0') return true;
        while (*text != '\0') {
            if (like_match(pattern, text)) return true;
            text++;
        }
        return like_match(pattern, text);
    }

    if (*text == '\0') return false;
    if (*pattern == '_' || *pattern == *text) {
        return like_match(pattern + 1, text + 1);
    }
    return false;
}

bool tinydb_generic_predicate_matches(const TinyDBGenericPredicate* predicate,
                                      const TinyDBValue* value) {
    if (predicate == NULL || value == NULL || value->type != predicate->value.type) {
        return false;
    }
    if (predicate->op == TINYDB_GENERIC_COMPARE_LIKE) {
        return value->type == COL_TYPE_VARCHAR &&
               like_match(predicate->value.text, value->text);
    }

    int compared = compare_values(value, &predicate->value);
    switch (predicate->op) {
        case TINYDB_GENERIC_COMPARE_EQ:
            return compared == 0;
        case TINYDB_GENERIC_COMPARE_GT:
            return compared > 0;
        case TINYDB_GENERIC_COMPARE_GTE:
            return compared >= 0;
        case TINYDB_GENERIC_COMPARE_LT:
            return compared < 0;
        case TINYDB_GENERIC_COMPARE_LTE:
            return compared <= 0;
    }
    return false;
}

const char* tinydb_generic_compare_op_text(TinyDBGenericCompareOp op) {
    if (op == TINYDB_GENERIC_COMPARE_LIKE) return "LIKE";
    switch (op) {
        case TINYDB_GENERIC_COMPARE_EQ: return "=";
        case TINYDB_GENERIC_COMPARE_GT: return ">";
        case TINYDB_GENERIC_COMPARE_GTE: return ">=";
        case TINYDB_GENERIC_COMPARE_LT: return "<";
        case TINYDB_GENERIC_COMPARE_LTE: return "<=";
    }
    return "?";
}
