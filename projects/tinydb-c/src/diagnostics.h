#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "table.h"
#include "pager_try_pin.h"
#include "leaf_page_access.h"

#include <ctype.h>

#define TINYDB_DIAGNOSTIC_MESSAGE_MAX 256

typedef struct {
    uint32_t root_page_num;
    uint32_t total_rows;
    uint32_t leaf_pages;
    uint32_t internal_pages;
    uint32_t height;
} TinyDBTreeStats;

typedef struct {
    uint32_t table_count;
    uint32_t total_rows;
    uint32_t leaf_pages;
    uint32_t internal_pages;
    uint32_t max_height;
} TinyDBDatabaseTreeStats;

typedef struct {
    TinyDBDatabaseTreeStats aggregate;
    TinyDBTreeStats table_stats[MAX_TABLES];
} TinyDBCatalogTreeStatsSnapshot;

typedef struct {
    uint32_t total_pages;
    uint32_t owned_pages;
    uint32_t free_pages;
    uint32_t orphan_pages;
    uint32_t shared_pages;
} TinyDBPageOwnershipStats;

typedef struct {
    uint32_t table_count;
    TinyDBTreeStats table_stats[MAX_TABLES];
    TinyDBPageOwnershipStats ownership;
} TinyDBCatalogTreeCheck;

const TableSchema* tinydb_find_table_schema(const Table* table, const char* table_name);

bool tinydb_get_tree_stats(Table* table,
                           const char* table_name,
                           TinyDBTreeStats* stats);

/* ABI-additive variant that preserves the detailed non-fatal reason from the
 * tree walker (for example, buffer-pool backpressure or malformed leaf data).
 * The historical bool-only helper above remains available and delegates here. */
bool tinydb_get_tree_stats_diagnostic(Table* table,
                                      const char* table_name,
                                      TinyDBTreeStats* stats,
                                      char* message,
                                      size_t message_size);

/* Externally linked catalog snapshot for diagnostics-aware callers that need
 * aggregate and per-table statistics from one traversal. Accumulation stays
 * private until every root succeeds, so BUSY/corruption/overflow cannot
 * publish a prefix of per-table results or plausible-looking aggregate totals. */
bool tinydb_get_catalog_tree_stats_snapshot(
    Table* table,
    TinyDBCatalogTreeStatsSnapshot* snapshot,
    char* message,
    size_t message_size);

/* Aggregate-only compatibility API backed by the same linked snapshot seam. */
bool tinydb_get_database_tree_stats(
    Table* table,
    TinyDBDatabaseTreeStats* stats,
    char* message,
    size_t message_size);

bool tinydb_check_table_tree(Table* table,
                             const char* table_name,
                             char* message,
                             size_t message_size);

bool tinydb_check_page_ownership(Table* table,
                                 TinyDBPageOwnershipStats* stats,
                                 char* message,
                                 size_t message_size);

/* Catalog-wide tree/statistics validation plus one page-ownership pass. The
 * result remains fail-closed and is published only after both phases pass. */
bool tinydb_check_catalog_trees(
    Table* table,
    TinyDBCatalogTreeCheck* result,
    char* message,
    size_t message_size);

bool tinydb_check_database(Table* table,
                           TinyDBPageOwnershipStats* ownership_stats,
                           char* message,
                           size_t message_size);

/* Keep source-built .page/.btree compatibility routing header-only while the
 * catalog-level diagnostics above are real library symbols. */
#include "diagnostics_inspection_inline.h"

#endif /* DIAGNOSTICS_H */
