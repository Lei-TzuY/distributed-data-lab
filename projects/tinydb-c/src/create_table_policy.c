#include "column_type.h"
#include "engine.h"
#include "record_payload.h"
#include "user_version.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

TinyDBSqlStatus tinydb_execute_sql_base(TinyDB* database,
                                        const char* sql,
                                        TinyDBSqlResult* result);

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

static bool legacy_column_names_and_types(const CreateTableStatement* create) {
    TinyDBColumnTypeSpec id;
    TinyDBColumnTypeSpec username;
    TinyDBColumnTypeSpec email;
    return create->num_columns == 3u &&
           ci_equal(create->col_names[0], "id") &&
           ci_equal(create->col_names[1], "username") &&
           ci_equal(create->col_names[2], "email") &&
           tinydb_column_type_parse(create->col_types[0], &id) &&
           id.type == COL_TYPE_INT &&
           tinydb_column_type_parse(create->col_types[1], &username) &&
           username.type == COL_TYPE_VARCHAR &&
           tinydb_column_type_parse(create->col_types[2], &email) &&
           email.type == COL_TYPE_VARCHAR;
}

static bool legacy_fixed_row_shape(const CreateTableStatement* create) {
    if (!legacy_column_names_and_types(create)) return false;

    TinyDBColumnTypeSpec username;
    TinyDBColumnTypeSpec email;
    if (!tinydb_column_type_parse(create->col_types[1], &username) ||
        !tinydb_column_type_parse(create->col_types[2], &email)) {
        return false;
    }

    /*
     * Names alone are not an ABI. Historical bare VARCHAR schemas keep the
     * original Row executor, and explicit widths opt into that ABI only when
     * they exactly match the serialized Row fields. Any other explicit width
     * is a normal schema-aware generic layout even if the columns happen to be
     * named id/username/email.
     */
    if (!username.explicitly_sized && !email.explicitly_sized) return true;
    return username.explicitly_sized && email.explicitly_sized &&
           username.storage_size == USERNAME_SIZE &&
           email.storage_size == EMAIL_SIZE;
}

static bool generic_record_candidate(const CreateTableStatement* create) {
    TinyDBColumnTypeSpec id;
    return create->num_columns > 0u &&
           ci_equal(create->col_names[0], "id") &&
           tinydb_column_type_parse(create->col_types[0], &id) &&
           id.type == COL_TYPE_INT &&
           !legacy_fixed_row_shape(create);
}

static bool validate_generic_layout(const CreateTableStatement* create,
                                    char* message,
                                    size_t message_size) {
    uint32_t row_size = 0;
    for (uint32_t i = 0; i < create->num_columns; i++) {
        TinyDBColumnTypeSpec type;
        if (!tinydb_column_type_parse(create->col_types[i], &type)) {
            /* Preserve the historical metadata-only behavior for unsupported
             * type names. This wrapper only owns executable generic layouts. */
            return true;
        }

        if (row_size > TINYDB_RECORD_PAYLOAD_MAX ||
            type.storage_size > TINYDB_RECORD_PAYLOAD_MAX - row_size) {
            snprintf(message,
                     message_size,
                     "CREATE TABLE row layout exceeds the schema-sized payload limit");
            return false;
        }
        row_size += type.storage_size;
    }
    return true;
}

static TinyDBSqlStatus policy_error(TinyDBSqlResult* result,
                                    const char* message) {
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->status = TINYDB_SQL_POLICY_ERROR;
        result->prepare_result = PREPARE_SUCCESS;
        result->execute_result = EXECUTE_SUCCESS;
        result->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
        result->statement_type = STATEMENT_CREATE_TABLE;
        result->statement_type_valid = true;
        result->executed = false;
        snprintf(result->message, sizeof(result->message), "%s", message);
    }
    return TINYDB_SQL_POLICY_ERROR;
}

static TinyDBSqlStatus read_user_version_nonfatal(TinyDB* database,
                                                  TinyDBSqlResult* result) {
    TinyDBSqlResult local_result;
    TinyDBSqlResult* output = result != NULL ? result : &local_result;
    memset(output, 0, sizeof(*output));
    output->status = TINYDB_SQL_SUCCESS;
    output->prepare_result = PREPARE_SUCCESS;
    output->execute_result = EXECUTE_SUCCESS;
    output->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
    output->statement_type = STATEMENT_PRAGMA_USER_VERSION;
    output->statement_type_valid = true;

    Table* table = tinydb_table(database);
    if (table == NULL || table->pager == NULL) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        output->executed = false;
        snprintf(output->message,
                 sizeof(output->message),
                 "invalid database handle for PRAGMA user_version");
        return output->status;
    }

    uint32_t version = 0u;
    char message[TINYDB_ENGINE_MESSAGE_MAX];
    if (!db_try_get_user_version(table,
                                 &version,
                                 message,
                                 sizeof(message))) {
        output->status = TINYDB_SQL_EXECUTE_ERROR;
        output->executed = false;
        snprintf(output->message,
                 sizeof(output->message),
                 "PRAGMA user_version %.235s",
                 message[0] != '\0' ? message : "read failed");
        return output->status;
    }

    printf("%u\n", version);
    output->executed = true;
    return TINYDB_SQL_SUCCESS;
}

TinyDBSqlStatus tinydb_execute_sql(TinyDB* database,
                                   const char* sql,
                                   TinyDBSqlResult* result) {
    if (database != NULL && sql != NULL) {
        Statement statement;
        memset(&statement, 0, sizeof(statement));
        if (prepare_statement(sql, &statement) == PREPARE_SUCCESS) {
            if (statement.type == STATEMENT_PRAGMA_USER_VERSION) {
                return read_user_version_nonfatal(database, result);
            }

            if (statement.type == STATEMENT_CREATE_TABLE) {
                const CreateTableStatement* create = &statement.create_table;

                if (generic_record_candidate(create)) {
                    char message[TINYDB_ENGINE_MESSAGE_MAX];
                    if (!validate_generic_layout(create,
                                                 message,
                                                 sizeof(message))) {
                        return policy_error(result, message);
                    }
                }
            }
        }
    }

    return tinydb_execute_sql_base(database, sql, result);
}
