#include "multitable.h"

#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define SCHEMA_CATALOG_MAGIC 0x4d435354u /* TSCM */
#define SCHEMA_CATALOG_VERSION 1u
#define SCHEMA_CATALOG_WAL_COMMIT_MAGIC 0x57435354u /* TSCW */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_tables;
    uint32_t num_views;
    TableSchema schemas[MAX_TABLES];
    ViewSchema views[MAX_VIEWS];
} SchemaCatalogDisk;

static bool sync_catalog_file(FILE* file) {
    if (fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static bool build_catalog_path(char* output,
                               size_t output_size,
                               const char* database_filename,
                               const char* suffix) {
    int written = snprintf(output, output_size, "%s%s", database_filename, suffix);
    return written >= 0 && (size_t)written < output_size;
}

static bool schema_catalog_valid(const SchemaCatalogDisk* disk) {
    return disk->magic == SCHEMA_CATALOG_MAGIC &&
           disk->version == SCHEMA_CATALOG_VERSION &&
           disk->num_tables > 0 &&
           disk->num_tables <= MAX_TABLES &&
           disk->num_views <= MAX_VIEWS;
}

static bool read_schema_catalog_payload(FILE* file, SchemaCatalogDisk* disk) {
    return fread(disk, sizeof(*disk), 1, file) == 1 && schema_catalog_valid(disk);
}

static bool write_schema_catalog_payload(FILE* file, const SchemaCatalogDisk* disk) {
    return fwrite(disk, sizeof(*disk), 1, file) == 1;
}

static bool write_schema_catalog_file(const char* path, const SchemaCatalogDisk* disk) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = write_schema_catalog_payload(file, disk);
    if (ok) ok = sync_catalog_file(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static void fill_schema_catalog_disk(Table* table, SchemaCatalogDisk* disk) {
    memset(disk, 0, sizeof(*disk));
    disk->magic = SCHEMA_CATALOG_MAGIC;
    disk->version = SCHEMA_CATALOG_VERSION;
    disk->num_tables = table->catalog.num_tables;
    disk->num_views = table->catalog.num_views;
    memcpy(disk->schemas,
           table->catalog.schemas,
           sizeof(TableSchema) * table->catalog.num_tables);
    memcpy(disk->views,
           table->catalog.views,
           sizeof(ViewSchema) * table->catalog.num_views);
}

static bool recover_schema_catalog(const char* main_path, const char* wal_path) {
    FILE* wal = fopen(wal_path, "rb");
    if (wal == NULL) return true;

    SchemaCatalogDisk disk;
    uint32_t commit_magic = 0;
    bool complete = read_schema_catalog_payload(wal, &disk) &&
                    fread(&commit_magic, sizeof(commit_magic), 1, wal) == 1;
    fclose(wal);

    if (!complete || commit_magic != SCHEMA_CATALOG_WAL_COMMIT_MAGIC) {
        (void)remove(wal_path);
        printf("Ignoring incomplete schema catalog WAL.\n");
        return true;
    }

    if (!write_schema_catalog_file(main_path, &disk)) {
        printf("Unable to recover schema catalog.\n");
        return false;
    }
    if (remove(wal_path) != 0 && errno != ENOENT) {
        printf("Unable to remove recovered schema catalog WAL.\n");
        return false;
    }
    printf("Schema catalog recovery complete.\n");
    return true;
}

bool multitable_catalog_load(Table* table, const char* database_filename) {
    char main_path[768];
    char wal_path[768];
    if (!build_catalog_path(main_path, sizeof(main_path), database_filename, ".schema") ||
        !build_catalog_path(wal_path, sizeof(wal_path), database_filename, ".schema.wal")) {
        printf("Database filename too long for schema catalog.\n");
        return false;
    }

    if (!recover_schema_catalog(main_path, wal_path)) return false;

    FILE* file = fopen(main_path, "rb");
    if (file == NULL) return true;

    SchemaCatalogDisk disk;
    bool ok = read_schema_catalog_payload(file, &disk);
    fclose(file);
    if (!ok) {
        printf("Ignoring invalid schema catalog.\n");
        return false;
    }

    for (uint32_t i = 0; i < disk.num_tables; i++) {
        if (disk.schemas[i].root_page_num >= table->pager->num_pages) {
            printf("Ignoring schema catalog with invalid root page %u for table '%s'.\n",
                   disk.schemas[i].root_page_num,
                   disk.schemas[i].name);
            return false;
        }
    }

    table->catalog.num_tables = disk.num_tables;
    memcpy(table->catalog.schemas,
           disk.schemas,
           sizeof(TableSchema) * disk.num_tables);
    table->catalog.num_views = disk.num_views;
    memcpy(table->catalog.views,
           disk.views,
           sizeof(ViewSchema) * disk.num_views);
    return true;
}

bool multitable_catalog_save(Table* table, const char* database_filename) {
    char main_path[768];
    char wal_path[768];
    if (!build_catalog_path(main_path, sizeof(main_path), database_filename, ".schema") ||
        !build_catalog_path(wal_path, sizeof(wal_path), database_filename, ".schema.wal")) {
        printf("Database filename too long for schema catalog.\n");
        return false;
    }

    SchemaCatalogDisk disk;
    fill_schema_catalog_disk(table, &disk);

    FILE* wal = fopen(wal_path, "wb");
    if (wal == NULL) {
        printf("Unable to open schema catalog WAL.\n");
        return false;
    }
    uint32_t commit_magic = SCHEMA_CATALOG_WAL_COMMIT_MAGIC;
    bool ok = write_schema_catalog_payload(wal, &disk) &&
              fwrite(&commit_magic, sizeof(commit_magic), 1, wal) == 1;
    if (ok) ok = sync_catalog_file(wal);
    if (fclose(wal) != 0) ok = false;
    if (!ok) {
        printf("Unable to write schema catalog WAL.\n");
        return false;
    }

    if (!write_schema_catalog_file(main_path, &disk)) {
        printf("Unable to write schema catalog.\n");
        return false;
    }
    if (remove(wal_path) != 0 && errno != ENOENT) {
        printf("Unable to remove committed schema catalog WAL.\n");
        return false;
    }
    return true;
}

static int ci_char(int ch) {
    return tolower((unsigned char)ch);
}

static bool ci_equal(const char* left, const char* right) {
    while (*left != '\0' && *right != '\0') {
        if (ci_char(*left) != ci_char(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static const char* skip_spaces_local(const char* input) {
    while (isspace((unsigned char)*input)) input++;
    return input;
}

static bool consume_word_local(const char** input, const char* word) {
    const char* p = skip_spaces_local(*input);
    const char* w = word;
    while (*w != '\0' && ci_char(*p) == ci_char(*w)) {
        p++;
        w++;
    }
    if (*w != '\0') return false;
    if (isalnum((unsigned char)*p) || *p == '_') return false;
    *input = p;
    return true;
}

static bool parse_identifier_local(const char** input, char* output, size_t output_size) {
    const char* p = skip_spaces_local(*input);
    if (!isalpha((unsigned char)*p) && *p != '_') return false;
    const char* start = p;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    size_t length = (size_t)(p - start);
    if (length == 0 || length >= output_size) return false;
    memcpy(output, start, length);
    output[length] = '\0';
    *input = p;
    return true;
}

static bool extract_sql_target(const char* sql,
                               StatementType type,
                               char* output,
                               size_t output_size) {
    output[0] = '\0';
    const char* p = sql;

    switch (type) {
        case STATEMENT_INSERT:
            if (!consume_word_local(&p, "insert")) return false;
            if (!consume_word_local(&p, "into")) {
                strncpy(output, "users", output_size - 1);
                output[output_size - 1] = '\0';
                return true;
            }
            return parse_identifier_local(&p, output, output_size);
        case STATEMENT_DELETE:
            if (!consume_word_local(&p, "delete") ||
                !consume_word_local(&p, "from")) return false;
            return parse_identifier_local(&p, output, output_size);
        case STATEMENT_UPDATE:
            if (!consume_word_local(&p, "update")) return false;
            return parse_identifier_local(&p, output, output_size);
        default:
            return false;
    }
}

static TableSchema* find_schema_exact(Table* table, const char* name) {
    if (name == NULL || name[0] == '\0') return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (ci_equal(table->catalog.schemas[i].name, name)) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static ViewSchema* find_view_exact(Table* table, const char* name) {
    if (name == NULL || name[0] == '\0') return NULL;
    for (uint32_t i = 0; i < table->catalog.num_views; i++) {
        if (ci_equal(table->catalog.views[i].name, name)) {
            return &table->catalog.views[i];
        }
    }
    return NULL;
}

static TableSchema* resolve_schema_name(Table* table, const char* name, int depth) {
    if (depth > 4) return NULL;
    TableSchema* schema = find_schema_exact(table, name);
    if (schema != NULL) return schema;

    ViewSchema* view = find_view_exact(table, name);
    if (view == NULL) return NULL;

    Statement view_statement;
    memset(&view_statement, 0, sizeof(view_statement));
    if (prepare_statement(view->select_sql, &view_statement) != PREPARE_SUCCESS ||
        view_statement.type != STATEMENT_SELECT) {
        return NULL;
    }
    const char* nested_name = view_statement.table_name[0] != '\0'
        ? view_statement.table_name
        : view_statement.select.table_name;
    return resolve_schema_name(table, nested_name, depth + 1);
}

static bool schema_is_row_compatible(const TableSchema* schema) {
    return schema != NULL &&
           schema->num_columns == 3 &&
           ci_equal(schema->columns[0].name, "id") &&
           ci_equal(schema->columns[1].name, "username") &&
           ci_equal(schema->columns[2].name, "email") &&
           schema->columns[0].type == COL_TYPE_INT &&
           schema->columns[1].type == COL_TYPE_VARCHAR &&
           schema->columns[2].type == COL_TYPE_VARCHAR;
}

static bool nested_name_uses_other_or_unknown_root(Table* table,
                                                    const char* name,
                                                    uint32_t root_page_num) {
    if (name == NULL || name[0] == '\0') return true;
    TableSchema* schema = resolve_schema_name(table, name, 0);
    return schema == NULL || schema->root_page_num != root_page_num;
}

static bool nested_select_sql_uses_other_or_unknown_root(Table* table,
                                                         const char* sql,
                                                         uint32_t root_page_num) {
    if (sql == NULL || sql[0] == '\0') return true;
    Statement nested_statement;
    memset(&nested_statement, 0, sizeof(nested_statement));
    if (prepare_statement(sql, &nested_statement) != PREPARE_SUCCESS ||
        nested_statement.type != STATEMENT_SELECT) {
        return true;
    }
    const char* nested = nested_statement.table_name[0] != '\0'
        ? nested_statement.table_name
        : nested_statement.select.table_name;
    return nested_name_uses_other_or_unknown_root(table, nested, root_page_num);
}

static bool select_uses_other_or_unknown_root(Table* table,
                                              const SelectStatement* sel,
                                              uint32_t root_page_num) {
    if (sel->has_join &&
        nested_name_uses_other_or_unknown_root(table, sel->join_table, root_page_num)) {
        return true;
    }

    if (sel->has_in_subquery && sel->in_subquery != NULL) {
        const char* nested = sel->in_subquery->table_name;
        if (nested_name_uses_other_or_unknown_root(table, nested, root_page_num)) {
            return true;
        }
    }

    if (sel->has_exists_subquery && sel->exists_subquery != NULL) {
        const char* nested = sel->exists_subquery->table_name;
        if (nested_name_uses_other_or_unknown_root(table, nested, root_page_num)) {
            return true;
        }
    }

    if (sel->has_scalar_subquery &&
        nested_select_sql_uses_other_or_unknown_root(table,
                                                     sel->scalar_subquery_sql,
                                                     root_page_num)) {
        return true;
    }

    if (sel->is_union && sel->union_second_select[0] != '\0' &&
        nested_select_sql_uses_other_or_unknown_root(table,
                                                     sel->union_second_select,
                                                     root_page_num)) {
        return true;
    }

    return false;
}

static MultiTableRouteResult resolve_statement_schema(Table* table,
                                                       Statement* statement,
                                                       const char* sql,
                                                       TableSchema** schema_out) {
    char target[MAX_NAME_SIZE];
    target[0] = '\0';

    if (statement->type == STATEMENT_SELECT) {
        if (statement->select.sys_func != SYS_FUNC_NONE ||
            statement->select.is_catalog_query) {
            return MULTITABLE_ROUTE_NOT_APPLICABLE;
        }
        const char* name = statement->table_name[0] != '\0'
            ? statement->table_name
            : statement->select.table_name;
        if (name[0] == '\0') return MULTITABLE_ROUTE_NOT_APPLICABLE;
        strncpy(target, name, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';

        if (statement->has_cte && ci_equal(target, statement->cte_name)) {
            Statement cte_statement;
            memset(&cte_statement, 0, sizeof(cte_statement));
            if (prepare_statement(statement->cte_select_sql, &cte_statement) != PREPARE_SUCCESS ||
                cte_statement.type != STATEMENT_SELECT) {
                return MULTITABLE_ROUTE_UNSUPPORTED_QUERY;
            }
            const char* nested = cte_statement.table_name[0] != '\0'
                ? cte_statement.table_name
                : cte_statement.select.table_name;
            strncpy(target, nested, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';
        }
    } else if (statement->type == STATEMENT_INSERT ||
               statement->type == STATEMENT_DELETE ||
               statement->type == STATEMENT_UPDATE) {
        if (!extract_sql_target(sql, statement->type, target, sizeof(target))) {
            return MULTITABLE_ROUTE_UNSUPPORTED_QUERY;
        }
    } else {
        return MULTITABLE_ROUTE_NOT_APPLICABLE;
    }

    TableSchema* schema = resolve_schema_name(table, target, 0);
    if (schema == NULL) return MULTITABLE_ROUTE_TABLE_NOT_FOUND;
    if (!schema_is_row_compatible(schema)) return MULTITABLE_ROUTE_INCOMPATIBLE_SCHEMA;

    if (statement->type == STATEMENT_SELECT) {
        if (schema->root_page_num != 0 && statement->select.has_match_filter) {
            return MULTITABLE_ROUTE_UNSUPPORTED_QUERY;
        }
        if (select_uses_other_or_unknown_root(table,
                                              &statement->select,
                                              schema->root_page_num)) {
            return MULTITABLE_ROUTE_UNSUPPORTED_QUERY;
        }
    }

    *schema_out = schema;
    return MULTITABLE_ROUTE_OK;
}

MultiTableRouteResult multitable_begin_statement_scope(Table* table,
                                                        Statement* statement,
                                                        const char* sql,
                                                        MultiTableRouteScope* scope) {
    memset(scope, 0, sizeof(*scope));

    TableSchema* schema = NULL;
    MultiTableRouteResult result = resolve_statement_schema(table, statement, sql, &schema);
    if (result != MULTITABLE_ROUTE_OK) return result;

    scope->previous_root_page_num = table->root_page_num;
    scope->previous_username_index_enabled = table->username_index_enabled;
    scope->previous_num_sec_indexes = table->num_sec_indexes;
    scope->active = true;
    strncpy(scope->table_name, schema->name, sizeof(scope->table_name) - 1);
    scope->table_name[sizeof(scope->table_name) - 1] = '\0';

    table->root_page_num = schema->root_page_num;
    if (schema->root_page_num != 0) {
        table->username_index_enabled = false;
        table->num_sec_indexes = 0;
        scope->indexes_suppressed = true;
    }
    return MULTITABLE_ROUTE_OK;
}

void multitable_end_statement_scope(Table* table, MultiTableRouteScope* scope) {
    if (!scope->active) return;
    table->root_page_num = scope->previous_root_page_num;
    if (scope->indexes_suppressed) {
        table->username_index_enabled = scope->previous_username_index_enabled;
        table->num_sec_indexes = scope->previous_num_sec_indexes;
    }
    scope->active = false;
}

static const char* join_column_basename(const char* column) {
    const char* dot = strrchr(column, '.');
    return dot == NULL ? column : dot + 1;
}

static bool simple_primary_key_join_supported(const SelectStatement* sel) {
    return sel->aggregate == AGGREGATE_NONE &&
           !sel->has_project_col &&
           !sel->has_id_filter &&
           !sel->has_id_min_filter &&
           !sel->has_id_max_filter &&
           !sel->has_username_filter &&
           !sel->has_username_like &&
           !sel->has_email_filter &&
           !sel->has_email_like &&
           !sel->has_group_by &&
           !sel->has_having &&
           !sel->has_in_subquery &&
           !sel->has_exists_subquery &&
           !sel->is_distinct &&
           !sel->has_match_filter &&
           !sel->has_window_func &&
           !sel->is_union &&
           !sel->has_is_null_filter &&
           !sel->has_is_not_null_filter &&
           !sel->has_order_by_col &&
           !sel->has_secondary_order_by &&
           !sel->has_in_list &&
           !sel->has_between_filter &&
           !sel->has_scalar_subquery &&
           sel->str_func == STRING_FUNC_NONE &&
           sel->math_func == MATH_FUNC_NONE &&
           !sel->has_order_desc &&
           ci_equal(join_column_basename(sel->join_left_col), "id") &&
           ci_equal(join_column_basename(sel->join_right_col), "id");
}

MultiTableRouteResult multitable_execute_join(Table* table,
                                               Statement* statement,
                                               bool* handled,
                                               ExecuteResult* execute_result) {
    *handled = false;
    *execute_result = EXECUTE_SUCCESS;

    if (statement->type != STATEMENT_SELECT || !statement->select.has_join) {
        return MULTITABLE_ROUTE_NOT_APPLICABLE;
    }

    SelectStatement* sel = &statement->select;
    const char* left_name = statement->table_name[0] != '\0'
        ? statement->table_name
        : sel->table_name;
    TableSchema* left_schema = resolve_schema_name(table, left_name, 0);
    TableSchema* right_schema = resolve_schema_name(table, sel->join_table, 0);

    if (left_schema == NULL || right_schema == NULL) {
        *handled = true;
        return MULTITABLE_ROUTE_TABLE_NOT_FOUND;
    }
    if (!schema_is_row_compatible(left_schema) ||
        !schema_is_row_compatible(right_schema)) {
        *handled = true;
        return MULTITABLE_ROUTE_INCOMPATIBLE_SCHEMA;
    }

    if (left_schema->root_page_num == right_schema->root_page_num) {
        return MULTITABLE_ROUTE_NOT_APPLICABLE;
    }

    *handled = true;
    if (!simple_primary_key_join_supported(sel)) {
        return MULTITABLE_ROUTE_UNSUPPORTED_QUERY;
    }

    uint32_t original_root = table->root_page_num;
    uint32_t emitted = 0;
    uint32_t skipped = 0;

    table->root_page_num = left_schema->root_page_num;
    Cursor* left_cursor = table_start(table);
    while (!left_cursor->end_of_table) {
        Row left_row;
        deserialize_row(cursor_value(left_cursor), &left_row);

        table->root_page_num = right_schema->root_page_num;
        Cursor* right_cursor = table_find(table, left_row.id);
        void* right_node = get_page(table->pager, right_cursor->page_num);
        uint32_t right_cells = *leaf_node_num_cells(right_node);

        if (right_cursor->cell_num < right_cells &&
            *leaf_node_key(right_node, right_cursor->cell_num) == left_row.id) {
            Row right_row;
            deserialize_row(cursor_value(right_cursor), &right_row);
            if (sel->has_offset && skipped < sel->offset) {
                skipped++;
            } else if (!sel->has_limit || emitted < sel->limit) {
                printf("(%u, %s, %s) | (%u, %s, %s)\n",
                       left_row.id,
                       left_row.username,
                       left_row.email,
                       right_row.id,
                       right_row.username,
                       right_row.email);
                emitted++;
            }
        }
        free(right_cursor);

        table->root_page_num = left_schema->root_page_num;
        cursor_advance(left_cursor);
        if (sel->has_limit && emitted >= sel->limit) break;
    }
    free(left_cursor);
    table->root_page_num = original_root;

    *execute_result = EXECUTE_SUCCESS;
    return MULTITABLE_ROUTE_OK;
}

ExecuteResult multitable_execute_delete_all(Statement* statement,
                                             Table* table,
                                             const MultiTableRouteScope* scope) {
    (void)statement;
    while (true) {
        Cursor* cursor = table_start(table);
        if (cursor->end_of_table) {
            free(cursor);
            return EXECUTE_SUCCESS;
        }

        void* node = get_page(table->pager, cursor->page_num);
        uint32_t key = *leaf_node_key(node, cursor->cell_num);
        free(cursor);

        Statement one_delete;
        memset(&one_delete, 0, sizeof(one_delete));
        one_delete.type = STATEMENT_DELETE;
        one_delete.delete_id = key;
        strncpy(one_delete.table_name,
                scope->table_name,
                sizeof(one_delete.table_name) - 1);
        ExecuteResult result = execute_statement(&one_delete, table);
        if (result != EXECUTE_SUCCESS) return result;
    }
}

bool multitable_is_schema_ddl(StatementType type) {
    return type == STATEMENT_CREATE_TABLE ||
           type == STATEMENT_ALTER_TABLE ||
           type == STATEMENT_CREATE_VIEW ||
           type == STATEMENT_DROP_VIEW;
}

bool multitable_index_target_supported(Table* table, const char* table_name) {
    TableSchema* schema = find_schema_exact(table, table_name);
    return schema != NULL && schema->root_page_num == 0;
}

const char* multitable_route_error(MultiTableRouteResult result) {
    switch (result) {
        case MULTITABLE_ROUTE_TABLE_NOT_FOUND:
            return "table or view not found";
        case MULTITABLE_ROUTE_INCOMPATIBLE_SCHEMA:
            return "table uses a schema that is not compatible with the current fixed Row storage layout";
        case MULTITABLE_ROUTE_UNSUPPORTED_QUERY:
            return "query requires a cross-table/index path that is not routed safely yet";
        default:
            return "multi-table routing failed";
    }
}
