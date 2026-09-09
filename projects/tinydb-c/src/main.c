#include "common.h"
#include "diagnostics.h"
#include "engine.h"
#include "table.h"

#include <ctype.h>

typedef struct {
    char* buffer;
    size_t buffer_length;
    size_t input_length;
} InputBuffer;

InputBuffer* new_input_buffer(void) {
    InputBuffer* input_buffer = (InputBuffer*)malloc(sizeof(InputBuffer));
    input_buffer->buffer = NULL;
    input_buffer->buffer_length = 0;
    input_buffer->input_length = 0;
    return input_buffer;
}

void print_prompt(void) { printf("db > "); }

void read_input(InputBuffer* input_buffer) {
    size_t capacity = 1024;
    if (input_buffer->buffer == NULL) {
        input_buffer->buffer = (char*)malloc(capacity);
        input_buffer->buffer_length = capacity;
    }

    if (fgets(input_buffer->buffer, (int)input_buffer->buffer_length, stdin) == NULL) {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
    }

    input_buffer->input_length = strlen(input_buffer->buffer);
    if (input_buffer->input_length > 0 &&
        input_buffer->buffer[input_buffer->input_length - 1] == '\n') {
        input_buffer->buffer[input_buffer->input_length - 1] = '\0';
        input_buffer->input_length--;
    }
}

void close_input_buffer(InputBuffer* input_buffer) {
    free(input_buffer->buffer);
    free(input_buffer);
}

typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

static const char* skip_spaces(const char* input) {
    while (isspace((unsigned char)*input)) input++;
    return input;
}

static const char* meta_argument(const char* input, const char* command) {
    size_t command_length = strlen(command);
    if (strncmp(input, command, command_length) != 0) return NULL;
    const char* argument = input + command_length;
    if (*argument != '\0' && !isspace((unsigned char)*argument)) return NULL;
    return skip_spaces(argument);
}

static void print_schema(Table* table, const TableSchema* schema) {
    if (schema == NULL) return;
    printf("Table: %s (root page %u)\n", schema->name, schema->root_page_num);
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        if (column->type == COL_TYPE_INT) {
            printf("  %-16s INT          offset=%u size=%u%s\n",
                   column->name,
                   column->offset,
                   column->size,
                   i == 0 ? " PRIMARY KEY" : "");
        } else {
            printf("  %-16s VARCHAR      offset=%u size=%u\n",
                   column->name,
                   column->offset,
                   column->size);
        }
    }
    if (strcmp(schema->name, "users") == 0 && table->username_index_enabled) {
        printf("Index: idx_users_username ON users(username)\n");
    }
}

static bool get_table_stats(Table* table,
                            const char* table_name,
                            TinyDBTreeStats* stats) {
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_get_tree_stats_diagnostic(table,
                                          table_name,
                                          stats,
                                          message,
                                          sizeof(message))) {
        printf("Error: Unable to inspect table '%s': %s.\n",
               table_name,
               message[0] != '\0' ? message : "tree statistics unavailable");
        return false;
    }
    return true;
}

static void print_table_stats_values(const char* table_name,
                                     const TinyDBTreeStats* stats) {
    printf("Table: %s\n", table_name);
    printf("  Root Page: %u\n", stats->root_page_num);
    printf("  Height: %u\n", stats->height);
    printf("  Rows: %u\n", stats->total_rows);
    printf("  Leaf Pages: %u\n", stats->leaf_pages);
    printf("  Internal Pages: %u\n", stats->internal_pages);
}

static void print_table_stats(Table* table, const char* table_name) {
    TinyDBTreeStats stats;
    if (!get_table_stats(table, table_name, &stats)) return;
    print_table_stats_values(table_name, &stats);
}

static bool print_global_stats(Table* table) {
    TinyDBCatalogTreeStatsSnapshot snapshot;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_get_catalog_tree_stats_snapshot(table,
                                                &snapshot,
                                                message,
                                                sizeof(message))) {
        printf("Error: Unable to inspect database tree statistics: %s.\n",
               message[0] != '\0'
                   ? message
                   : "catalog tree statistics unavailable");
        return false;
    }

    const TinyDBDatabaseTreeStats* stats = &snapshot.aggregate;
    /* Keep the long-standing field names for scripts/tests while extending
     * their meaning to all catalog roots in a multi-table database. */
    printf("Total Pages: %u\n", table->pager->num_pages);
    printf("Leaf Pages: %u\n", stats->leaf_pages);
    printf("Internal Pages: %u\n", stats->internal_pages);
    printf("Free Pages: %u\n", table->pager->free_page_count);
    printf("Total Rows: %u\n", stats->total_rows);
    printf("In Transaction: %s\n", table->in_transaction ? "Yes" : "No");
    printf("Secondary Index: %s\n", table->username_index_enabled ? "Enabled" : "Disabled");

    if (stats->table_count > 1u) {
        for (uint32_t i = 0u; i < stats->table_count; i++) {
            print_table_stats_values(table->catalog.schemas[i].name,
                                     &snapshot.table_stats[i]);
        }
    }
    return true;
}

static void check_table(Table* table, const char* table_name) {
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    bool ok = tinydb_check_table_tree(table, table_name, message, sizeof(message));
    printf("%s: %s%s\n", table_name, ok ? "" : "ERROR: ", message);
}

static void check_all_tables(Table* table) {
    TinyDBCatalogTreeCheck check;
    char message[TINYDB_DIAGNOSTIC_MESSAGE_MAX];
    if (!tinydb_check_catalog_trees(table, &check, message, sizeof(message))) {
        printf("all: ERROR: %s\n",
               message[0] != '\0' ? message : "catalog tree validation failed");
        return;
    }

    for (uint32_t i = 0u; i < check.table_count; i++) {
        const TinyDBTreeStats* stats = &check.table_stats[i];
        printf("%s: ok: root=%u height=%u rows=%u leaf_pages=%u internal_pages=%u ownership_owned=%u free=%u\n",
               table->catalog.schemas[i].name,
               stats->root_page_num,
               stats->height,
               stats->total_rows,
               stats->leaf_pages,
               stats->internal_pages,
               check.ownership.owned_pages,
               check.ownership.free_pages);
    }
}

MetaCommandResult do_meta_command(InputBuffer* input_buffer, TinyDB* database) {
    Table* table = tinydb_table(database);
    const char* argument;

    if (strcmp(input_buffer->buffer, ".exit") == 0) {
        close_input_buffer(input_buffer);
        tinydb_close(database);
        exit(EXIT_SUCCESS);
    }

    argument = meta_argument(input_buffer->buffer, ".btree");
    if (argument != NULL) {
        const char* table_name = argument[0] != '\0' ? argument : "users";
        const TableSchema* schema = tinydb_find_table_schema(table, table_name);
        if (schema == NULL) {
            printf("Error: Table '%s' not found.\n", table_name);
            return META_COMMAND_SUCCESS;
        }
        printf("B+ tree for %s (root page %u):\n", schema->name, schema->root_page_num);
        print_tree(table->pager, schema->root_page_num, 0);
        return META_COMMAND_SUCCESS;
    }

    if (strcmp(input_buffer->buffer, ".constants") == 0) {
        printf("PAGE_SIZE: %d\n", PAGE_SIZE);
        printf("ROW_SIZE: %d\n", (int)ROW_SIZE);
        printf("COMMON_NODE_HEADER_SIZE: %d\n", (int)COMMON_NODE_HEADER_SIZE);
        printf("LEAF_NODE_HEADER_SIZE: %d\n", (int)LEAF_NODE_HEADER_SIZE);
        printf("LEAF_NODE_CELL_SIZE: %d\n", (int)LEAF_NODE_CELL_SIZE);
        printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", (int)LEAF_NODE_SPACE_FOR_CELLS);
        printf("LEAF_NODE_MAX_CELLS: %d\n", (int)LEAF_NODE_MAX_CELLS);
        printf("INTERNAL_NODE_MAX_KEYS: %d\n", (int)INTERNAL_NODE_MAX_KEYS);
        return META_COMMAND_SUCCESS;
    }

    argument = meta_argument(input_buffer->buffer, ".stats");
    if (argument != NULL) {
        if (argument[0] != '\0') {
            print_table_stats(table, argument);
        } else {
            (void)print_global_stats(table);
        }
        return META_COMMAND_SUCCESS;
    }

    if (strncmp(input_buffer->buffer, ".page", 5) == 0) {
        uint32_t page_num = 0;
        if (sscanf(input_buffer->buffer, ".page %u", &page_num) == 1) {
            print_page(table, page_num);
            return META_COMMAND_SUCCESS;
        }
        printf("Usage: .page <page_num>\n");
        return META_COMMAND_SUCCESS;
    }

    argument = meta_argument(input_buffer->buffer, ".schema");
    if (argument != NULL) {
        if (argument[0] != '\0') {
            const TableSchema* schema = tinydb_find_table_schema(table, argument);
            if (schema == NULL) {
                printf("Error: Table '%s' not found.\n", argument);
            } else {
                print_schema(table, schema);
            }
        } else {
            for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
                print_schema(table, &table->catalog.schemas[i]);
            }
        }
        return META_COMMAND_SUCCESS;
    }

    if (strcmp(input_buffer->buffer, ".tables") == 0) {
        table_print_tables(table);
        return META_COMMAND_SUCCESS;
    }

    if (strcmp(input_buffer->buffer, ".checkpoint") == 0) {
        db_checkpoint(table);
        return META_COMMAND_SUCCESS;
    }

    argument = meta_argument(input_buffer->buffer, ".check");
    if (argument != NULL) {
        if (argument[0] != '\0' && strcmp(argument, "all") != 0) {
            check_table(table, argument);
        } else {
            check_all_tables(table);
        }
        return META_COMMAND_SUCCESS;
    }

    if (strcmp(input_buffer->buffer, ".buffer_pool") == 0 ||
        strcmp(input_buffer->buffer, ".cache") == 0) {
        pager_print_buffer_pool_stats(table->pager);
        return META_COMMAND_SUCCESS;
    }

    if (strcmp(input_buffer->buffer, ".help") == 0) {
        printf("Meta commands:\n");
        printf("  .tables               List tables\n");
        printf("  .schema [table]       Show catalog-backed table schema(s)\n");
        printf("  .btree [table]        Print one table B+ tree (default: users)\n");
        printf("  .stats [table]        Show global or per-root B+ tree statistics\n");
        printf("  .check [table|all]    Validate one or every catalog B+ tree root\n");
        printf("  .constants            Show database engine constants\n");
        printf("  .buffer_pool / .cache Display Buffer Pool Manager & LRU eviction stats\n");
        printf("  .page <n>             Inspect physical page <n> details\n");
        printf("  .checkpoint           Flush WAL frames to main database file\n");
        printf("  .help                 Show this help\n");
        printf("  .exit                 Exit\n");
        printf("\nSQL statements:\n");
        printf("  CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);\n");
        printf("  INSERT INTO users VALUES (id, 'username', 'email');\n");
        printf("  SELECT * FROM users;\n");
        printf("  SELECT * FROM users JOIN archive ON users.id = archive.id;\n");
        printf("  SELECT * FROM users WHERE [id = N] [AND username LIKE 'p%%'] [AND email LIKE '%%s'];\n");
        printf("  SELECT COUNT(*)|MIN(id)|MAX(id)|SUM(id)|AVG(id) FROM users;\n");
        printf("  CREATE INDEX idx_users_username ON users(username);\n");
        printf("  DROP INDEX idx_users_username;\n");
        printf("  UPDATE users SET username = 'x' [, email = 'y'] WHERE id = N;\n");
        printf("  DELETE FROM users WHERE id = N;\n");
        printf("  DELETE FROM users;\n");
        printf("  VACUUM;\n");
        printf("  BEGIN;  COMMIT;  ROLLBACK;\n");
        printf("  SAVEPOINT sp1;  ROLLBACK TO sp1;  RELEASE sp1;\n");
        printf("  CHECKPOINT;\n");
        printf("  PRAGMA integrity_check;\n");
        printf("  PRAGMA table_info; / PRAGMA table_info(users);\n");
        printf("  PRAGMA index_list; / PRAGMA index_list(users);\n");
        printf("  PRAGMA user_version; / PRAGMA user_version = N;\n");
        printf("  EXPLAIN SELECT ...;\n");
        printf("  EXPLAIN ANALYZE SELECT ...;\n");
        return META_COMMAND_SUCCESS;
    }

    return META_COMMAND_UNRECOGNIZED_COMMAND;
}

static void print_execute_result(const TinyDBSqlResult* result, const char* sql) {
    if (result->has_profile) {
        printf("ANALYZE: execution_time_ms=%.3f cache_hits=%u cache_misses=%u evictions=%u page_accesses=%u\n",
               result->profile.execution_time_ms,
               result->profile.cache_hits,
               result->profile.cache_misses,
               result->profile.evictions,
               result->profile.page_accesses);
    }

    if (result->status == TINYDB_SQL_SUCCESS) {
        printf("Executed.\n");
        return;
    }

    if (result->status == TINYDB_SQL_SYNTAX_ERROR) {
        printf("Syntax error. Could not parse statement.\n");
        return;
    }
    if (result->status == TINYDB_SQL_UNRECOGNIZED_STATEMENT) {
        printf("Unrecognized keyword at start of '%s'.\n", sql);
        return;
    }
    if (result->status == TINYDB_SQL_POLICY_ERROR ||
        result->status == TINYDB_SQL_ROUTE_ERROR ||
        result->status == TINYDB_SQL_CATALOG_PERSIST_ERROR) {
        printf("Error: %s.\n", result->message[0] != '\0'
               ? result->message
               : tinydb_sql_status_string(result->status));
        return;
    }

    /* Generic INSERT validation failures historically reuse the legacy
     * EXECUTE_KEY_NOT_FOUND enum because ExecuteResult has no validation
     * variant. Preserve the precise engine message instead of masking it as
     * a missing-key error. Duplicate-key INSERTs keep their dedicated enum. */
    if (result->status == TINYDB_SQL_EXECUTE_ERROR &&
        result->statement_type_valid &&
        result->statement_type == STATEMENT_INSERT &&
        result->execute_result == EXECUTE_KEY_NOT_FOUND &&
        result->message[0] != '\0') {
        printf("Error: %s.\n", result->message);
        return;
    }

    switch (result->execute_result) {
        case EXECUTE_TABLE_FULL:
            printf("Error: Table full.\n");
            break;
        case EXECUTE_DUPLICATE_KEY:
            printf("Error: Duplicate key.\n");
            break;
        case EXECUTE_KEY_NOT_FOUND:
            printf("Error: Key not found.\n");
            break;
        case EXECUTE_TRANSACTION_ALREADY_ACTIVE:
            printf("Error: Transaction already active.\n");
            break;
        case EXECUTE_NO_ACTIVE_TRANSACTION:
            printf("Error: No active transaction.\n");
            break;
        case EXECUTE_DDL_INSIDE_TRANSACTION:
            printf("Error: Index DDL is not allowed inside a transaction.\n");
            break;
        case EXECUTE_SAVEPOINT_NOT_FOUND:
            printf("Error: Savepoint not found.\n");
            break;
        case EXECUTE_SAVEPOINT_STACK_FULL:
            printf("Error: Savepoint stack full.\n");
            break;
        case EXECUTE_SUCCESS:
            printf("Error: %s.\n", result->message[0] != '\0'
                   ? result->message
                   : "execution failed");
            break;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        return EXIT_FAILURE;
    }

    TinyDB* database = tinydb_open(argv[1]);
    if (database == NULL) {
        printf("Unable to open database.\n");
        return EXIT_FAILURE;
    }

    InputBuffer* input_buffer = new_input_buffer();
    while (true) {
        print_prompt();
        read_input(input_buffer);

        if (input_buffer->input_length == 0) continue;

        if (input_buffer->buffer[0] == '.') {
            switch (do_meta_command(input_buffer, database)) {
                case META_COMMAND_SUCCESS:
                    continue;
                case META_COMMAND_UNRECOGNIZED_COMMAND:
                    printf("Unrecognized command '%s'\n", input_buffer->buffer);
                    continue;
            }
        }

        TinyDBSqlResult result;
        (void)tinydb_execute_sql(database, input_buffer->buffer, &result);
        print_execute_result(&result, input_buffer->buffer);
    }
}