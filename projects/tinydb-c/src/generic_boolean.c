#include "generic_boolean.h"

#include <stdio.h>

static bool allocate_node(TinyDBGenericBooleanExpression* expression,
                          TinyDBGenericBooleanKind kind,
                          uint32_t* index) {
    if (expression->count >= TINYDB_GENERIC_BOOLEAN_MAX_NODES) return false;
    *index = expression->count++;
    memset(&expression->nodes[*index], 0, sizeof(expression->nodes[*index]));
    expression->nodes[*index].kind = kind;
    return true;
}

static bool parse_or(TinyDBGenericParser* parser,
                     const TableSchema* schema,
                     TinyDBGenericBooleanExpression* expression,
                     uint32_t* root);

static bool parse_primary(TinyDBGenericParser* parser,
                          const TableSchema* schema,
                          TinyDBGenericBooleanExpression* expression,
                          uint32_t* root) {
    tinydb_generic_skip_spaces(parser);
    if (*parser->current == '(') {
        parser->current++;
        expression->saw_grouping = true;
        if (!parse_or(parser, schema, expression, root) ||
            !tinydb_generic_consume_char(parser, ')')) {
            return false;
        }
        expression->nodes[*root].grouped = true;
        return true;
    }

    uint32_t node_index = 0;
    if (!allocate_node(expression, TINYDB_GENERIC_BOOLEAN_PREDICATE, &node_index)) {
        return false;
    }
    if (!tinydb_generic_parse_predicate(parser,
                                        schema,
                                        &expression->nodes[node_index].predicate)) {
        expression->count--;
        return false;
    }
    *root = node_index;
    return true;
}

static bool parse_and(TinyDBGenericParser* parser,
                      const TableSchema* schema,
                      TinyDBGenericBooleanExpression* expression,
                      uint32_t* root) {
    uint32_t left = 0;
    if (!parse_primary(parser, schema, expression, &left)) return false;

    while (tinydb_generic_consume_word(parser, "and")) {
        uint32_t right = 0;
        uint32_t combined = 0;
        if (!parse_primary(parser, schema, expression, &right) ||
            !allocate_node(expression, TINYDB_GENERIC_BOOLEAN_AND, &combined)) {
            return false;
        }
        expression->nodes[combined].left = left;
        expression->nodes[combined].right = right;
        left = combined;
    }

    *root = left;
    return true;
}

static bool parse_or(TinyDBGenericParser* parser,
                     const TableSchema* schema,
                     TinyDBGenericBooleanExpression* expression,
                     uint32_t* root) {
    uint32_t left = 0;
    if (!parse_and(parser, schema, expression, &left)) return false;

    while (tinydb_generic_consume_word(parser, "or")) {
        uint32_t right = 0;
        uint32_t combined = 0;
        if (!parse_and(parser, schema, expression, &right) ||
            !allocate_node(expression, TINYDB_GENERIC_BOOLEAN_OR, &combined)) {
            return false;
        }
        expression->nodes[combined].left = left;
        expression->nodes[combined].right = right;
        left = combined;
    }

    *root = left;
    return true;
}

bool tinydb_generic_parse_boolean_expression(
    TinyDBGenericParser* parser,
    const TableSchema* schema,
    TinyDBGenericBooleanExpression* expression) {
    if (parser == NULL || schema == NULL || expression == NULL) return false;
    memset(expression, 0, sizeof(*expression));
    if (!parse_or(parser, schema, expression, &expression->root)) return false;
    return expression->count > 0;
}

static bool matches_node(const TinyDBGenericBooleanExpression* expression,
                         uint32_t node_index,
                         const TinyDBValue* values,
                         uint32_t value_count) {
    if (node_index >= expression->count) return false;
    const TinyDBGenericBooleanNode* node = &expression->nodes[node_index];
    if (node->kind == TINYDB_GENERIC_BOOLEAN_PREDICATE) {
        if (node->predicate.column_index >= value_count) return false;
        return tinydb_generic_predicate_matches(
            &node->predicate, &values[node->predicate.column_index]);
    }
    if (node->kind == TINYDB_GENERIC_BOOLEAN_AND) {
        return matches_node(expression, node->left, values, value_count) &&
               matches_node(expression, node->right, values, value_count);
    }
    if (node->kind == TINYDB_GENERIC_BOOLEAN_OR) {
        return matches_node(expression, node->left, values, value_count) ||
               matches_node(expression, node->right, values, value_count);
    }
    return false;
}

bool tinydb_generic_boolean_matches(
    const TinyDBGenericBooleanExpression* expression,
    const TinyDBValue* values,
    uint32_t value_count) {
    if (expression == NULL || values == NULL || expression->count == 0) {
        return false;
    }
    return matches_node(expression, expression->root, values, value_count);
}

static bool append_char(char* output,
                        size_t output_size,
                        size_t* length,
                        char value) {
    if (*length + 1 >= output_size) return false;
    output[(*length)++] = value;
    output[*length] = '\0';
    return true;
}

static bool append_text(char* output,
                        size_t output_size,
                        size_t* length,
                        const char* text) {
    while (*text != '\0') {
        if (!append_char(output, output_size, length, *text++)) return false;
    }
    return true;
}

static int node_precedence(TinyDBGenericBooleanKind kind) {
    if (kind == TINYDB_GENERIC_BOOLEAN_OR) return 1;
    if (kind == TINYDB_GENERIC_BOOLEAN_AND) return 2;
    return 3;
}

static bool append_predicate(char* output,
                             size_t output_size,
                             size_t* length,
                             const TableSchema* schema,
                             const TinyDBGenericPredicate* predicate) {
    if (predicate->column_index >= schema->num_columns) return false;
    if (!append_text(output,
                     output_size,
                     length,
                     schema->columns[predicate->column_index].name) ||
        !append_char(output, output_size, length, ' ') ||
        !append_text(output,
                     output_size,
                     length,
                     tinydb_generic_compare_op_text(predicate->op)) ||
        !append_char(output, output_size, length, ' ')) {
        return false;
    }

    if (predicate->value.type == COL_TYPE_INT) {
        char number[16];
        snprintf(number, sizeof(number), "%u", predicate->value.int_value);
        return append_text(output, output_size, length, number);
    }

    if (!append_char(output, output_size, length, '\'')) return false;
    const char* current = predicate->value.text;
    while (*current != '\0') {
        if (*current == '\'' &&
            !append_char(output, output_size, length, '\'')) {
            return false;
        }
        if (!append_char(output, output_size, length, *current++)) return false;
    }
    return append_char(output, output_size, length, '\'');
}

static bool format_node(const TinyDBGenericBooleanExpression* expression,
                        uint32_t node_index,
                        const TableSchema* schema,
                        int parent_precedence,
                        char* output,
                        size_t output_size,
                        size_t* length) {
    if (node_index >= expression->count) return false;
    const TinyDBGenericBooleanNode* node = &expression->nodes[node_index];
    int precedence = node_precedence(node->kind);
    bool wrap = node->grouped || precedence < parent_precedence;
    if (wrap && !append_char(output, output_size, length, '(')) return false;

    if (node->kind == TINYDB_GENERIC_BOOLEAN_PREDICATE) {
        if (!append_predicate(output,
                              output_size,
                              length,
                              schema,
                              &node->predicate)) {
            return false;
        }
    } else {
        if (!format_node(expression,
                         node->left,
                         schema,
                         precedence,
                         output,
                         output_size,
                         length)) {
            return false;
        }
        const char* operator_text =
            node->kind == TINYDB_GENERIC_BOOLEAN_AND ? " AND " : " OR ";
        if (!append_text(output, output_size, length, operator_text) ||
            !format_node(expression,
                         node->right,
                         schema,
                         precedence,
                         output,
                         output_size,
                         length)) {
            return false;
        }
    }

    return !wrap || append_char(output, output_size, length, ')');
}

bool tinydb_generic_boolean_format(
    const TinyDBGenericBooleanExpression* expression,
    const TableSchema* schema,
    char* output,
    size_t output_size) {
    if (expression == NULL || schema == NULL || output == NULL ||
        output_size == 0 || expression->count == 0) {
        return false;
    }
    size_t length = 0;
    output[0] = '\0';
    return format_node(expression,
                       expression->root,
                       schema,
                       0,
                       output,
                       output_size,
                       &length);
}
