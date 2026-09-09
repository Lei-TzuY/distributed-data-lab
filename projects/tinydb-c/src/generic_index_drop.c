#include "generic_index_correlation.h"
#include "table.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

bool table_drop_index_legacy(Table* table, const char* index_name);

static bool build_range_filename(Table* table,
                                 const char* index_name,
                                 char* output,
                                 size_t output_size) {
    if (table == NULL || table->pager == NULL || index_name == NULL) return false;
    int written = snprintf(output,
                           output_size,
                           "%s.%s.idx.range",
                           table->pager->filename,
                           index_name);
    return written >= 0 && (size_t)written < output_size;
}

static bool remove_sidecar(const char* filename, const char* description) {
    if (filename == NULL) return true;
    if (remove(filename) == 0 || errno == ENOENT) return true;
    printf("Error: unable to remove %s.\n", description);
    return false;
}

bool table_drop_index(Table* table, const char* index_name) {
    char range_filename[640];
    char stats_filename[672];
    char pair_filenames[MAX_INDEXES][768];
    uint32_t pair_filename_count = 0;

    bool has_range_filename = build_range_filename(table,
                                                    index_name,
                                                    range_filename,
                                                    sizeof(range_filename));
    bool has_stats_filename = false;
    if (has_range_filename) {
        int written = snprintf(stats_filename,
                               sizeof(stats_filename),
                               "%s.stats",
                               range_filename);
        has_stats_filename =
            written >= 0 && (size_t)written < sizeof(stats_filename);
    }

    GenericSecondaryIndex* target = table_find_index_by_name(table, index_name);
    if (target != NULL) {
        for (uint32_t i = 0;
             i < table->num_sec_indexes && pair_filename_count < MAX_INDEXES;
             i++) {
            GenericSecondaryIndex* other = &table->sec_indexes[i];
            if (other == target || !other->enabled || other->num_columns != 1 ||
                strcmp(other->table_name, target->table_name) != 0) {
                continue;
            }
            if (tinydb_generic_index_pair_stats_filename(
                    table,
                    target,
                    other,
                    pair_filenames[pair_filename_count],
                    sizeof(pair_filenames[pair_filename_count]))) {
                pair_filename_count++;
            }
        }
    }

    bool dropped = table_drop_index_legacy(table, index_name);
    if (!dropped) return false;

    if (has_range_filename &&
        !remove_sidecar(range_filename, "generic range index snapshot")) {
        return false;
    }
    if (has_stats_filename &&
        !remove_sidecar(stats_filename, "generic index optimizer statistics")) {
        return false;
    }
    for (uint32_t i = 0; i < pair_filename_count; i++) {
        if (!remove_sidecar(pair_filenames[i],
                            "generic pairwise correlation statistics")) {
            return false;
        }
    }
    return true;
}
