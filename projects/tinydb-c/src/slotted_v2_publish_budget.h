#ifndef TINYDB_SLOTTED_V2_PUBLISH_BUDGET_H
#define TINYDB_SLOTTED_V2_PUBLISH_BUDGET_H

#include "pager.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Recursive V2 mutations may stage leaf siblings plus several internal
 * ancestors in one publication boundary. Bound rollback memory by bytes rather
 * than by an arbitrary tree-height/page-count constant.
 *
 * The default is intentionally conservative and may be overridden at build
 * time for workloads that need unusually deep atomic publication. Rollback
 * images are heap-backed, so increasing this budget does not enlarge stack
 * frames.
 */
#ifndef TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES
#define TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES (4u * 1024u * 1024u)
#endif

#if TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES < PAGE_USABLE_SIZE
#error "TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES must fit at least one page"
#endif

/* Compatibility/inspection value derived from the byte budget, not a tree cap. */
#define TINYDB_V2_PUBLISH_BATCH_MAX_PAGES \
    (TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES / PAGE_USABLE_SIZE)
#define TINYDB_V2_PUBLISH_NO_FAIL UINT32_MAX

/*
 * Validate rollback allocation size before any publication entry is
 * dereferenced. Keeping this independent of either pointer- or Pager-based
 * publication lets both implementations share exactly the same memory bound.
 */
static bool tinydb_v2_publish_batch_snapshot_size(
    uint32_t count,
    size_t* snapshot_bytes_out) {
    if (snapshot_bytes_out != NULL) *snapshot_bytes_out = 0u;
    if (count == 0u) return false;
#if SIZE_MAX < UINT32_MAX
    if (PAGE_USABLE_SIZE != 0u &&
        (size_t)count > SIZE_MAX / (size_t)PAGE_USABLE_SIZE) {
        return false;
    }
#endif

    size_t snapshot_bytes = (size_t)count * (size_t)PAGE_USABLE_SIZE;
    if (snapshot_bytes >
        (size_t)TINYDB_V2_PUBLISH_BATCH_MAX_ROLLBACK_BYTES) {
        return false;
    }
    if (snapshot_bytes_out != NULL) *snapshot_bytes_out = snapshot_bytes;
    return true;
}

#endif
