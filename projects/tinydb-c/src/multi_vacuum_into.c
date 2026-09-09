#include "engine.h"
#include "leaf_format.h"
#include "multitable.h"
#include "record_payload.h"
#include "slotted_leaf_v2.h"

#include <stdio.h>
#include <string.h>

TinyDBSqlStatus tinydb_execute_sql_create_policy_base(
    TinyDB* database,
    const char* sql,
    TinyDBSqlResult* result);

static void initialize_result(TinyDBSqlResult* result) {
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->status = TINYDB_SQL_SUCCESS;
    result->prepare_result = PREPARE_SUCCESS;
    result->execute_result = EXECUTE_SUCCESS;
    result->route_result = MULTITABLE_ROUTE_NOT_APPLICABLE;
    result->statement_type = STATEMENT_VACUUM;
    result->statement_type_valid = true;
}

static TinyDBSqlStatus fail_result(TinyDBSqlResult* result,
                                   TinyDBSqlStatus status,
                                   const char* message) {
    initialize_result(result);
    if (result != NULL) {
        result->status = status;
        result->executed = false;
        snprintf(result->message, sizeof(result->message), "%s", message);
    }
    return status;
}

static bool file_exists(const char* filename) {
    FILE* file = fopen(filename, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static bool build_sidecar_name(char* output,
                               size_t output_size,
                               const char* database_filename,
                               const char* suffix) {
    int written = snprintf(output,
                           output_size,
                           "%s%s",
                           database_filename,
                           suffix);
    return written >= 0 && (size_t)written < output_size;
}

static bool build_index_sidecar_name(char* output,
                                     size_t output_size,
                                     const char* database_filename,
                                     const char* index_name,
                                     const char* suffix) {
    int written = snprintf(output,
                           output_size,
                           "%s.%s%s",
                           database_filename,
                           index_name,
                           suffix);
    return written >= 0 && (size_t)written < output_size;
}

static bool destination_artifact_exists(const Table* source,
                                        const char* destination) {
    static const char* suffixes[] = {
        "",
        ".wal",
        ".free",
        ".schema",
        ".schema.wal",
        ".catalog",
        ".catalog.wal",
        ".username.idx",
        ".username.idx.wal",
        ".gidx.epoch"
    };

    char path[TINYDB_ENGINE_FILENAME_MAX + MAX_NAME_SIZE + 32];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (!build_sidecar_name(path, sizeof(path), destination, suffixes[i])) {
            return true;
        }
        if (file_exists(path)) return true;
    }

    if (source != NULL) {
        static const char* index_suffixes[] = {".idx", ".idx.wal", ".idx.range"};
        for (uint32_t i = 0; i < source->num_sec_indexes; i++) {
            const GenericSecondaryIndex* index = &source->sec_indexes[i];
            if (!index->enabled || index->name[0] == '\0') continue;
            for (size_t s = 0;
                 s < sizeof(index_suffixes) / sizeof(index_suffixes[0]);
                 s++) {
                if (!build_index_sidecar_name(path,
                                              sizeof(path),
                                              destination,
                                              index->name,
                                              index_suffixes[s])) {
                    return true;
                }
                if (file_exists(path)) return true;
            }
        }
    }
    return false;
}

static void remove_if_present(const char* filename) {
    if (filename == NULL || filename[0] == '\0') return;
    (void)remove(filename);
}

static void cleanup_destination_artifacts(const Table* source,
                                          const char* destination) {
    static const char* suffixes[] = {
        "",
        ".wal",
        ".free",
        ".schema",
        ".schema.wal",
        ".catalog",
        ".catalog.wal",
        ".username.idx",
        ".username.idx.wal",
        ".gidx.epoch"
    };
    char path[TINYDB_ENGINE_FILENAME_MAX + MAX_NAME_SIZE + 32];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (build_sidecar_name(path, sizeof(path), destination, suffixes[i])) {
            remove_if_present(path);
        }
    }

    if (source != NULL) {
        static const char* index_suffixes[] = {".idx", ".idx.wal", ".idx.range"};
        for (uint32_t i = 0; i < source->num_sec_indexes; i++) {
            const GenericSecondaryIndex* index = &source->sec_indexes[i];
            if (!index->enabled || index->name[0] == '\0') continue;
            for (size_t s = 0;
                 s < sizeof(index_suffixes) / sizeof(index_suffixes[0]);
                 s++) {
                if (build_index_sidecar_name(path,
                                              sizeof(path),
                                              destination,
                                              index->name,
                                              index_suffixes[s])) {
                    remove_if_present(path);
                }
            }
        }
    }
}

static TableSchema* find_schema(Table* table, const char* name) {
    if (table == NULL || name == NULL) return NULL;
    for (uint32_t i = 0; i < table->catalog.num_tables; i++) {
        if (strcmp(table->catalog.schemas[i].name, name) == 0) {
            return &table->catalog.schemas[i];
        }
    }
    return NULL;
}

static bool prepare_payload_destination_root(Table* destination,
                                             const TableSchema* schema) {
    if (destination == NULL || destination->pager == NULL || schema == NULL) {
        return false;
    }
    if (schema->row_size <= ROW_SIZE) return true;
    if (schema->root_page_num >= destination->pager->num_pages) return false;

    void* root = get_page(destination->pager, schema->root_page_num);
    if (get_node_type(root) != NODE_LEAF || !is_node_root(root) ||
        *node_parent(root) != 0u) {
        return false;
    }

    TinyDBLeafPageFormat format =
        tinydb_leaf_format_detect_page(root, PAGE_SIZE);
    if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return tinydb_slotted_leaf_v2_validate(root, PAGE_SIZE) &&
               tinydb_slotted_leaf_v2_count(root, PAGE_SIZE) == 0u;
    }
    if (format != TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 ||
        *leaf_node_num_cells(root) != 0u ||
        *leaf_node_next_leaf(root) != 0u ||
        *leaf_node_prev_leaf(root) != 0u) {
        return false;
    }

    unsigned char staged[PAGE_SIZE];
    memset(staged, 0, sizeof(staged));
    if (!tinydb_slotted_leaf_v2_init(staged, sizeof(staged))) return false;
    set_node_root(staged, true);
    *node_parent(staged) = 0u;

    memcpy(root, staged, PAGE_USABLE_SIZE);
    mark_page_dirty(destination->pager, schema->root_page_num);
    return true;
}

static bool create_destination_schema(Table* destination,
                                      const TableSchema* source_schema,
                                      TableSchema** created_schema) {
    if (destination == NULL || source_schema == NULL || created_schema == NULL) {
        return false;
    }

    if (strcmp(source_schema->name, "users") == 0) {
        TableSchema* schema = find_schema(destination, "users");
        if (schema == NULL) return false;
        uint32_t root_page_num = schema->root_page_num;
        *schema = *source_schema;
        schema->root_page_num = root_page_num;
        *created_schema = schema;
        return true;
    }

    char column_names[MAX_COLUMNS_PER_TABLE][32];
    char column_types[MAX_COLUMNS_PER_TABLE][16];
    memset(column_names, 0, sizeof(column_names));
    memset(column_types, 0, sizeof(column_types));

    if (source_schema->num_columns == 0 ||
        source_schema->num_columns > MAX_COLUMNS_PER_TABLE) {
        return false;
    }

    for (uint32_t i = 0; i < source_schema->num_columns; i++) {
        snprintf(column_names[i],
                 sizeof(column_names[i]),
                 "%s",
                 source_schema->columns[i].name);
        snprintf(column_types[i],
                 sizeof(column_types[i]),
                 "%s",
                 source_schema->columns[i].type == COL_TYPE_INT
                     ? "INT"
                     : "VARCHAR");
    }

    if (!table_create_table(destination,
                            source_schema->name,
                            source_schema->num_columns,
                            column_names,
                            column_types,
                            source_schema->has_fk,
                            source_schema->fk_col,
                            source_schema->fk_parent_table,
                            source_schema->fk_parent_col,
                            source_schema->fk_on_delete_cascade)) {
        return false;
    }

    TableSchema* schema = find_schema(destination, source_schema->name);
    if (schema == NULL) return false;
    uint32_t root_page_num = schema->root_page_num;
    *schema = *source_schema;
    schema->root_page_num = root_page_num;
    if (!prepare_payload_destination_root(destination, schema)) return false;
    *created_schema = schema;
    return true;
}

static bool cursor_has_key(Table* table, Cursor* cursor, uint32_t key) {
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t count = *leaf_node_num_cells(node);
    return cursor->cell_num < count &&
           *leaf_node_key(node, cursor->cell_num) == key;
}

static bool insert_raw_record(Table* destination,
                              const TableSchema* schema,
                              uint32_t key,
                              const void* raw_value) {
    uint32_t previous_root = destination->root_page_num;
    destination->root_page_num = schema->root_page_num;

    Cursor* cursor = table_find(destination, key);
    if (cursor == NULL || cursor_has_key(destination, cursor, key)) {
        free(cursor);
        destination->root_page_num = previous_root;
        return false;
    }

    Row carrier;
    memset(&carrier, 0, sizeof(carrier));
    memcpy(&carrier, raw_value, ROW_SIZE);
    leaf_node_insert(cursor, key, &carrier);
    free(cursor);
    destination->root_page_num = previous_root;
    return true;
}

typedef struct {
    Table* destination;
    const TableSchema* destination_schema;
    bool ok;
    char message[TINYDB_RECORD_MESSAGE_MAX];
} PayloadCopyContext;

static bool copy_payload_row(const TableSchema* source_schema,
                             const TinyDBRecordPayload* payload,
                             void* context) {
    (void)source_schema;
    PayloadCopyContext* copy = (PayloadCopyContext*)context;
    if (copy == NULL || !copy->ok) return false;
    if (!tinydb_record_payload_insert(copy->destination,
                                      copy->destination_schema,
                                      payload,
                                      copy->message,
                                      sizeof(copy->message))) {
        copy->ok = false;
        return false;
    }
    return true;
}

static bool copy_schema_payload_rows(Table* source,
                                     const TableSchema* source_schema,
                                     Table* destination,
                                     const TableSchema* destination_schema) {
    PayloadCopyContext copy;
    memset(&copy, 0, sizeof(copy));
    copy.destination = destination;
    copy.destination_schema = destination_schema;
    copy.ok = true;

    bool scan_complete = false;
    char scan_message[TINYDB_RECORD_MESSAGE_MAX];
    (void)tinydb_record_payload_scan(source,
                                     source_schema,
                                     copy_payload_row,
                                     &copy,
                                     &scan_complete,
                                     scan_message,
                                     sizeof(scan_message));
    return copy.ok && scan_complete;
}

static bool copy_schema_rows(Table* source,
                             const TableSchema* source_schema,
                             Table* destination,
                             const TableSchema* destination_schema) {
    if (source_schema->row_size > ROW_SIZE) {
        return copy_schema_payload_rows(source,
                                        source_schema,
                                        destination,
                                        destination_schema);
    }

    uint32_t source_previous_root = source->root_page_num;
    source->root_page_num = source_schema->root_page_num;

    Cursor* cursor = table_start(source);
    if (cursor == NULL) {
        source->root_page_num = source_previous_root;
        return false;
    }

    bool ok = true;
    while (!cursor->end_of_table) {
        void* node = get_page(source->pager, cursor->page_num);
        uint32_t key = *leaf_node_key(node, cursor->cell_num);
        unsigned char raw_value[ROW_SIZE];
        memcpy(raw_value, cursor_value(cursor), ROW_SIZE);
        if (!insert_raw_record(destination,
                               destination_schema,
                               key,
                               raw_value)) {
            ok = false;
            break;
        }
        cursor_advance(cursor);
    }

    free(cursor);
    source->root_page_num = source_previous_root;
    return ok;
}

static bool rebuild_views(const Table* source, Table* destination) {
    for (uint32_t i = 0; i < source->catalog.num_views; i++) {
        const ViewSchema* view = &source->catalog.views[i];
        if (!table_create_view(destination, view->name, view->select_sql)) {
            return false;
        }
    }
    return true;
}

static bool rebuild_indexes(Table* source, Table* destination) {
    if (source->username_index_enabled) {
        table_create_username_index(destination);
    }

    for (uint32_t i = 0; i < source->num_sec_indexes; i++) {
        const GenericSecondaryIndex* index = &source->sec_indexes[i];
        if (!index->enabled) continue;
        if (source->username_index_enabled &&
            strcmp(index->name, "idx_users_username") == 0) {
            continue;
        }
        const char* second = index->num_columns == 2
            ? index->column_name2
            : NULL;
        if (!table_create_index(destination,
                                index->name,
                                index->table_name,
                                index->column_name,
                                second)) {
            return false;
        }
    }
    return true;
}

static bool logical_vacuum_into(TinyDB* source_database,
                                const char* destination_filename,
                                char* message,
                                size_t message_size) {
    Table* source = source_database->table;
    if (strcmp(source_database->filename, destination_filename) == 0) {
        snprintf(message,
                 message_size,
                 "%s",
                 "VACUUM INTO destination must differ from the source database");
        return false;
    }
    if (destination_artifact_exists(source, destination_filename)) {
        snprintf(message,
                 message_size,
                 "%s",
                 "VACUUM INTO destination or one of its sidecars already exists");
        return false;
    }

    TinyDB* destination_database = tinydb_open(destination_filename);
    if (destination_database == NULL) {
        snprintf(message,
                 message_size,
                 "%s",
                 "unable to create VACUUM INTO destination database");
        return false;
    }

    Table* destination = destination_database->table;
    bool ok = true;
    uint32_t user_version = db_get_user_version(source);

    for (uint32_t i = 0; i < source->catalog.num_tables && ok; i++) {
        const TableSchema* source_schema = &source->catalog.schemas[i];
        TableSchema* destination_schema = NULL;
        ok = create_destination_schema(destination,
                                       source_schema,
                                       &destination_schema);
        if (ok) {
            ok = copy_schema_rows(source,
                                  source_schema,
                                  destination,
                                  destination_schema);
        }
    }

    if (ok) ok = rebuild_views(source, destination);
    if (ok) ok = rebuild_indexes(source, destination);
    if (ok) {
        db_set_user_version(destination, user_version);
        pager_commit(destination->pager);
        pager_checkpoint(destination->pager);
        ok = multitable_catalog_save(destination, destination_filename);
    }

    tinydb_close(destination_database);

    if (!ok) {
        cleanup_destination_artifacts(source, destination_filename);
        snprintf(message,
                 message_size,
                 "%s",
                 "multi-table VACUUM INTO logical rebuild failed");
        return false;
    }

    snprintf(message, message_size, "%s", "ok");
    return true;
}

TinyDBSqlStatus tinydb_execute_sql(TinyDB* database,
                                   const char* sql,
                                   TinyDBSqlResult* result) {
    if (database == NULL || database->table == NULL || sql == NULL) {
        return tinydb_execute_sql_create_policy_base(database, sql, result);
    }

    Statement statement;
    memset(&statement, 0, sizeof(statement));
    if (prepare_statement(sql, &statement) != PREPARE_SUCCESS ||
        statement.type != STATEMENT_VACUUM ||
        !statement.vacuum.has_into ||
        database->table->catalog.num_tables <= 1) {
        return tinydb_execute_sql_create_policy_base(database, sql, result);
    }

    if (database->table->in_transaction) {
        return fail_result(result,
                           TINYDB_SQL_POLICY_ERROR,
                           "VACUUM INTO is not allowed inside a transaction");
    }

    char message[TINYDB_ENGINE_MESSAGE_MAX];
    if (!logical_vacuum_into(database,
                             statement.vacuum.into_filename,
                             message,
                             sizeof(message))) {
        return fail_result(result, TINYDB_SQL_EXECUTE_ERROR, message);
    }

    initialize_result(result);
    if (result != NULL) {
        result->status = TINYDB_SQL_SUCCESS;
        result->executed = true;
        result->execute_result = EXECUTE_SUCCESS;
    }
    printf("Database backed up to '%s'.\n", statement.vacuum.into_filename);
    return TINYDB_SQL_SUCCESS;
}
