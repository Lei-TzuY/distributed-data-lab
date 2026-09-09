#include "column_type.h"
#include "engine.h"
#include "generic_index_epoch.h"
#include "leaf_format.h"
#include "leaf_migration.h"
#include "multitable.h"
#include "record.h"
#include "record_payload.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TinyDBSqlStatus tinydb_execute_sql_alter_delegate_base(
    TinyDB* database,
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

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool schema_has_column(const TableSchema* schema, const char* name) {
    if (schema == NULL || name == NULL) return false;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (ci_equal(schema->columns[i].name, name)) return true;
    }
    return false;
}

static bool test_fail_before_catalog_persist(void) {
    const char* value = getenv("TINYDB_TEST_FAIL_ALTER_BEFORE_CATALOG_PERSIST");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool test_fail_after_storage_migration(void) {
    const char* value = getenv("TINYDB_TEST_FAIL_ALTER_AFTER_STORAGE_MIGRATION");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool test_fail_catalog_save(void) {
    const char* value = getenv("TINYDB_TEST_FAIL_ALTER_CATALOG_SAVE");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool fixed_row_shape(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3u &&
           schema->row_size == ROW_SIZE &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR &&
           schema->columns[0].offset == ID_OFFSET &&
           schema->columns[0].size == ID_SIZE &&
           schema->columns[1].offset == USERNAME_OFFSET &&
           schema->columns[1].size == USERNAME_SIZE &&
           schema->columns[2].offset == EMAIL_OFFSET &&
           schema->columns[2].size == EMAIL_SIZE;
}

static void initialize_result(TinyDBSqlResult* result, TinyDBSqlStatus status) {
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->status = status;
    result->prepare_result = PREPARE_SUCCESS;
    result->execute_result = EXECUTE_SUCCESS;
    result->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
    result->statement_type = STATEMENT_ALTER_TABLE;
    result->statement_type_valid = true;
}

static TinyDBSqlStatus fail_result(TinyDBSqlResult* result,
                                   TinyDBSqlStatus status,
                                   const char* message) {
    initialize_result(result, status);
    if (result != NULL) {
        result->executed = false;
        snprintf(result->message, sizeof(result->message), "%s", message);
    }
    return status;
}

static bool schema_rows_accept_appended_column(
    Table* table,
    const TableSchema* schema,
    const TinyDBColumnTypeSpec* type,
    char* message,
    size_t message_size) {
    if (table == NULL || schema == NULL || type == NULL ||
        schema->num_columns >= MAX_COLUMNS_PER_TABLE ||
        schema->row_size > TINYDB_RECORD_PAYLOAD_MAX ||
        type->storage_size > TINYDB_RECORD_PAYLOAD_MAX - schema->row_size) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "%s",
                     "prospective schema is outside append-compatible bounds");
        }
        return false;
    }

    TableSchema prospective = *schema;
    TableColumn* added = &prospective.columns[prospective.num_columns];
    memset(added, 0, sizeof(*added));
    added->type = type->type;
    added->offset = schema->row_size;
    added->size = type->storage_size;
    prospective.num_columns++;
    prospective.row_size += type->storage_size;

    bool scan_complete = false;
    (void)tinydb_record_payload_scan(table,
                                     &prospective,
                                     NULL,
                                     NULL,
                                     &scan_complete,
                                     message,
                                     message_size);
    return scan_complete;
}

TinyDBSqlStatus tinydb_execute_sql_prepared_delegate_base(
    TinyDB* database,
    const char* sql,
    TinyDBSqlResult* result) {
    if (database == NULL || database->table == NULL || sql == NULL) {
        return tinydb_execute_sql_alter_delegate_base(database, sql, result);
    }

    Statement statement;
    memset(&statement, 0, sizeof(statement));
    if (prepare_statement(sql, &statement) != PREPARE_SUCCESS ||
        statement.type != STATEMENT_ALTER_TABLE ||
        !statement.alter_table.is_add_column) {
        return tinydb_execute_sql_alter_delegate_base(database, sql, result);
    }

    TinyDBColumnTypeSpec type;
    if (!tinydb_column_type_parse(statement.alter_table.new_col_type, &type) ||
        !((type.type == COL_TYPE_VARCHAR && type.explicitly_sized) ||
          type.type == COL_TYPE_INT)) {
        return tinydb_execute_sql_alter_delegate_base(database, sql, result);
    }

    Table* table = database->table;
    if (table->in_transaction) {
        return fail_result(result,
                           TINYDB_SQL_POLICY_ERROR,
                           "schema DDL is not allowed inside a transaction");
    }

    TableSchema* target = find_schema(table, statement.alter_table.table_name);
    if (target == NULL) {
        return tinydb_execute_sql_alter_delegate_base(database, sql, result);
    }
    if (ci_equal(target->name, "users") || fixed_row_shape(target)) {
        return fail_result(
            result,
            TINYDB_SQL_POLICY_ERROR,
            "ALTER TABLE ADD COLUMN is disabled for executable fixed-Row table roots until physical row migration is implemented");
    }

    if (schema_has_column(target, statement.alter_table.new_col_name)) {
        return fail_result(result,
                           TINYDB_SQL_POLICY_ERROR,
                           "ALTER TABLE ADD COLUMN cannot reuse an existing column name");
    }

    if (target->row_size > TINYDB_RECORD_PAYLOAD_MAX ||
        type.storage_size > TINYDB_RECORD_PAYLOAD_MAX - target->row_size) {
        return fail_result(
            result,
            TINYDB_SQL_POLICY_ERROR,
            "ALTER TABLE ADD COLUMN would exceed the schema-sized payload limit");
    }

    if (target->row_size > ROW_SIZE) {
        char scan_message[TINYDB_RECORD_MESSAGE_MAX];
        if (!schema_rows_accept_appended_column(table,
                                                target,
                                                &type,
                                                scan_message,
                                                sizeof(scan_message))) {
            return fail_result(
                result,
                TINYDB_SQL_POLICY_ERROR,
                "ALTER TABLE ADD COLUMN requires append-compatible compact V2 rows for a non-empty schema-sized payload table");
        }
    }

    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    bool executable_generic = tinydb_schema_supports_records(
        target, schema_message, sizeof(schema_message));
    bool staged_fixed_root_migration = false;
    unsigned char staged_root[PAGE_SIZE];
    memset(staged_root, 0, sizeof(staged_root));

    if (executable_generic && target->row_size <= ROW_SIZE &&
        type.storage_size > ROW_SIZE - target->row_size) {
        if (target->root_page_num >= table->pager->num_pages) {
            return fail_result(result,
                               TINYDB_SQL_POLICY_ERROR,
                               "ALTER TABLE ADD COLUMN cannot migrate an invalid table root");
        }

        void* root = get_page(table->pager, target->root_page_num);
        TinyDBLeafPageFormat root_format =
            tinydb_leaf_format_detect_page(root, PAGE_SIZE);
        if (root_format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
            if (!tinydb_leaf_migrate_v1_to_compact_v2(root,
                                                       PAGE_SIZE,
                                                       target,
                                                       staged_root,
                                                       sizeof(staged_root))) {
                return fail_result(
                    result,
                    TINYDB_SQL_POLICY_ERROR,
                    "ALTER TABLE ADD COLUMN cannot migrate this fixed root leaf to compact V2 without a split");
            }
            staged_fixed_root_migration = true;
        } else if (root_format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
                   (get_node_type(root) == NODE_INTERNAL &&
                    is_node_root(root) && *node_parent(root) == 0u)) {
            char scan_message[TINYDB_RECORD_MESSAGE_MAX];
            if (!schema_rows_accept_appended_column(table,
                                                    target,
                                                    &type,
                                                    scan_message,
                                                    sizeof(scan_message))) {
                return fail_result(
                    result,
                    TINYDB_SQL_POLICY_ERROR,
                    "ALTER TABLE ADD COLUMN requires append-compatible compact V2 rows before crossing the fixed record boundary");
            }
        } else {
            return fail_result(
                result,
                TINYDB_SQL_POLICY_ERROR,
                "ALTER TABLE ADD COLUMN requires a table-rebuild migration for multi-leaf fixed storage");
        }
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, target)) {
        return fail_result(result,
                           TINYDB_SQL_EXECUTE_ERROR,
                           "ALTER TABLE ADD COLUMN could not invalidate generic index snapshots");
    }

    TableSchema schema_before_alter = *target;
    if (staged_fixed_root_migration) {
        void* root = get_page(table->pager, target->root_page_num);
        memcpy(root, staged_root, PAGE_USABLE_SIZE);
        mark_page_dirty(table->pager, target->root_page_num);

        /* The storage format upgrade is backward-readable with the old schema.
         * Checkpoint it before the test interruption so recovery exercises the
         * real crash window where physical migration is durable but catalog
         * mutation has not started. */
        if (test_fail_after_storage_migration()) {
            db_checkpoint(table);
            return fail_result(result,
                               TINYDB_SQL_CATALOG_PERSIST_ERROR,
                               "ALTER TABLE ADD COLUMN interrupted after physical row migration before schema mutation");
        }
    }

    if (!table_add_column(table,
                          statement.alter_table.table_name,
                          statement.alter_table.new_col_name,
                          statement.alter_table.new_col_type)) {
        return fail_result(result,
                           TINYDB_SQL_EXECUTE_ERROR,
                           "ALTER TABLE ADD COLUMN failed");
    }

    /* A compact-V2 physical upgrade is intentionally not rolled back here.
     * It is backward-readable through schema_before_alter and therefore remains
     * a safe, forward-compatible storage upgrade if catalog publication fails. */
    if (test_fail_before_catalog_persist()) {
        *target = schema_before_alter;
        return fail_result(result,
                           TINYDB_SQL_CATALOG_PERSIST_ERROR,
                           "ALTER TABLE ADD COLUMN interrupted before schema catalog publication");
    }

    db_checkpoint(table);
    if (test_fail_catalog_save() ||
        !multitable_catalog_save(table, database->filename)) {
        *target = schema_before_alter;
        return fail_result(result,
                           TINYDB_SQL_CATALOG_PERSIST_ERROR,
                           "schema catalog could not be persisted");
    }

    printf("Column '%s' added to table '%s'.\n",
           statement.alter_table.new_col_name,
           statement.alter_table.table_name);

    initialize_result(result, TINYDB_SQL_SUCCESS);
    if (result != NULL) {
        result->executed = true;
        result->schema_persisted = true;
    }
    return TINYDB_SQL_SUCCESS;
}
