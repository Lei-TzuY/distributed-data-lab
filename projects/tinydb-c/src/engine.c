#include "engine.h"
#include "catalog_pragmas.h"
#include "diagnostics.h"
#include "generic_sql.h"
#include "join_plan.h"

#include <ctype.h>
#include <time.h>

static void initialize_result(TinyDBSqlResult* result) {
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_SQL_SUCCESS;
    result->prepare_result = PREPARE_SUCCESS;
    result->execute_result = EXECUTE_SUCCESS;
    result->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
}

static void set_result_message(TinyDBSqlResult* result, const char* message) {
    if (message == NULL) {
        result->message[0] = '\0';
        return;
    }
    snprintf(result->message, sizeof(result->message), "%s", message);
}

static bool ci_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL || name[0] == '\0') return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool schema_uses_fixed_row_shape(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3 &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool schema_uses_generic_fixed_slot(const TableSchema* schema) {
    if (schema == NULL || schema_uses_fixed_row_shape(schema)) return false;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    return tinydb_schema_supports_records(schema, message, sizeof(message));
}

static uint32_t schema_column_storage_size(const char* type) {
    if (ci_equal(type, "INT") || ci_equal(type, "INTEGER")) {
        return (uint32_t)sizeof(uint32_t);
    }
    return 256u;
}

static bool consume_ci_word(const char** input, const char* word) {
    const char* p = *input;
    const char* w = word;

    while (isspace((unsigned char)*p)) p++;
    while (*w != '\0' &&
           tolower((unsigned char)*p) == tolower((unsigned char)*w)) {
        p++;
        w++;
    }
    if (*w != '\0') return false;
    if (isalnum((unsigned char)*p) || *p == '_') return false;

    *input = p;
    return true;
}

static bool extract_explain_query(const char* sql,
                                  bool* analyze,
                                  const char** query) {
    const char* current = sql;
    if (!consume_ci_word(&current, "explain")) return false;

    *analyze = consume_ci_word(&current, "analyze");
    while (isspace((unsigned char)*current)) current++;
    *query = current;
    return true;
}

static TinyDBSqlStatus map_generic_result(TinyDBSqlResult* output,
                                          TinyDBGenericSqlStatus generic_status,
                                          const TinyDBGenericSqlResult* generic_result) {
    output->statement_type = generic_result->statement_type;
    output->statement_type_valid = generic_result->statement_type_valid;
    output->execute_result = generic_result->execute_result;
    output->executed = generic_result->executed;
    set_result_message(output, generic_result->message);

    if (generic_status == TINYDB_GENERIC_SQL_SUCCESS) {
        output->status = TINYDB_SQL_SUCCESS;
    } else if (generic_status == TINYDB_GENERIC_SQL_SYNTAX_ERROR) {
        output->prepare_result = PREPARE_SYNTAX_ERROR;
        output->status = TINYDB_SQL_SYNTAX_ERROR;
    } else if (generic_status == TINYDB_GENERIC_SQL_EXECUTE_ERROR) {
        output->status = TINYDB_SQL_EXECUTE_ERROR;
    }
    return output->status;
}

static TinyDBSqlStatus profile_cross_root_join(TinyDB* database,
                                                Statement* statement,
                                                TinyDBSqlResult* result,
                                                const TinyDBJoinPlan* plan) {
    Table* table = database->table;
    uint32_t hits_before = table->pager->cache_hits;
    uint32_t misses_before = table->pager->cache_misses;
    uint32_t evictions_before = table->pager->evictions;
    bool handled = false;
    ExecuteResult execute_result = EXECUTE_SUCCESS;

    memset(&result->profile, 0, sizeof(result->profile));
    result->has_profile = true;
    result->profile.plan_result = EXECUTE_SUCCESS;

    printf("QUERY PLAN\n");
    tinydb_print_join_plan(plan, &statement->select);
    printf("ACTUAL RESULT\n");

    clock_t started = clock();
    result->route_result = multitable_execute_join(
        table, statement, &handled, &execute_result);
    clock_t finished = clock();

    result->join_handled = handled;
    result->profile.execute_result = execute_result;
    result->profile.execution_time_ms =
        1000.0 * (double)(finished - started) / (double)CLOCKS_PER_SEC;
    result->profile.cache_hits = table->pager->cache_hits - hits_before;
    result->profile.cache_misses = table->pager->cache_misses - misses_before;
    result->profile.evictions = table->pager->evictions - evictions_before;
    result->profile.page_accesses =
        result->profile.cache_hits + result->profile.cache_misses;
    result->execute_result = execute_result;
    result->executed = handled &&
                       result->route_result == MULTITABLE_ROUTE_OK &&
                       execute_result == EXECUTE_SUCCESS;

    if (!handled || result->route_result != MULTITABLE_ROUTE_OK) {
        result->status = TINYDB_SQL_ROUTE_ERROR;
        set_result_message(result, multitable_route_error(result->route_result));
        return result->status;
    }
    if (execute_result != EXECUTE_SUCCESS) {
        result->status = TINYDB_SQL_EXECUTE_ERROR;
        set_result_message(result, "cross-root JOIN execution failed");
        return result->status;
    }

    result->status = TINYDB_SQL_SUCCESS;
    return result->status;
}

static TinyDBSqlStatus execute_generic_explain(TinyDB* database,
                                                const char* query,
                                                bool analyze,
                                                const TinyDBGenericSelectPlan* plan,
                                                TinyDBSqlResult* result) {
    result->statement_type = STATEMENT_SELECT;
    result->statement_type_valid = true;
    result->execute_result = EXECUTE_SUCCESS;

    if (!analyze) {
        tinydb_generic_sql_print_plan(plan);
        result->executed = true;
        result->status = TINYDB_SQL_SUCCESS;
        return result->status;
    }

    Table* table = database->table;
    uint32_t hits_before = table->pager->cache_hits;
    uint32_t misses_before = table->pager->cache_misses;
    uint32_t evictions_before = table->pager->evictions;

    memset(&result->profile, 0, sizeof(result->profile));
    result->has_profile = true;
    result->profile.plan_result = EXECUTE_SUCCESS;

    printf("QUERY PLAN\n");
    tinydb_generic_sql_print_plan(plan);
    printf("ACTUAL RESULT\n");

    TinyDBGenericSqlResult generic_result;
    clock_t started = clock();
    TinyDBGenericSqlStatus generic_status = tinydb_generic_sql_try_execute(
        table, query, &generic_result);
    clock_t finished = clock();

    result->profile.execution_time_ms =
        1000.0 * (double)(finished - started) / (double)CLOCKS_PER_SEC;
    result->profile.cache_hits = table->pager->cache_hits - hits_before;
    result->profile.cache_misses = table->pager->cache_misses - misses_before;
    result->profile.evictions = table->pager->evictions - evictions_before;
    result->profile.page_accesses =
        result->profile.cache_hits + result->profile.cache_misses;
    result->profile.execute_result = generic_result.execute_result;

    TinyDBSqlStatus mapped = map_generic_result(result,
                                                generic_status,
                                                &generic_result);
    if (mapped == TINYDB_SQL_SUCCESS) {
        result->executed = true;
    }
    return mapped;
}

static TinyDBSqlStatus execute_explain_analyze(TinyDB* database,
                                                const char* query,
                                                TinyDBSqlResult* result) {
    Statement statement;
    MultiTableRouteScope route_scope;

    result->prepare_result = prepare_statement(query, &statement);
    if (result->prepare_result != PREPARE_SUCCESS ||
        statement.type != STATEMENT_SELECT) {
        result->status = TINYDB_SQL_SYNTAX_ERROR;
        set_result_message(result,
                           "EXPLAIN ANALYZE currently requires a SELECT statement");
        return result->status;
    }

    result->statement_type = statement.type;
    result->statement_type_valid = true;

    if (statement.select.has_join) {
        TinyDBJoinPlan join_plan;
        result->route_result = tinydb_build_join_plan(
            database->table, &statement, &join_plan);
        if (join_plan.applicable) {
            if (result->route_result != MULTITABLE_ROUTE_OK) {
                result->status = TINYDB_SQL_ROUTE_ERROR;
                set_result_message(result, multitable_route_error(result->route_result));
                return result->status;
            }
            return profile_cross_root_join(database, &statement, result, &join_plan);
        }
    }

    result->route_result = multitable_begin_statement_scope(
        database->table, &statement, query, &route_scope);
    if (result->route_result != MULTITABLE_ROUTE_OK &&
        result->route_result != MULTITABLE_ROUTE_NOT_APPLICABLE) {
        result->status = TINYDB_SQL_ROUTE_ERROR;
        set_result_message(result, multitable_route_error(result->route_result));
        return result->status;
    }

    result->has_profile = query_profile_execute(
        &statement, database->table, &result->profile);
    multitable_end_statement_scope(database->table, &route_scope);

    if (!result->has_profile) {
        result->status = TINYDB_SQL_EXECUTE_ERROR;
        set_result_message(result, "EXPLAIN ANALYZE could not profile this statement");
        return result->status;
    }

    result->execute_result = result->profile.execute_result;
    result->executed = result->profile.execute_result == EXECUTE_SUCCESS;
    if (result->profile.plan_result != EXECUTE_SUCCESS ||
        result->profile.execute_result != EXECUTE_SUCCESS) {
        result->status = TINYDB_SQL_EXECUTE_ERROR;
        set_result_message(result, "EXPLAIN ANALYZE execution failed");
        return result->status;
    }

    result->status = TINYDB_SQL_SUCCESS;
    return result->status;
}

static TinyDBSqlStatus execute_catalog_integrity_check(Table* table,
                                                       TinyDBSqlResult* result) {
    bool ok = true;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];

    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        const TableSchema* schema = &table->catalog.schemas[i];
        for (uint32_t j = 0; j < i; j++) {
            if (table->catalog.schemas[j].root_page_num == schema->root_page_num) {
                printf("Error: Tables '%s' and '%s' share root page %u.\n",
                       table->catalog.schemas[j].name,
                       schema->name,
                       schema->root_page_num);
                ok = false;
            }
        }

        if (!tinydb_check_table_tree(table,
                                     schema->name,
                                     message,
                                     sizeof(message))) {
            printf("Error: Table '%s': %s\n", schema->name, message);
            ok = false;
        }
    }

    result->executed = true;
    result->execute_result = ok ? EXECUTE_SUCCESS : EXECUTE_KEY_NOT_FOUND;
    if (ok) {
        printf("ok\n");
        result->status = TINYDB_SQL_SUCCESS;
        return result->status;
    }

    result->status = TINYDB_SQL_EXECUTE_ERROR;
    set_result_message(result, "catalog B+ tree integrity check failed");
    return result->status;
}

TinyDB* tinydb_open(const char* filename) {
    if (filename == NULL || filename[0] == '\0' ||
        strlen(filename) >= TINYDB_ENGINE_FILENAME_MAX) {
        return NULL;
    }

    TinyDB* database = (TinyDB*)calloc(1, sizeof(*database));
    if (database == NULL) return NULL;

    snprintf(database->filename, sizeof(database->filename), "%s", filename);
    database->table = db_open(filename);
    if (database->table == NULL) {
        free(database);
        return NULL;
    }

    if (!multitable_catalog_load(database->table, database->filename)) {
        printf("Warning: schema catalog could not be loaded; using in-memory catalog state.\n");
    }
    return database;
}

void tinydb_close(TinyDB* database) {
    if (database == NULL) return;
    if (database->table != NULL) {
        db_close(database->table);
        database->table = NULL;
    }
    free(database);
}

Table* tinydb_table(TinyDB* database) {
    return database != NULL ? database->table : NULL;
}

const char* tinydb_filename(const TinyDB* database) {
    return database != NULL ? database->filename : NULL;
}

TinyDBSqlStatus tinydb_execute_sql(TinyDB* database,
                                   const char* sql,
                                   TinyDBSqlResult* result) {
    TinyDBSqlResult local_result;
    TinyDBSqlResult* output = result != NULL ? result : &local_result;
    initialize_result(output);

    if (database == NULL || database->table == NULL || sql == NULL) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output, "invalid database handle or SQL string");
        return output->status;
    }

    bool explain_analyze = false;
    const char* explain_query = NULL;
    if (extract_explain_query(sql, &explain_analyze, &explain_query)) {
        TinyDBGenericSelectPlan generic_plan;
        TinyDBGenericSqlResult generic_plan_result;
        TinyDBGenericSqlStatus generic_plan_status =
            tinydb_generic_sql_build_select_plan(database->table,
                                                 explain_query,
                                                 &generic_plan,
                                                 &generic_plan_result);
        if (generic_plan_status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) {
            if (generic_plan_status != TINYDB_GENERIC_SQL_SUCCESS) {
                return map_generic_result(output,
                                          generic_plan_status,
                                          &generic_plan_result);
            }
            return execute_generic_explain(database,
                                           explain_query,
                                           explain_analyze,
                                           &generic_plan,
                                           output);
        }

        if (explain_analyze) {
            return execute_explain_analyze(database, explain_query, output);
        }
    }

    TinyDBGenericSqlResult generic_result;
    TinyDBGenericSqlStatus generic_status = tinydb_generic_sql_try_execute(
        database->table, sql, &generic_result);
    if (generic_status != TINYDB_GENERIC_SQL_NOT_APPLICABLE) {
        return map_generic_result(output, generic_status, &generic_result);
    }

    Statement statement;
    output->prepare_result = prepare_statement(sql, &statement);
    if (output->prepare_result == PREPARE_SYNTAX_ERROR) {
        output->status = TINYDB_SQL_SYNTAX_ERROR;
        set_result_message(output, "syntax error");
        return output->status;
    }
    if (output->prepare_result == PREPARE_UNRECOGNIZED_STATEMENT) {
        output->status = TINYDB_SQL_UNRECOGNIZED_STATEMENT;
        set_result_message(output, "unrecognized statement");
        return output->status;
    }

    output->statement_type = statement.type;
    output->statement_type_valid = true;
    Table* table = database->table;

    if (statement.explain &&
        statement.type == STATEMENT_SELECT &&
        statement.select.has_join) {
        TinyDBJoinPlan join_plan;
        output->route_result = tinydb_build_join_plan(table, &statement, &join_plan);
        if (join_plan.applicable) {
            if (output->route_result != MULTITABLE_ROUTE_OK) {
                output->status = TINYDB_SQL_ROUTE_ERROR;
                set_result_message(output, multitable_route_error(output->route_result));
                return output->status;
            }
            tinydb_print_join_plan(&join_plan, &statement.select);
            output->executed = true;
            output->execute_result = EXECUTE_SUCCESS;
            output->status = TINYDB_SQL_SUCCESS;
            return output->status;
        }
    }

    if (statement.type == STATEMENT_PRAGMA_TABLE_INFO ||
        statement.type == STATEMENT_PRAGMA_INDEX_LIST) {
        CatalogPragmaResult pragma_result = tinydb_execute_catalog_pragma(
            table,
            statement.type,
            sql,
            output->message,
            sizeof(output->message));
        if (pragma_result == CATALOG_PRAGMA_SUCCESS) {
            output->executed = true;
            output->execute_result = EXECUTE_SUCCESS;
            output->status = TINYDB_SQL_SUCCESS;
            return output->status;
        }
        if (pragma_result == CATALOG_PRAGMA_TABLE_NOT_FOUND) {
            output->status = TINYDB_SQL_ROUTE_ERROR;
            return output->status;
        }
        if (pragma_result == CATALOG_PRAGMA_INVALID_TARGET) {
            output->status = TINYDB_SQL_SYNTAX_ERROR;
            return output->status;
        }
    }

    if (statement.type == STATEMENT_PRAGMA_INTEGRITY_CHECK &&
        table->catalog.num_tables > 1) {
        return execute_catalog_integrity_check(table, output);
    }

    if (multitable_is_schema_ddl(statement.type) && table->in_transaction) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output, "schema DDL is not allowed inside a transaction");
        return output->status;
    }

    if (table->catalog.num_tables > 1 && statement.type == STATEMENT_VACUUM) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output,
                           "VACUUM/VACUUM INTO is disabled for multi-table databases until compaction preserves every table root and schema sidecar");
        return output->status;
    }

    if (table->catalog.num_tables > 1 &&
        statement.type == STATEMENT_EXECUTE_PREPARED) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output,
                           "EXECUTE PREPARED is disabled for multi-table databases until bound SQL participates in root routing");
        return output->status;
    }

    if (table->catalog.num_tables > 1 &&
        statement.type == STATEMENT_ALTER_TABLE &&
        statement.alter_table.is_add_column) {
        TableSchema* target = find_schema(table, statement.alter_table.table_name);
        if (target != NULL &&
            !ci_equal(target->name, "users") &&
            schema_uses_fixed_row_shape(target)) {
            output->status = TINYDB_SQL_POLICY_ERROR;
            set_result_message(output,
                               "ALTER TABLE ADD COLUMN is disabled for executable fixed-Row table roots until physical row migration is implemented");
            return output->status;
        }

        if (target != NULL && schema_uses_generic_fixed_slot(target)) {
            uint32_t added_size = schema_column_storage_size(
                statement.alter_table.new_col_type);
            if (target->row_size > ROW_SIZE ||
                added_size > ROW_SIZE - target->row_size) {
                output->status = TINYDB_SQL_POLICY_ERROR;
                set_result_message(output,
                                   "ALTER TABLE ADD COLUMN would exceed the fixed generic record slot; variable-size row migration is not implemented");
                return output->status;
            }
        }
    }

    if (statement.type == STATEMENT_CREATE_INDEX &&
        !multitable_index_target_supported(table,
                                           statement.create_index.table_name)) {
        output->status = TINYDB_SQL_POLICY_ERROR;
        set_result_message(output,
                           "secondary indexes on non-primary table roots are not routed safely yet");
        return output->status;
    }

    if (!statement.explain &&
        statement.type == STATEMENT_SELECT &&
        statement.select.has_join) {
        bool handled = false;
        ExecuteResult join_execute_result = EXECUTE_SUCCESS;
        output->route_result = multitable_execute_join(
            table, &statement, &handled, &join_execute_result);
        output->join_handled = handled;
        if (handled) {
            if (output->route_result != MULTITABLE_ROUTE_OK) {
                output->status = TINYDB_SQL_ROUTE_ERROR;
                set_result_message(output,
                                   multitable_route_error(output->route_result));
                return output->status;
            }
            output->execute_result = join_execute_result;
            output->executed = join_execute_result == EXECUTE_SUCCESS;
            if (join_execute_result != EXECUTE_SUCCESS) {
                output->status = TINYDB_SQL_EXECUTE_ERROR;
                set_result_message(output, "JOIN execution failed");
                return output->status;
            }
            output->status = TINYDB_SQL_SUCCESS;
            return output->status;
        }
    }

    MultiTableRouteScope route_scope;
    output->route_result = multitable_begin_statement_scope(
        table, &statement, sql, &route_scope);
    if (output->route_result != MULTITABLE_ROUTE_OK &&
        output->route_result != MULTITABLE_ROUTE_NOT_APPLICABLE) {
        output->status = TINYDB_SQL_ROUTE_ERROR;
        set_result_message(output, multitable_route_error(output->route_result));
        return output->status;
    }

    if (statement.type == STATEMENT_DELETE &&
        statement.delete_all &&
        route_scope.active &&
        table->catalog.num_tables > 1) {
        output->execute_result = multitable_execute_delete_all(
            &statement, table, &route_scope);
    } else {
        output->execute_result = execute_statement(&statement, table);
    }
    multitable_end_statement_scope(table, &route_scope);
    output->executed = true;

    if (output->execute_result != EXECUTE_SUCCESS) {
        output->status = TINYDB_SQL_EXECUTE_ERROR;
        return output->status;
    }

    if (multitable_is_schema_ddl(statement.type)) {
        db_checkpoint(table);
        if (!multitable_catalog_save(table, database->filename)) {
            output->status = TINYDB_SQL_CATALOG_PERSIST_ERROR;
            set_result_message(output, "schema catalog could not be persisted");
            return output->status;
        }
        output->schema_persisted = true;
    }

    output->status = TINYDB_SQL_SUCCESS;
    return output->status;
}

const char* tinydb_sql_status_string(TinyDBSqlStatus status) {
    switch (status) {
        case TINYDB_SQL_SUCCESS:
            return "success";
        case TINYDB_SQL_SYNTAX_ERROR:
            return "syntax error";
        case TINYDB_SQL_UNRECOGNIZED_STATEMENT:
            return "unrecognized statement";
        case TINYDB_SQL_POLICY_ERROR:
            return "policy error";
        case TINYDB_SQL_ROUTE_ERROR:
            return "route error";
        case TINYDB_SQL_EXECUTE_ERROR:
            return "execution error";
        case TINYDB_SQL_CATALOG_PERSIST_ERROR:
            return "catalog persistence error";
    }
    return "unknown";
}
