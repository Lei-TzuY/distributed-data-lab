#ifndef LEAF_FORMAT_H
#define LEAF_FORMAT_H

#include "table.h"

#include <stddef.h>

#define TINYDB_LEAF_FORMAT_FIXED_V1 1u

typedef struct {
    uint32_t format_version;
    bool variable_length_values;
    uint32_t page_size;
    uint32_t usable_size;
    uint32_t header_size;
    uint32_t key_size;
    uint32_t value_capacity;
    uint32_t cell_size;
    uint32_t max_cells;
} TinyDBLeafFormatDescriptor;

typedef enum {
    TINYDB_LEAF_PAGE_FORMAT_UNKNOWN = 0,
    TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 = 1,
    TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 = 2
} TinyDBLeafPageFormat;

/* Current on-disk leaf format. V1 is intentionally a descriptor around the
 * historical fixed-cell layout; it does not change any page bytes. A future
 * slotted-page V2 can switch the descriptor and storage implementation behind
 * the leaf_value seam without changing schema-aware record encoding. */
const TinyDBLeafFormatDescriptor* tinydb_leaf_format_current(void);

bool tinydb_leaf_format_validate_current(void);
bool tinydb_leaf_format_can_store_value(uint32_t length);

/* V2 deliberately places its 32-bit magic at the byte offset occupied by the
 * V1 leaf num_cells field. The chosen marker cannot equal any valid V1 cell
 * count, so a valid legacy page and a valid V2 page are unambiguous without
 * changing the common B+ tree header prefix. Detection is read-only; V2 is not
 * yet enabled for production Pager/B+ tree pages. */
bool tinydb_leaf_format_v2_marker_disjoint_from_v1(void);
TinyDBLeafPageFormat tinydb_leaf_format_detect_page(const void* page,
                                                    size_t page_capacity);

#endif /* LEAF_FORMAT_H */
