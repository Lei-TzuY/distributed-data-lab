#ifndef LEAF_PAGE_ACCESS_H
#define LEAF_PAGE_ACCESS_H

#include "leaf_format.h"

#include <stddef.h>

/* Read-only, format-aware access to production leaf pages.
 *
 * FIXED_V1 keeps the historical native-layout cell representation.
 * SLOTTED_V2 uses the validated slot directory and variable-length payloads.
 * Mutation intentionally remains outside this seam until V2 split/delete/WAL
 * semantics are ready for production use. */
bool tinydb_leaf_page_count(const void* page,
                            size_t page_capacity,
                            uint32_t* count);

bool tinydb_leaf_page_key_at(const void* page,
                             size_t page_capacity,
                             uint32_t cell_index,
                             uint32_t* key);

bool tinydb_leaf_page_value_at(const void* page,
                               size_t page_capacity,
                               uint32_t cell_index,
                               const void** value,
                               uint32_t* value_length);

bool tinydb_leaf_page_lower_bound(const void* page,
                                  size_t page_capacity,
                                  uint32_t key,
                                  uint32_t* cell_index,
                                  bool* exact_match);

bool tinydb_leaf_page_next(const void* page,
                           size_t page_capacity,
                           uint32_t* page_num);

bool tinydb_leaf_page_prev(const void* page,
                           size_t page_capacity,
                           uint32_t* page_num);

bool tinydb_leaf_page_is_fixed_v1(const void* page, size_t page_capacity);

#endif /* LEAF_PAGE_ACCESS_H */
