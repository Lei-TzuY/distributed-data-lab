#include "column_type.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static const char* skip_spaces(const char* input) {
    while (input != NULL && isspace((unsigned char)*input)) input++;
    return input;
}

static bool consume_word(const char** input, const char* word) {
    const char* current = skip_spaces(*input);
    const char* expected = word;
    while (*expected != '\0' && *current != '\0' &&
           ci_char(*current) == ci_char(*expected)) {
        current++;
        expected++;
    }
    if (*expected != '\0') return false;
    if (isalnum((unsigned char)*current) || *current == '_') return false;
    *input = current;
    return true;
}

static bool at_end(const char* input) {
    input = skip_spaces(input);
    return input != NULL && *input == '\0';
}

bool tinydb_column_type_parse_prefix(const char* text,
                                     TinyDBColumnTypeSpec* spec,
                                     const char** end_out) {
    if (text == NULL || spec == NULL) return false;
    memset(spec, 0, sizeof(*spec));

    const char* current = text;
    if (consume_word(&current, "INT") || consume_word(&current, "INTEGER")) {
        spec->type = COL_TYPE_INT;
        spec->storage_size = (uint32_t)sizeof(uint32_t);
        if (end_out != NULL) *end_out = current;
        return true;
    }

    current = text;
    if (!consume_word(&current, "VARCHAR")) return false;
    const char* after_word = current;
    current = skip_spaces(current);

    if (*current != '(') {
        spec->type = COL_TYPE_VARCHAR;
        spec->storage_size = 256u;
        spec->declared_capacity = 255u;
        spec->explicitly_sized = false;
        if (end_out != NULL) *end_out = after_word;
        return true;
    }

    current = skip_spaces(current + 1);
    if (!isdigit((unsigned char)*current)) return false;

    uint32_t declared = 0;
    while (isdigit((unsigned char)*current)) {
        uint32_t digit = (uint32_t)(*current - '0');
        declared = declared * 10u + digit;
        if (declared > 255u) return false;
        current++;
    }

    current = skip_spaces(current);
    if (*current != ')' || declared == 0u) return false;
    current++;

    spec->type = COL_TYPE_VARCHAR;
    spec->storage_size = declared + 1u;
    spec->declared_capacity = declared;
    spec->explicitly_sized = true;
    if (end_out != NULL) *end_out = current;
    return true;
}

bool tinydb_column_type_parse(const char* text, TinyDBColumnTypeSpec* spec) {
    const char* end = NULL;
    if (!tinydb_column_type_parse_prefix(text, spec, &end)) return false;
    return at_end(end);
}

bool tinydb_column_type_is_int(const char* text) {
    TinyDBColumnTypeSpec spec;
    return tinydb_column_type_parse(text, &spec) && spec.type == COL_TYPE_INT;
}

bool tinydb_column_type_is_varchar(const char* text) {
    TinyDBColumnTypeSpec spec;
    return tinydb_column_type_parse(text, &spec) && spec.type == COL_TYPE_VARCHAR;
}

bool tinydb_column_type_format(const TableColumn* column,
                               char* output,
                               size_t output_size) {
    if (column == NULL || output == NULL || output_size == 0u) return false;

    int written = 0;
    if (column->type == COL_TYPE_INT) {
        if (column->size != (uint32_t)sizeof(uint32_t)) return false;
        written = snprintf(output, output_size, "INT");
    } else if (column->type == COL_TYPE_VARCHAR) {
        if (column->size == 0u) return false;
        written = snprintf(output,
                           output_size,
                           "VARCHAR(%u)",
                           column->size - 1u);
    } else {
        return false;
    }

    return written >= 0 && (size_t)written < output_size;
}
