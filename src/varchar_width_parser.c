#include "column_type.h"
#include "compiler.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define VARCHAR_WIDTH_SQL_MAX 2048u

PrepareResult prepare_statement_legacy_base(const char* input,
                                            Statement* statement);

static int ci_char(int value) {
    return tolower((unsigned char)value);
}

static bool ci_word_at(const char* input, const char* word, size_t* length_out) {
    const char* current = input;
    const char* expected = word;
    while (*expected != '\0' && *current != '\0' &&
           ci_char(*current) == ci_char(*expected)) {
        current++;
        expected++;
    }
    if (*expected != '\0') return false;
    if (isalnum((unsigned char)*current) || *current == '_') return false;
    if (length_out != NULL) *length_out = (size_t)(current - input);
    return true;
}

static const char* skip_spaces(const char* input) {
    while (isspace((unsigned char)*input)) input++;
    return input;
}

static bool consume_ci_word(const char** input, const char* word) {
    const char* current = skip_spaces(*input);
    size_t length = 0;
    if (!ci_word_at(current, word, &length)) return false;
    *input = current + length;
    return true;
}

static bool starts_create_table(const char* input) {
    const char* current = input;
    return consume_ci_word(&current, "create") &&
           consume_ci_word(&current, "table");
}

static bool starts_alter_table(const char* input) {
    const char* current = input;
    return consume_ci_word(&current, "alter") &&
           consume_ci_word(&current, "table");
}

static bool copy_byte(char* output,
                      size_t output_size,
                      size_t* written,
                      char value) {
    if (*written + 1u >= output_size) return false;
    output[(*written)++] = value;
    output[*written] = '\0';
    return true;
}

static bool copy_span(char* output,
                      size_t output_size,
                      size_t* written,
                      const char* input,
                      size_t length) {
    if (length > output_size - *written - 1u) return false;
    memcpy(output + *written, input, length);
    *written += length;
    output[*written] = '\0';
    return true;
}

static bool normalize_varchar_widths(const char* input,
                                     char* output,
                                     size_t output_size,
                                     bool* had_sized_varchar) {
    size_t written = 0;
    bool in_string = false;
    *had_sized_varchar = false;
    output[0] = '\0';

    for (const char* current = input; *current != '\0';) {
        if (*current == '\'') {
            if (!copy_byte(output, output_size, &written, *current++)) return false;
            if (in_string && *current == '\'') {
                if (!copy_byte(output, output_size, &written, *current++)) return false;
                continue;
            }
            in_string = !in_string;
            continue;
        }

        if (!in_string &&
            (current == input || !isalnum((unsigned char)current[-1])) &&
            (current == input || current[-1] != '_')) {
            size_t word_length = 0;
            if (ci_word_at(current, "varchar", &word_length)) {
                TinyDBColumnTypeSpec type;
                const char* type_end = NULL;
                if (tinydb_column_type_parse_prefix(current, &type, &type_end) &&
                    type.type == COL_TYPE_VARCHAR &&
                    type.explicitly_sized) {
                    if (!copy_span(output,
                                   output_size,
                                   &written,
                                   current,
                                   word_length)) {
                        return false;
                    }
                    current = type_end;
                    *had_sized_varchar = true;
                    continue;
                }

                const char* after_word = skip_spaces(current + word_length);
                if (*after_word == '(') {
                    /* A VARCHAR suffix was present but malformed or outside
                     * the shared 1..255 character range. */
                    return false;
                }
            }
        }

        if (!copy_byte(output, output_size, &written, *current++)) return false;
    }
    return !in_string;
}

static bool parse_identifier(const char** input) {
    const char* current = skip_spaces(*input);
    if (!isalpha((unsigned char)*current) && *current != '_') return false;
    while (isalnum((unsigned char)*current) || *current == '_') current++;
    *input = current;
    return true;
}

static bool annotate_create_widths(const char* input,
                                   CreateTableStatement* create) {
    const char* current = input;
    if (!consume_ci_word(&current, "create") ||
        !consume_ci_word(&current, "table") ||
        !parse_identifier(&current)) {
        return false;
    }
    current = skip_spaces(current);
    if (*current != '(') return false;
    current++;

    for (uint32_t column = 0; column < create->num_columns; column++) {
        if (!parse_identifier(&current)) return false;
        current = skip_spaces(current);

        const char* type_start = current;
        while (isalpha((unsigned char)*current)) current++;
        if (current == type_start) return false;

        size_t type_length = (size_t)(current - type_start);
        bool is_varchar = type_length == 7u;
        if (is_varchar) {
            static const char expected[] = "varchar";
            for (size_t i = 0; i < 7u; i++) {
                if (ci_char(type_start[i]) != expected[i]) {
                    is_varchar = false;
                    break;
                }
            }
        }

        if (is_varchar) {
            const char* after_type = skip_spaces(current);
            if (*after_type == '(') {
                TinyDBColumnTypeSpec type;
                const char* type_end = NULL;
                if (!tinydb_column_type_parse_prefix(type_start, &type, &type_end) ||
                    type.type != COL_TYPE_VARCHAR ||
                    !type.explicitly_sized) {
                    return false;
                }
                int formatted = snprintf(create->col_types[column],
                                         sizeof(create->col_types[column]),
                                         "VARCHAR(%u)",
                                         type.declared_capacity);
                if (formatted < 0 ||
                    (size_t)formatted >= sizeof(create->col_types[column])) {
                    return false;
                }
                current = type_end;
            }
        }

        int depth = 1;
        bool in_string = false;
        while (*current != '\0') {
            char value = *current;
            if (value == '\'') {
                if (in_string && current[1] == '\'') {
                    current += 2;
                    continue;
                }
                in_string = !in_string;
                current++;
                continue;
            }
            if (!in_string) {
                if (value == '(') {
                    depth++;
                } else if (value == ')') {
                    depth--;
                    if (depth == 0) {
                        if (column + 1u != create->num_columns) return false;
                        return true;
                    }
                } else if (value == ',' && depth == 1) {
                    current++;
                    break;
                }
            }
            current++;
        }
        if (*current == '\0' && column + 1u < create->num_columns) return false;
    }
    return true;
}

static bool annotate_alter_width(const char* input,
                                 AlterTableStatement* alter) {
    const char* current = input;
    if (!consume_ci_word(&current, "alter") ||
        !consume_ci_word(&current, "table") ||
        !parse_identifier(&current) ||
        !consume_ci_word(&current, "add") ||
        !consume_ci_word(&current, "column") ||
        !parse_identifier(&current)) {
        return false;
    }

    TinyDBColumnTypeSpec type;
    const char* type_end = NULL;
    if (!tinydb_column_type_parse_prefix(current, &type, &type_end) ||
        type.type != COL_TYPE_VARCHAR ||
        !type.explicitly_sized) {
        return false;
    }

    current = skip_spaces(type_end);
    if (*current == ';') current = skip_spaces(current + 1);
    if (*current != '\0') return false;

    int formatted = snprintf(alter->new_col_type,
                             sizeof(alter->new_col_type),
                             "VARCHAR(%u)",
                             type.declared_capacity);
    return formatted >= 0 &&
           (size_t)formatted < sizeof(alter->new_col_type);
}

PrepareResult prepare_statement(const char* input, Statement* statement) {
    if (input == NULL || statement == NULL) {
        return prepare_statement_legacy_base(input, statement);
    }

    bool create_table = starts_create_table(input);
    bool alter_table = starts_alter_table(input);
    if (!create_table && !alter_table) {
        return prepare_statement_legacy_base(input, statement);
    }

    char normalized[VARCHAR_WIDTH_SQL_MAX];
    bool had_sized_varchar = false;
    if (!normalize_varchar_widths(input,
                                  normalized,
                                  sizeof(normalized),
                                  &had_sized_varchar)) {
        return PREPARE_SYNTAX_ERROR;
    }

    PrepareResult result = prepare_statement_legacy_base(
        had_sized_varchar ? normalized : input,
        statement);
    if (result != PREPARE_SUCCESS || !had_sized_varchar) {
        return result;
    }

    bool annotated = false;
    if (statement->type == STATEMENT_CREATE_TABLE) {
        annotated = annotate_create_widths(input, &statement->create_table);
    } else if (statement->type == STATEMENT_ALTER_TABLE &&
               statement->alter_table.is_add_column) {
        annotated = annotate_alter_width(input, &statement->alter_table);
    }

    if (!annotated) {
        memset(statement, 0, sizeof(*statement));
        return PREPARE_SYNTAX_ERROR;
    }
    return PREPARE_SUCCESS;
}
